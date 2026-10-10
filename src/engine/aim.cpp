#include "engine/aim.h"
#include "engine/controls.h"
#include "engine/player.h"
#include "util/log.h"
#include "util/pattern.h"

#include <MinHook.h>
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace engine::aim {
namespace {

// Point visé par le joueur, __cdecl(joueur, float sortie[3]), résultat = objet visé :
// sub esp,0E4h ; fldz ; push ebx ; mov ebx,[esp+0F0h] ; fst [esp+64h] ; push ebp ; xor ebp,ebp ;
// push esi ; push edi ; mov edi,[esp+0F8h]
constexpr char kAimPointSig[] =
    "81 EC E4 00 00 00 D9 EE 53 8B 9C 24 F0 00 00 00 D9 54 24 64 55 33 ED 56 57 8B BC 24 F8 00 00 00";
// Mise à jour de l'arme d'un personnage, __cdecl(personnage), à chaque image (rechargement,
// armes guidées) : push ebp ; mov ebp,esp ; and esp,-8 ; sub esp,20Ch ; push ebx ;
// mov ebx,[ebp+8] ; test ebx,ebx ; push esi ; push edi ; je ... ; mov eax,[ebx+10CCh]
constexpr char kWeaponUpdateSig[] =
    "55 8B EC 83 E4 F8 81 EC 0C 02 00 00 53 8B 5D 08 85 DB 56 57 74 12 8B 83 CC 10 00 00";
// Tir du joueur à pied, bool __cdecl(int autre main), appelé à chaque image où l'attaque est
// tenue (c'est lui qui appelle la fonction de tir des personnages, celle qui crée la balle) :
// sub esp,8 ; mov ecx,[joueur] ; test ecx,ecx ; push ebx ; push ebp ; push esi ; mov ebx,ecx ;
// push edi ; mov [esp+10h],ebx ; je ... ; mov edx,[ecx+10CCh] ; test edx,edx ; lea edi,[ecx+10CCh]
// Fonction de tir des personnages, bool __cdecl(personnage, cible, 0, 0, autre main, tir
// secondaire, 0) : push ebp ; mov ebp,esp ; and esp,-8 ; sub esp,0A64h ; push ebx ;
// mov ebx,[ebp+8] ; test ebx,ebx ; push esi ; push edi ; je ... ; mov eax,[ebx+10CCh]
constexpr char kCharacterFireSig[] =
    "55 8B EC 83 E4 F8 81 EC 64 0A 00 00 53 8B 5D 08 85 DB 56 57 0F 84 ?? ?? ?? ?? 8B 83 CC 10 00 00";
constexpr char kPlayerFireSig[] = "83 EC 08 8B 0D ?? ?? ?? ?? 85 C9 53 55 56 8B D9 57 89 5C 24 10 74 ?? 8B 91 CC 10 00 00 85 "
                                  "D2 8D B9 CC 10 00 00";
// Direction de visée d'un personnage (sert à lire deux adresses, pas détournée) : sub esp,4Ch ;
// cmp eax,[joueur] ; push ebx ; mov ebx,[esp+54h] ; push esi ; push edi ; mov ecx,9 ;
// lea edi,[esp+34h] ; lea esi,[eax+12ACh] ; jne ... ; mov esi,<orientation idéale> ;
// fld [décalage de tangage]
constexpr char kAimDirSig[] = "83 EC 4C 3B 05 ?? ?? ?? ?? 53 8B 5C 24 54 56 57 B9 09 00 00 00 8D 7C 24 34 8D B0 AC 12 00 "
                              "00 75 05 BE ?? ?? ?? ?? D9 05 ?? ?? ?? ??";
constexpr int kIdealOrientOffset = 34;
constexpr int kPitchOffsetOffset = 40;
// Dans le point visé : le rayon du jeu ne part pas de la caméra mais d'un peu plus loin, pour
// sauter ce qui se trouve entre la caméra à la 3e personne et le personnage. Selon la classe
// de l'arme : à la distance caméra - personnage, ou à une distance fixe (4 m). Aiguillage :
// cmp eax,16h ; ja <loin> ; movzx ecx,[eax+<table>] ; jmp [ecx*4+<sauts>] ; branche « loin » :
// cmp eax,5 ; sete al ; test al,al ; jne <proche> ; fld [<distance fixe>]
constexpr char kStartSwitchSig[] = "83 F8 16 77 ?? 0F B6 88 ?? ?? ?? ?? FF 24 8D ?? ?? ?? ??";
constexpr char kStartFarSig[] = "83 F8 05 0F 94 C0 84 C0 75 ?? D9 05 ?? ?? ?? ??";
constexpr int kStartClassMax = 0x16;
constexpr int kStartNearClass = 5;
// Avec le rayon d'une manette, cette distance fixe ferait partir le rayon 4 m devant la main :
// la caméra présentée au jeu recule d'autant, moins ce petit écart
constexpr float kStartGap = 0.10f;
// Dans la caméra : l'orientation idéale suit l'orientation finale de 0x24 octets, la position
// idéale suit la position finale de 12 octets
constexpr ptrdiff_t kIdealOrientDelta = 0x24;
// Dans le joueur : objet visé et point visé
constexpr size_t kAimTarget = 0x11C8;
constexpr size_t kAimPoint = 0x11CC;
// Dans la case d'inventaire de l'arme en main : sa description, les balles du chargeur
constexpr size_t kSlotAmmo = 0x10;
constexpr ULONGLONG kSightMaxAgeMs = 200;
// Le tir ne recalcule le point visé que si le jeu le calcule lui-même en ce moment
constexpr ULONGLONG kFreeAimMaxAgeMs = 250;

using AimPointFn = int(__cdecl*)(uint8_t* player, float* out);
using WeaponUpdateFn = int(__cdecl*)(uint8_t* human);
using PlayerFireFn = bool(__cdecl*)(int offhand);
using CharacterFireFn = bool(__cdecl*)(uint8_t* human, void* target, int a3, int a4, int offhand, int alt, int a7);
AimPointFn g_aimPoint = nullptr;
WeaponUpdateFn g_weaponUpdate = nullptr;
PlayerFireFn g_playerFire = nullptr;
CharacterFireFn g_characterFire = nullptr;
void* g_aimPointTarget = nullptr;
void* g_weaponUpdateTarget = nullptr;
void* g_playerFireTarget = nullptr;
void* g_characterFireTarget = nullptr;

float* g_camPos = nullptr;
float* g_camOrient = nullptr;
float* g_idealPos = nullptr; // nullptr : disposition inattendue, on n'y touche pas
float* g_idealOrient = nullptr;
float* g_pitchOffset = nullptr;
RayFn g_rayFn = nullptr;

// Départ du rayon du jeu selon la classe de l'arme (nullptr : aiguillage introuvable)
const uint8_t* g_startTable = nullptr;
const uint32_t* g_startJumps = nullptr;
uint32_t g_startFarBranch = 0;
const float* g_startFar = nullptr;

bool g_active = false;
int g_depth = 0;
int g_fireHand = -1;       // main qui tire, le temps de la fonction de tir
int g_playerFireHand = -1; // main demandée par la gâchette, le temps du tir du joueur
int g_lastFireHand = 1;    // dernière main demandée (pour un tir différé)
unsigned g_deferred = 0;   // tirs différés (hors du tir du joueur) depuis le dernier relevé
ULONGLONG g_freeAimTick = 0;

struct Record {
    Sight sight;
    ULONGLONG tick = 0;
};
Record g_sights[3]; // regard, main gauche, main droite
unsigned g_points = 0;
unsigned g_shots[2]{};   // tirs dont le point visé a été recalculé, par main
unsigned g_calls[2]{};   // appels du tir du joueur, par main
unsigned g_bullets[2]{}; // balles parties du chargeur pendant ces appels, par main
unsigned g_scoped[2]{};  // appels faits avec la caméra décrivant le rayon de la main, par main
bool g_announced[3]{};
bool g_shotAnnounced[2]{};
bool g_dualAnnounced = false;

const char* SourceName(int hand)
{
    return hand < 0 ? "du regard" : hand == 0 ? "de la manette gauche" : "de la manette droite";
}

// Le temps d'une fonction du jeu : la caméra décrit notre rayon
class Scope {
public:
    Scope(const float origin[3], const float orient[9])
    {
        ++g_depth;
        memcpy(pos_, g_camPos, sizeof(pos_));
        memcpy(orient_, g_camOrient, sizeof(orient_));
        memcpy(ideal_, g_idealOrient, sizeof(ideal_));
        pitch_ = *g_pitchOffset;
        memcpy(g_camPos, origin, sizeof(pos_));
        memcpy(g_camOrient, orient, sizeof(orient_));
        memcpy(g_idealOrient, orient, sizeof(ideal_));
        *g_pitchOffset = 0.0f;
        if (g_idealPos) {
            memcpy(idealPos_, g_idealPos, sizeof(idealPos_));
            memcpy(g_idealPos, origin, sizeof(idealPos_));
        }
    }
    ~Scope()
    {
        memcpy(g_camPos, pos_, sizeof(pos_));
        memcpy(g_camOrient, orient_, sizeof(orient_));
        memcpy(g_idealOrient, ideal_, sizeof(ideal_));
        *g_pitchOffset = pitch_;
        if (g_idealPos)
            memcpy(g_idealPos, idealPos_, sizeof(idealPos_));
        --g_depth;
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    float pos_[3]{}, orient_[9]{}, ideal_[9]{}, idealPos_[3]{};
    float pitch_ = 0.0f;
};

// Distance fixe à laquelle le jeu fait partir son rayon pour l'arme en main, 0 s'il part de la
// distance caméra - personnage (ou si on ne sait pas)
float FixedStart(int weaponClass)
{
    if (!g_startTable || weaponClass == kStartNearClass)
        return 0.0f;
    if (weaponClass >= 0 && weaponClass <= kStartClassMax) {
        const uint8_t branch = g_startTable[weaponClass];
        if (branch > 7 || g_startJumps[branch] != g_startFarBranch)
            return 0.0f;
    }
    const float start = *g_startFar;
    return start > 0.0f && start < 50.0f ? start : 0.0f;
}

// Le rayon demandé, à défaut celui du regard (sauf handOnly). used : rayon obtenu.
bool Ray(int hand, bool handOnly, float origin[3], float orient[9], int* used)
{
    if (hand >= 0 && g_rayFn(hand, origin, orient)) {
        *used = hand;
        return true;
    }
    if (handOnly || !g_rayFn(-1, origin, orient))
        return false;
    *used = -1;
    return true;
}

// Fait calculer au jeu le point visé le long d'un de nos rayons. out : sortie du jeu (point
// visé, nul s'il n'y a rien à viser). Résultat du jeu : l'objet visé. traced : faux si aucun
// de nos rayons n'était disponible (le jeu a alors visé avec sa caméra).
int Trace(int hand, bool handOnly, uint8_t* player, float* out, bool* traced)
{
    float origin[3], orient[9];
    int used = -1;
    *traced = Ray(hand, handOnly, origin, orient, &used);
    if (!*traced) {
        if (handOnly) {
            g_sights[hand + 1].tick = 0;
            out[0] = out[1] = out[2] = 0.0f;
            return 0;
        }
        return g_aimPoint(player, out);
    }
    int weaponClass = -1;
    float range = 0.0f;
    const bool armed = player::WeaponInfo(&weaponClass, &range);
    // Le jeu part de « caméra + distance fixe » : on lui présente une caméra reculée d'autant
    const float back = armed ? FixedStart(weaponClass) - kStartGap : 0.0f;
    float camera[3]{origin[0], origin[1], origin[2]};
    if (back > 0.0f)
        for (int k = 0; k < 3; ++k)
            camera[k] -= back * orient[6 + k];

    int target = 0;
    {
        Scope scope(camera, orient);
        target = g_aimPoint(player, out);
    }
    Record& record = g_sights[used + 1];
    // Sortie nulle : le jeu n'a rien à viser (mains vides)
    if (out[0] == 0.0f && out[1] == 0.0f && out[2] == 0.0f) {
        record.tick = 0;
        return target;
    }
    Sight& sight = record.sight;
    memcpy(sight.origin, origin, sizeof(sight.origin));
    memcpy(sight.forward, orient + 6, sizeof(sight.forward));
    memcpy(sight.point, out, sizeof(sight.point));
    float squared = 0.0f;
    for (int k = 0; k < 3; ++k)
        squared += (out[k] - origin[k]) * (out[k] - origin[k]);
    sight.distance = std::sqrt(squared);
    // Rien de touché : le point est au bout de la portée de l'arme
    sight.hit = armed && range > 1.0f && sight.distance < range - 0.05f;
    record.tick = GetTickCount64();
    ++g_points;
    if (!g_announced[used + 1]) {
        g_announced[used + 1] = true;
        LOG_ENGINE("visee : premier point vise avec le rayon %s : (%.1f, %.1f, %.1f) a %.1f m, depart (%.1f, %.1f, "
                   "%.1f), arme de classe %d, portee %.0f m, depart du rayon du jeu %s",
                   SourceName(used), out[0], out[1], out[2], sight.distance, origin[0], origin[1], origin[2],
                   weaponClass, range, back > 0.0f ? "ramene a la main" : "laisse au jeu");
    }
    return target;
}

int __cdecl HkAimPoint(uint8_t* player, float* out)
{
    if (!g_active || g_depth || !out || !player::IsLocal(player))
        return g_aimPoint(player, out);
    const int hand = g_fireHand >= 0 ? g_fireHand : controls::AimHand();
    bool traced = false;
    const int target = Trace(hand, false, player, out, &traced);
    // Le jeu range lui-même ce point dans le joueur : la visée libre est en cours
    if (traced && out == reinterpret_cast<float*>(player + kAimPoint)) {
        g_freeAimTick = GetTickCount64();
        // Une arme dans chaque main : le rayon de l'autre main aussi, pour son repère
        if (g_fireHand < 0 && hand >= 0 && hand <= 1 && player::DualWielding() && !player::InVehicle()) {
            float other[3];
            bool ignored = false;
            Trace(1 - hand, true, player, other, &ignored);
        } else if (hand >= 0 && hand <= 1) {
            g_sights[2 - hand].tick = 0;
        }
    }
    return target;
}

int __cdecl HkWeaponUpdate(uint8_t* human)
{
    if (!g_active || g_depth || !player::IsLocal(human))
        return g_weaponUpdate(human);
    float origin[3], orient[9];
    int used = -1;
    if (!Ray(controls::AimHand(), false, origin, orient, &used))
        return g_weaponUpdate(human);
    Scope scope(origin, orient);
    return g_weaponUpdate(human);
}

// Avant le tir : le point visé du joueur devient celui de la main qui tire, à cet instant
void AimForShot(uint8_t* player, int hand)
{
    const ULONGLONG now = GetTickCount64();
    if (!g_freeAimTick || now - g_freeAimTick > kFreeAimMaxAgeMs)
        return;
    float origin[3], orient[9];
    if (!g_rayFn(hand, origin, orient))
        return; // manette non suivie : le point visé de l'image reste valable
    float point[3]{};
    bool traced = false;
    const int target = Trace(hand, true, player, point, &traced);
    if (!traced || (point[0] == 0.0f && point[1] == 0.0f && point[2] == 0.0f))
        return;
    memcpy(player + kAimPoint, point, sizeof(point));
    memcpy(player + kAimTarget, &target, sizeof(target));
    ++g_shots[hand];
    if (!g_shotAnnounced[hand]) {
        g_shotAnnounced[hand] = true;
        LOG_ENGINE("visee : premier tir de la main %s, point vise recalcule avec son rayon : (%.1f, %.1f, %.1f)",
                   hand ? "droite" : "gauche", point[0], point[1], point[2]);
    }
}

// Fonction de tir des personnages (0x009C0500) : c'est elle qui crée la balle. Deux chemins y
// mènent pour le joueur : le tir du joueur (0x009D9090, à chaque image où l'attaque est tenue),
// et la mise à jour du personnage quand un tir attendait la fin d'une animation (arme en train
// d'être levée : le « premier coup »). Dans les deux cas :
//  - la balle ne va pas au point visé rangé dans le joueur : la fonction qui la crée
//    (0x009BE6F0) redemande un point à 0x009C4560, qui relance un rayon depuis la position
//    finale de la caméra le long de son avant, donc là où porte le regard. Tout l'appel se
//    fait donc avec la caméra décrivant le rayon de la main qui tire ;
//  - avec une arme dans chaque main, « autre main » devient la main de la gâchette
//    (engine/controls a levé ce qui, dans cette fonction, faisait partir les deux armes
//    ensemble ou imposait l'alternance).
bool __cdecl HkCharacterFire(uint8_t* human, void* target, int a3, int a4, int offhand, int alt, int a7)
{
    if (!g_active || g_depth || !player::IsLocal(human))
        return g_characterFire(human, target, a3, a4, offhand, alt, a7);

    int hand = 1;
    if (player::DualWielding() && !player::InVehicle()) {
        // Tir différé (hors du tir du joueur) : la main du dernier tir demandé
        hand = g_playerFireHand >= 0 ? g_playerFireHand : g_lastFireHand;
        if (!(alt & 0xFF)) {
            if (!g_dualAnnounced) {
                g_dualAnnounced = true;
                LOG_ENGINE("visee : premier tir avec une arme dans chaque main : le jeu demandait la main %s, la "
                           "gachette la main %s",
                           (offhand & 0xFF) ? "gauche" : "droite", hand ? "droite" : "gauche");
            }
            offhand = hand ? 0 : 1;
        }
    }
    auto* slot = static_cast<uint8_t*>(player::HeldWeaponSlot());
    int before = 0, after = 0;
    if (slot)
        memcpy(&before, slot + kSlotAmmo, sizeof(before));

    // Le tir du joueur a déjà recalculé le point visé ; le tir différé le fait ici
    if (g_playerFireHand < 0) {
        AimForShot(human, hand);
        ++g_deferred;
    }
    g_fireHand = hand;
    bool fired = false;
    float origin[3], orient[9];
    int used = -1;
    if (Ray(hand, false, origin, orient, &used)) {
        Scope scope(origin, orient);
        fired = g_characterFire(human, target, a3, a4, offhand, alt, a7);
        if (used == hand)
            ++g_scoped[hand];
    } else {
        fired = g_characterFire(human, target, a3, a4, offhand, alt, a7);
    }
    g_fireHand = -1;

    ++g_calls[hand];
    // Le chargeur peut avoir changé d'arme ou été rempli pendant l'appel : seule une baisse compte
    if (slot && slot == player::HeldWeaponSlot()) {
        memcpy(&after, slot + kSlotAmmo, sizeof(after));
        if (after < before)
            g_bullets[hand] += static_cast<unsigned>(before - after);
    }
    return fired;
}

// Tir du joueur (0x009D9090). offhand est la main choisie par le jeu (son alternance) ; on
// retient celle de la gâchette pour la fonction de tir, et on recalcule d'abord le point visé
// du joueur avec son rayon (le tir du joueur lit l'objet visé avant de tirer).
bool __cdecl HkPlayerFire(int offhand)
{
    auto* player = static_cast<uint8_t*>(player::Local());
    if (!g_active || g_depth || !player)
        return g_playerFire(offhand);
    const int hand = player::DualWielding() && !player::InVehicle() ? controls::FireHand() : 1;
    g_lastFireHand = hand;
    AimForShot(player, hand);
    g_playerFireHand = hand;
    bool fired = false;
    if (g_characterFireTarget) {
        fired = g_playerFire(offhand);
    } else {
        // Fonction de tir non détournée : au moins la caméra sur le rayon pendant le tir
        float origin[3], orient[9];
        int used = -1;
        if (Ray(hand, false, origin, orient, &used)) {
            Scope scope(origin, orient);
            fired = g_playerFire(offhand);
        } else {
            fired = g_playerFire(offhand);
        }
    }
    g_playerFireHand = -1;
    return fired;
}

bool Hook(uint8_t* site, void* detour, void** original, void** target, const char* name)
{
    MH_STATUS st = MH_CreateHook(site, detour, original);
    if (st == MH_OK)
        st = MH_EnableHook(site);
    if (st != MH_OK) {
        LOG_ENGINE("visee : ECHEC MinHook sur %s : %s", name, MH_StatusToString(st));
        MH_RemoveHook(site);
        return false;
    }
    *target = site;
    return true;
}

} // namespace

bool Install(float* cameraPos, float* cameraOrient, RayFn ray)
{
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto rel = [base](const void* p) { return static_cast<unsigned long>(reinterpret_cast<uintptr_t>(p) - base); };
    const auto absolute = [](const uint8_t* operand) {
        uint32_t address = 0;
        memcpy(&address, operand, sizeof(address));
        return address;
    };
    auto* aimDir = static_cast<uint8_t*>(pattern::Find("direction de visee", kAimDirSig));
    auto* aimPoint = static_cast<uint8_t*>(pattern::Find("point vise", kAimPointSig));
    auto* update = static_cast<uint8_t*>(pattern::Find("mise a jour de l'arme", kWeaponUpdateSig));
    if (!cameraPos || !cameraOrient || !ray || !aimDir || !aimPoint || !update) {
        LOG_ENGINE("visee : fonctions de visee introuvables, le jeu vise avec sa camera");
        return false;
    }
    g_camPos = cameraPos;
    g_camOrient = cameraOrient;
    g_rayFn = ray;
    g_idealOrient = reinterpret_cast<float*>(static_cast<uintptr_t>(absolute(aimDir + kIdealOrientOffset)));
    g_pitchOffset = reinterpret_cast<float*>(static_cast<uintptr_t>(absolute(aimDir + kPitchOffsetOffset)));
    // Même disposition que dans l'exe étudié ? Sinon on laisse la position idéale tranquille.
    if (reinterpret_cast<uint8_t*>(g_idealOrient) - reinterpret_cast<uint8_t*>(g_camOrient) == kIdealOrientDelta)
        g_idealPos = g_camPos + 3;
    else
        LOG_ENGINE("visee : orientation ideale a une distance inattendue de l'orientation finale, position ideale "
                   "laissee au jeu");

    // Départ du rayon du jeu selon la classe de l'arme
    uint8_t* startSwitch = pattern::FindIn(aimPoint, 0x180, kStartSwitchSig);
    uint8_t* startFar = pattern::FindIn(aimPoint, 0x180, kStartFarSig);
    if (startSwitch && startFar) {
        g_startTable = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(absolute(startSwitch + 8)));
        g_startJumps = reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(absolute(startSwitch + 15)));
        g_startFarBranch = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(startFar));
        g_startFar = reinterpret_cast<const float*>(static_cast<uintptr_t>(absolute(startFar + 12)));
    } else {
        LOG_ENGINE("visee : aiguillage du depart du rayon introuvable, certaines armes viseront a partir de 4 m "
                   "devant la main");
    }

    bool hooked =
        Hook(aimPoint, reinterpret_cast<void*>(&HkAimPoint), reinterpret_cast<void**>(&g_aimPoint), &g_aimPointTarget,
             "le point vise") &&
        Hook(update, reinterpret_cast<void*>(&HkWeaponUpdate), reinterpret_cast<void**>(&g_weaponUpdate),
             &g_weaponUpdateTarget, "la mise a jour de l'arme");
    if (!hooked) {
        Remove();
        return false;
    }
    LOG_ENGINE("visee : point vise exe+0x%lX et mise a jour de l'arme exe+0x%lX detournes ; orientation ideale "
               "exe+0x%lX, decalage de tangage exe+0x%lX (vaut %.3f), depart fixe du rayon %.1f m",
               rel(aimPoint), rel(update), rel(g_idealOrient), rel(g_pitchOffset), static_cast<double>(*g_pitchOffset),
               g_startFar ? static_cast<double>(*g_startFar) : 0.0);

    // Tir du joueur : facultatif (sans lui, la balle va au point visé de l'image, celui de la
    // main qui vise)
    auto* characterFire = static_cast<uint8_t*>(pattern::Find("tir des personnages", kCharacterFireSig));
    if (characterFire &&
        Hook(characterFire, reinterpret_cast<void*>(&HkCharacterFire), reinterpret_cast<void**>(&g_characterFire),
             &g_characterFireTarget, "le tir des personnages"))
        LOG_ENGINE("visee : tir des personnages exe+0x%lX detourne (premier coup et tirs differes compris)",
                   rel(characterFire));
    auto* fire = static_cast<uint8_t*>(pattern::Find("tir du joueur", kPlayerFireSig));
    if (fire && Hook(fire, reinterpret_cast<void*>(&HkPlayerFire), reinterpret_cast<void**>(&g_playerFire),
                     &g_playerFireTarget, "le tir du joueur"))
        LOG_ENGINE("visee : tir du joueur exe+0x%lX detourne, chaque balle suit le rayon de la main qui tire",
                   rel(fire));
    else
        LOG_ENGINE("visee : tir du joueur non detourne, les balles vont au point vise par la derniere main");
    return true;
}

void Remove()
{
    for (void** target : {&g_aimPointTarget, &g_weaponUpdateTarget, &g_playerFireTarget, &g_characterFireTarget}) {
        if (*target) {
            MH_DisableHook(*target);
            MH_RemoveHook(*target);
            *target = nullptr;
        }
    }
    g_active = false;
}

void SetActive(bool active)
{
    g_active = active && g_aimPointTarget && g_weaponUpdateTarget;
}

bool GetSight(int hand, Sight* sight)
{
    if (hand < -1 || hand > 1 || !g_active)
        return false;
    const Record& record = g_sights[hand + 1];
    if (!record.tick || GetTickCount64() - record.tick > kSightMaxAgeMs)
        return false;
    *sight = record.sight;
    return true;
}

void TakeStats(Stats* stats)
{
    stats->points = g_points;
    g_points = 0;
    for (int hand = 0; hand < 2; ++hand) {
        stats->calls[hand] = g_calls[hand];
        stats->aimed[hand] = g_shots[hand];
        stats->bullets[hand] = g_bullets[hand];
        stats->scoped[hand] = g_scoped[hand];
        stats->deferred = g_deferred;
        g_calls[hand] = g_shots[hand] = g_bullets[hand] = g_scoped[hand] = 0;
    }
    g_deferred = 0;
}

} // namespace engine::aim
