#include "engine/melee.h"

#include "engine/hands.h"
#include "engine/player.h"
#include "input/actions.h"
#include "util/log.h"
#include "util/pattern.h"

#include <MinHook.h>
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace engine::melee {
namespace {

// Mise à jour de mêlée d'un personnage, une fois par image de simulation : void __cdecl(personnage)
constexpr char kUpdateSig[] = "55 8B EC 83 E4 F8 81 EC B4 00 00 00 33 C0 53 8B 5D 08 56 57 8B F3 89 83 04 0B 00 00";
// Balayage d'une attaque : nombre de points dans eax, personnage sur la pile (nettoyée par
// l'appelant). Lit l'attaque en cours (personnage+0xB25) et, pour chaque point, sa position
// précédente (+0xBBC) et courante (+0xBD4).
constexpr char kSweepSig[] = "81 EC 88 00 00 00 53 55 8B AC 24 94 00 00 00 56 8B F0 0F B6 85 25 0B 00 00 6B C0 74 03 05";
constexpr size_t kSweepTableOperand = 0x1E; // adresse du pointeur de la table des attaques
// Choix de l'attaque : personnage dans esi, genre dans ecx (0 principale), cible sur la pile
// (nettoyée par l'appelant), numéro de l'attaque dans al (0xFF : aucune)
constexpr char kPickSig[] = "83 EC 08 83 F9 02 7C 06 0C FF 83 C4 08 C3 8B 86 CC 10 00 00";

// Personnage
constexpr size_t kAttack = 0xB25;       // attaque en cours (octet, 0xFF : aucune)
constexpr size_t kAttackWeapon = 0xB28; // arme de l'attaque : si c'est l'arme en main et qu'elle a une forme, elle est balayée
constexpr size_t kBounce = 0xB5C;       // négatif : un coup dans le décor fait rebondir l'animation de l'attaque
constexpr size_t kHitPos = 0xB60;       // point d'impact et distance (16 octets)
constexpr size_t kFlags = 0xB70;        // bit 0x40 décor touché à ce balayage, 0x10 effet du décor déjà joué
constexpr size_t kHitCount = 0xB78;     // objets déjà touchés par cette attaque (16 au plus)
constexpr size_t kHitList = 0xB7C;
constexpr size_t kPrev = 0xBBC;         // positions précédentes des deux points (24 octets)
constexpr size_t kCur = 0xBD4;          // positions courantes
constexpr size_t kHeld = 0x10CC;        // objet tenu en main
constexpr int kMaxHits = 16;
constexpr uint32_t kFlagWorld = 0x40, kFlagWorldDone = 0x10;
// Objet tenu
constexpr size_t kItemPos = 0x14;
constexpr size_t kItemSlot = 0xC4;
constexpr size_t kItemShape = 0xC8;
constexpr size_t kItemFlags = 0xCC;
// Table des attaques (melee.xtbl) : pointeur, puis nombre ; 0x74 octets par attaque
constexpr size_t kEntrySize = 0x74;
constexpr size_t kEntryRadius = 0x1C;
constexpr size_t kEntryFlags = 0x48;
constexpr uint32_t kEntryCarried = 0x400; // attaque portée avec un objet ramassé : pas la nôtre

using UpdateFn = void(__cdecl*)(uint8_t*);
UpdateFn g_update = nullptr;
void* g_updateTarget = nullptr;
void* g_sweep = nullptr;
void* g_pick = nullptr;
uint8_t** g_table = nullptr; // pointeur de la table, suivi de son nombre d'entrées

bool g_active = false;
float g_startSpeed = 1.8f;
bool g_faulted = false;

struct Hand {
    bool swinging = false;
    bool havePrev = false;
    bool weaponPoint = false; // le point suivi est l'arme, pas le poing
    float prev[3]{};
    ULONGLONG tick = 0;
    int hitCount = 0;
    uint32_t hits[kMaxHits]{};
    bool worldDone = false;
    int swingHits = 0;
    float swingPeak = 0.0f;
    int attack = 0xFF;
};
Hand g_hands[2];
Stats g_stats{};
unsigned g_logged = 0;
constexpr unsigned kMaxLogged = 40;

// État du personnage emprunté le temps d'un balayage, rendu ensuite tel quel
struct Saved {
    bool pending = false;
    uint8_t* human = nullptr;
    uint8_t attack = 0xFF;
    uint32_t weapon = 0;
    int32_t bounce = 0;
    uint8_t hitPos[16]{};
    uint32_t flags = 0;
    int32_t hitCount = 0;
    uint32_t hitList[kMaxHits]{};
    uint8_t points[48]{};
};
Saved g_saved;

void Save(uint8_t* human)
{
    Saved& s = g_saved;
    s.human = human;
    s.attack = human[kAttack];
    memcpy(&s.weapon, human + kAttackWeapon, sizeof(s.weapon));
    memcpy(&s.bounce, human + kBounce, sizeof(s.bounce));
    memcpy(s.hitPos, human + kHitPos, sizeof(s.hitPos));
    memcpy(&s.flags, human + kFlags, sizeof(s.flags));
    memcpy(&s.hitCount, human + kHitCount, sizeof(s.hitCount));
    memcpy(s.hitList, human + kHitList, sizeof(s.hitList));
    memcpy(s.points, human + kPrev, sizeof(s.points));
    s.pending = true;
}

void Restore()
{
    Saved& s = g_saved;
    if (!s.pending)
        return;
    uint8_t* human = s.human;
    human[kAttack] = s.attack;
    memcpy(human + kAttackWeapon, &s.weapon, sizeof(s.weapon));
    memcpy(human + kBounce, &s.bounce, sizeof(s.bounce));
    memcpy(human + kHitPos, s.hitPos, sizeof(s.hitPos));
    memcpy(human + kFlags, &s.flags, sizeof(s.flags));
    memcpy(human + kHitCount, &s.hitCount, sizeof(s.hitCount));
    memcpy(human + kHitList, s.hitList, sizeof(s.hitList));
    memcpy(human + kPrev, s.points, sizeof(s.points));
    s.pending = false;
}

#if defined(_MSC_VER) && !defined(__clang__)
void CallSweep(void* fn, int count, uint8_t* human)
{
    __asm {
        mov eax, count
        push human
        call fn
        add esp, 4
    }
}

int CallPick(void* fn, uint8_t* human, int kind)
{
    int result = 0xFF;
    __asm {
        push esi
        mov esi, human
        mov ecx, kind
        push 0
        call fn
        add esp, 4
        pop esi
        movzx eax, al
        mov result, eax
    }
    return result;
}
#else
// Compilation de contrôle hors MSVC : pas d'appel (la DLL se compile avec MSVC)
void CallSweep(void*, int, uint8_t*) {}
int CallPick(void*, uint8_t*, int) { return 0xFF; }
#endif

// Balaie le trajet d'un point avec l'attaque donnée. Renvoie le nombre d'objets nouvellement
// touchés ; *world : le décor a été touché.
int Sweep(uint8_t* human, Hand& hand, int attack, uint8_t* weapon, const float from[3], const float to[3], bool* world)
{
    Save(human);
    human[kAttack] = static_cast<uint8_t>(attack);
    const uint32_t weaponValue = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(weapon));
    memcpy(human + kAttackWeapon, &weaponValue, sizeof(weaponValue));
    const int32_t noBounce = 0;
    memcpy(human + kBounce, &noBounce, sizeof(noBounce));
    const uint32_t flags = (g_saved.flags & ~(kFlagWorld | kFlagWorldDone)) | (hand.worldDone ? kFlagWorldDone : 0);
    memcpy(human + kFlags, &flags, sizeof(flags));
    const int32_t count = hand.hitCount;
    memcpy(human + kHitCount, &count, sizeof(count));
    memcpy(human + kHitList, hand.hits, sizeof(hand.hits));
    memcpy(human + kPrev, from, 3 * sizeof(float));
    memcpy(human + kCur, to, 3 * sizeof(float));

    CallSweep(g_sweep, 1, human);

    int32_t after = 0;
    uint32_t flagsAfter = 0;
    memcpy(&after, human + kHitCount, sizeof(after));
    memcpy(&flagsAfter, human + kFlags, sizeof(flagsAfter));
    if (after < hand.hitCount || after > kMaxHits)
        after = hand.hitCount;
    const int fresh = after - hand.hitCount;
    memcpy(hand.hits, human + kHitList, sizeof(hand.hits));
    hand.hitCount = after;
    *world = (flagsAfter & kFlagWorld) != 0 && !hand.worldDone;
    if (flagsAfter & kFlagWorldDone)
        hand.worldDone = true;
    Restore();
    return fresh;
}

void Update(uint8_t* human)
{
    const ULONGLONG now = GetTickCount64();
    const bool attacking = human[kAttack] != 0xFF; // attaque animée du jeu en cours : on ne s'en mêle pas
    const int weaponAttack = player::HeldMeleeAttack(); // -1 mains vides, 0xFF arme à feu
    uint8_t* item = *reinterpret_cast<uint8_t**>(human + kHeld);
    const uint32_t entries = g_table && *g_table ? *reinterpret_cast<const uint32_t*>(g_table + 1) : 0;
    int unarmed = -1; // attaque à mains nues, demandée au jeu une fois par image au besoin

    for (int index = 0; index < 2; ++index) {
        Hand& hand = g_hands[index];
        const float speed = input::actions::TakeHandSpeed(index);
        float point[3];
        // Ce que frappe cette main : la droite avec l'arme blanche tenue, sinon son poing ; rien
        // si elle tient une arme à feu, ni pour la gauche quand la droite est armée
        const bool weaponHand = index == 1 && weaponAttack >= 0 && weaponAttack != 0xFF && item;
        const bool fistHand = weaponAttack < 0;
        const bool shaped = weaponHand && *reinterpret_cast<const uint32_t*>(item + kItemShape) &&
                            *reinterpret_cast<const uint32_t*>(item + kItemSlot) && (item[kItemFlags] & 1);
        bool located = (weaponHand || fistHand) && speed >= 0.0f && hands::Fist(index, point);
        if (located && shaped)
            memcpy(point, item + kItemPos, sizeof(point)); // l'arme elle-même : le jeu balaie sa forme
        if (located && !(std::isfinite(point[0]) && std::isfinite(point[1]) && std::isfinite(point[2])))
            located = false;
        if (!located || !g_active) {
            hand.swinging = false;
            hand.havePrev = false;
            continue;
        }
        if (speed > g_stats.fastest)
            g_stats.fastest = speed;
        if (hand.havePrev && (now - hand.tick > 250 || hand.weaponPoint != shaped))
            hand.havePrev = false;

        // Début et fin du geste
        if (!hand.swinging) {
            if (speed >= g_startSpeed && hand.havePrev && !attacking) {
                int attack = weaponHand ? weaponAttack : unarmed;
                if (!weaponHand && unarmed < 0)
                    attack = unarmed = CallPick(g_pick, human, 0);
                bool usable = attack >= 0 && attack != 0xFF && static_cast<uint32_t>(attack) < entries;
                if (usable) {
                    const uint8_t* entry = *g_table + static_cast<size_t>(attack) * kEntrySize;
                    usable = !(*reinterpret_cast<const uint32_t*>(entry + kEntryFlags) & kEntryCarried);
                }
                if (usable) {
                    hand.swinging = true;
                    hand.attack = attack;
                    hand.hitCount = 0;
                    hand.worldDone = false;
                    hand.swingHits = 0;
                    hand.swingPeak = speed;
                    ++g_stats.swings[index];
                }
            }
        } else if (speed < 0.5f * g_startSpeed || attacking) {
            if (g_logged < kMaxLogged && !hand.swingHits) {
                ++g_logged;
                LOG_ENGINE("melee : geste de la main %s (%s, attaque %d, jusqu'a %.1f m/s) : rien touche",
                           index ? "droite" : "gauche", hand.weaponPoint ? "arme" : "poing", hand.attack, hand.swingPeak);
            }
            hand.swinging = false;
        }

        if (hand.swinging) {
            if (speed > hand.swingPeak)
                hand.swingPeak = speed;
            const float dx = point[0] - hand.prev[0], dy = point[1] - hand.prev[1], dz = point[2] - hand.prev[2];
            if (dx * dx + dy * dy + dz * dz < 1.5f * 1.5f) { // au-delà : téléportation, rien à balayer
                bool world = false;
                const int fresh = Sweep(human, hand, hand.attack, shaped ? item : nullptr, hand.prev, point, &world);
                if (fresh > 0) {
                    hand.swingHits += fresh;
                    g_stats.hits[index] += static_cast<unsigned>(fresh);
                    input::actions::RequestHaptic(index, 1.0f, 90);
                    if (g_logged < kMaxLogged) {
                        ++g_logged;
                        const uint8_t* entry = *g_table + static_cast<size_t>(hand.attack) * kEntrySize;
                        LOG_ENGINE("melee : coup de la main %s (%s, attaque %d de rayon %.2f m, %.1f m/s) : %d touche(s)",
                                   index ? "droite" : "gauche", shaped ? "forme de l'arme" : weaponHand ? "arme" : "poing",
                                   hand.attack, *reinterpret_cast<const float*>(entry + kEntryRadius), speed, fresh);
                    }
                } else if (world) {
                    ++g_stats.world[index];
                    input::actions::RequestHaptic(index, 0.45f, 50);
                }
            }
        }
        memcpy(hand.prev, point, sizeof(point));
        hand.havePrev = true;
        hand.weaponPoint = shaped;
        hand.tick = now;
    }
}

void UpdateGuarded(uint8_t* human)
{
#if defined(_MSC_VER) && !defined(__clang__)
    __try {
        Update(human);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Restore();
        g_faulted = true;
        LOG_ENGINE("melee : ERREUR pendant un coup au geste, coups au geste coupes pour cette partie");
    }
#else
    Update(human);
#endif
}

void __cdecl HkUpdate(uint8_t* human)
{
    g_update(human);
    if (human && !g_faulted && player::IsLocal(human))
        UpdateGuarded(human);
}

} // namespace

bool Install()
{
    auto* update = static_cast<uint8_t*>(pattern::Find("mise a jour de melee", kUpdateSig));
    auto* sweep = static_cast<uint8_t*>(pattern::Find("balayage d'une attaque de melee", kSweepSig));
    g_pick = pattern::Find("choix de l'attaque de melee", kPickSig);
    if (!update || !sweep || !g_pick) {
        LOG_ENGINE("melee : fonctions du jeu introuvables, pas de coups au geste");
        return false;
    }
    uintptr_t table = 0;
    memcpy(&table, sweep + kSweepTableOperand, sizeof(table));
    g_table = reinterpret_cast<uint8_t**>(table);
    g_sweep = sweep;
    const MH_STATUS st = MH_CreateHook(update, reinterpret_cast<void*>(&HkUpdate), reinterpret_cast<void**>(&g_update));
    if (st != MH_OK || MH_EnableHook(update) != MH_OK) {
        LOG_ENGINE("melee : ECHEC du detour de la mise a jour de melee (%s)", MH_StatusToString(st));
        return false;
    }
    g_updateTarget = update;
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    LOG_ENGINE("melee : mise a jour exe+0x%X detournee, balayage exe+0x%X, table des attaques en 0x%08X : coups au geste "
               "(poings, armes blanches)",
               static_cast<unsigned>(reinterpret_cast<uintptr_t>(update) - base),
               static_cast<unsigned>(reinterpret_cast<uintptr_t>(sweep) - base), static_cast<unsigned>(table));
    return true;
}

void Remove()
{
    g_active = false;
    if (g_updateTarget) {
        MH_DisableHook(g_updateTarget);
        MH_RemoveHook(g_updateTarget);
        g_updateTarget = nullptr;
    }
}

void SetFrame(bool active, float startSpeed)
{
    g_startSpeed = startSpeed >= 0.5f && startSpeed <= 10.0f ? startSpeed : 1.8f;
    g_active = active && g_updateTarget != nullptr;
}

void TakeStats(Stats* stats)
{
    *stats = g_stats;
    g_stats = Stats{};
}

} // namespace engine::melee
