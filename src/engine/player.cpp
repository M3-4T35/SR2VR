#include "engine/player.h"
#include "util/log.h"
#include "util/pattern.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace engine::player {
namespace {

// Début de la fonction d'entrée des magasins : mov ecx,[joueur] ; sub esp,48h ; push ebx ;
// mov ebx,[esp+50h] ; push ebp ; push esi ; xor esi,esi ; cmp ecx,esi ; push edi ; mov ebp,ecx
constexpr char kPlayerSig[] = "8B 0D ?? ?? ?? ?? 83 EC 48 53 8B 5C 24 50 55 56 33 F6 3B CE 57 8B E9";
// Milieu de la fonction « position d'un os ou d'un repère » (131 appelants) : imul ecx,5E8h ;
// add ecx,<table des personnages> ; je ... ; puis les quatre arguments repoussés et l'appel de
// la fonction interne. Trois fonctions commencent pareil : c'est la suite qui la distingue.
constexpr char kTagTransformSig[] =
    "69 C9 E8 05 00 00 81 C1 ?? ?? ?? ?? 74 1D 8B 54 24 14 8B 44 24 10 52 8B 54 24 0C 50 8B 44 24 14 52 50 8B 44 "
    "24 14 E8";
constexpr int kTagTransformStart = 0x1F; // la signature commence 0x1F octets après le début
constexpr int kInstancesOffset = 8;      // opérande du « add ecx » : table des personnages animés
constexpr size_t kInstanceSize = 0x5E8;
constexpr int kInstanceCount = 190;
constexpr uint8_t kTagTransformProlog[4] = {0x85, 0xC9, 0x7C, 0x46}; // test ecx,ecx ; jl ...

// Champs de l'objet joueur (exe Steam modifié par Juiced ; noms d'après les sources de Juiced)
constexpr size_t kRootPos = 0x14;     // 3 floats
constexpr size_t kRootOrient = 0x20;  // 9 floats
constexpr size_t kCharacter = 0x5E0;  // pointeur ; son premier champ est le numéro du personnage
constexpr size_t kHeadTag = 0xCBE;    // short : indice du repère « head », -1 s'il manque
constexpr size_t kVehicle = 0xD74;    // numéro de l'objet véhicule, 0 à pied
constexpr size_t kHeldObject = 0x10CC; // objet tenu en main (arme), nul les mains vides
constexpr size_t kHeldSlot = 0xC4;     // dans l'objet tenu : sa case d'inventaire
constexpr size_t kSlotInfo = 8;        // dans la case : description de l'arme
constexpr size_t kSlotFlags2 = 0xD;    // dans la case : bit 0 = arme tenue en double
constexpr size_t kInfoClass = 0x34;    // dans la description : classe de l'arme
constexpr size_t kInfoRange = 0xE0;    // dans la description : portée (float, mètres)
constexpr size_t kObjectNumber = 0x44; // dans tout objet : son numéro
constexpr size_t kObjectType = 0x4C;   // dans tout objet : son type (5 = véhicule)

const uintptr_t* g_player = nullptr; // adresse du pointeur global
void* g_tagTransform = nullptr;
uintptr_t g_instances = 0;           // table des personnages animés (un par numéro)
uintptr_t g_objects = 0;             // table des objets du jeu
uintptr_t g_dumpedRig = 0;           // squelette déjà relevé dans le log

// Relevé du squelette du joueur, une fois par squelette : sert à préparer le masquage de la
// tête et les mains. Personnage animé : matrices des os en +4 (48 octets par os, repère du
// modèle), squelette en +0x18. Squelette : nombre d'os en +0x24, de repères en +0x30, table
// des os en +0x38 (36 octets par os, nom en premier), table des repères en +0x3C (60 octets :
// nom, transformation locale en +4, os parent en +0x34).
void DumpSkeleton(uintptr_t player, int number)
{
    if (!g_instances)
        return;
    const uintptr_t instance = g_instances + static_cast<uintptr_t>(number % kInstanceCount) * kInstanceSize;
    const uintptr_t rig = *reinterpret_cast<const uintptr_t*>(instance + 0x18);
    if (!rig || rig == g_dumpedRig)
        return;
    g_dumpedRig = rig;
    const unsigned bones = *reinterpret_cast<const unsigned*>(rig + 0x24);
    const unsigned tags = *reinterpret_cast<const unsigned*>(rig + 0x30);
    const uintptr_t boneTable = *reinterpret_cast<const uintptr_t*>(rig + 0x38);
    const uintptr_t tagTable = *reinterpret_cast<const uintptr_t*>(rig + 0x3C);
    LOG_ENGINE("squelette du joueur : %u os, %u reperes, squelette %p, matrices des os %p", bones, tags,
               reinterpret_cast<void*>(rig), reinterpret_cast<void*>(*reinterpret_cast<const uintptr_t*>(instance + 4)));
    if (!boneTable || bones > 256 || tags > 512)
        return;
    static const struct {
        size_t offset;
        const char* name;
    } kCached[] = {{0xCBE, "head (repere)"},   {0xCBA, "neck (repere)"},   {0xCA4, "R-hand (repere)"},
                   {0xCA6, "L-hand (repere)"}, {0xCE4, "R-hand (os)"},     {0xCE2, "L-hand (os)"},
                   {0xCAE, "R-IKNode (os)"},   {0xCAC, "L-IKNode (os)"},   {0xCEE, "Camera (os)"},
                   {0xCF0, "camTarget (os)"},  {0xCB4, "spine (os)"},      {0xCB2, "pelvis (os)"}};
    for (const auto& cached : kCached)
        LOG_ENGINE("  indice garde par le joueur : %-18s = %d", cached.name,
                   static_cast<int>(*reinterpret_cast<const short*>(player + cached.offset)));
    for (unsigned i = 0; i < bones; ++i) {
        const uintptr_t bone = boneTable + i * 36;
        const char* name = *reinterpret_cast<const char* const*>(bone);
        const auto* w = reinterpret_cast<const int*>(bone + 4);
        LOG_ENGINE("  os %3u %-22.22s %08X %08X %08X %08X %08X %08X %08X %08X", i, name ? name : "?", w[0], w[1], w[2],
                   w[3], w[4], w[5], w[6], w[7]);
    }
    for (unsigned i = 0; tagTable && i < tags; ++i) {
        const uintptr_t tag = tagTable + i * 60;
        const char* name = *reinterpret_cast<const char* const*>(tag);
        const auto* local = reinterpret_cast<const float*>(tag + 4);
        LOG_ENGINE("  repere %3u (indice %3u) %-22.22s os parent %d, position locale (%.3f, %.3f, %.3f), dernier champ %08X",
                   i, bones + i, name ? name : "?", *reinterpret_cast<const int*>(tag + 0x34), local[9], local[10],
                   local[11], *reinterpret_cast<const unsigned*>(tag + 0x38));
    }
}

// La fonction du jeu : numéro du personnage dans ecx, cinq arguments sur la pile (indice,
// position et orientation de la racine, position et orientation de sortie, cette dernière
// facultative), pile nettoyée par l'appelant.
#if defined(_MSC_VER) && !defined(__clang__)
void CallTagTransform(void* fn, int character, int index, const float* rootPos, const float* rootOrient,
                      float* outPos, float* outOrient)
{
    __asm {
        push outOrient
        push outPos
        push rootOrient
        push rootPos
        push index
        mov ecx, character
        call fn
        add esp, 14h
    }
}
#else
// Compilation de contrôle hors MSVC : pas d'appel (la DLL se compile avec MSVC)
void CallTagTransform(void*, int, int, const float*, const float*, float*, float*) {}
#endif

// Lectures dans la mémoire du jeu : un pointeur périmé (changement de niveau) ne doit pas
// faire tomber le jeu
bool ReadHead(float head[3], float root[3], float orient[9])
{
#if defined(_MSC_VER) && !defined(__clang__)
    __try {
#endif
        const uintptr_t player = *g_player;
        if (!player)
            return false;
        const uintptr_t character = *reinterpret_cast<const uintptr_t*>(player + kCharacter);
        if (!character)
            return false;
        const int number = *reinterpret_cast<const int*>(character);
        const int index = *reinterpret_cast<const short*>(player + kHeadTag);
        if (number < 0 || index < 0)
            return false;
        DumpSkeleton(player, number);
        const auto* rootPos = reinterpret_cast<const float*>(player + kRootPos);
        memcpy(root, rootPos, 3 * sizeof(float));
        memcpy(orient, reinterpret_cast<const float*>(player + kRootOrient), 9 * sizeof(float));
        // La fonction n'écrit rien si l'indice est hors du squelette
        head[0] = head[1] = head[2] = NAN;
        CallTagTransform(g_tagTransform, number, index, rootPos, reinterpret_cast<const float*>(player + kRootOrient),
                         head, nullptr);
        return true;
#if defined(_MSC_VER) && !defined(__clang__)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
}

} // namespace

bool Install()
{
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (auto* site = static_cast<uint8_t*>(pattern::Find("joueur", kPlayerSig))) {
        uint32_t address = 0;
        memcpy(&address, site + 2, sizeof(address));
        g_player = reinterpret_cast<const uintptr_t*>(static_cast<uintptr_t>(address));
    }
    if (auto* site = static_cast<uint8_t*>(pattern::Find("position d'un repere", kTagTransformSig))) {
        uint8_t* start = site - kTagTransformStart;
        uint32_t instances = 0;
        memcpy(&instances, site + kInstancesOffset, sizeof(instances));
        g_instances = instances;
        if (memcmp(start, kTagTransformProlog, sizeof(kTagTransformProlog)) == 0)
            g_tagTransform = start;
        else
            LOG_ENGINE("joueur : debut inattendu pour la fonction des reperes (%02X %02X %02X %02X)", start[0], start[1],
                       start[2], start[3]);
    }
    if (!g_player || !g_tagTransform) {
        LOG_ENGINE("joueur : pointeur ou fonction des reperes introuvable, pas de vue a la premiere personne");
        g_player = nullptr;
        return false;
    }
    LOG_ENGINE("joueur : pointeur exe+0x%lX (vaut %p), position d'un repere exe+0x%lX",
               static_cast<unsigned long>(reinterpret_cast<uintptr_t>(g_player) - base),
               reinterpret_cast<void*>(*g_player),
               static_cast<unsigned long>(reinterpret_cast<uintptr_t>(g_tagTransform) - base));
    return true;
}

bool IsLocal(const void* p)
{
    return g_player && p && reinterpret_cast<uintptr_t>(p) == *g_player;
}

void SetObjectTable(const void* table)
{
    g_objects = reinterpret_cast<uintptr_t>(table);
}

bool InVehicle()
{
    if (!g_player || !g_objects)
        return false;
#if defined(_MSC_VER) && !defined(__clang__)
    __try {
#endif
        const uintptr_t player = *g_player;
        if (!player)
            return false;
        const uint32_t number = *reinterpret_cast<const uint32_t*>(player + kVehicle);
        if (!number)
            return false;
        const uintptr_t object = *reinterpret_cast<const uintptr_t*>(g_objects + ((number & 0xFFF) << 4));
        return object && *reinterpret_cast<const uint32_t*>(object + kObjectNumber) == number &&
               *reinterpret_cast<const int*>(object + kObjectType) == 5;
#if defined(_MSC_VER) && !defined(__clang__)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
}

namespace {

// Case d'inventaire de l'arme en main, nullptr les mains vides
const uint8_t* HeldSlot()
{
    if (!g_player)
        return nullptr;
#if defined(_MSC_VER) && !defined(__clang__)
    __try {
#endif
        const uintptr_t player = *g_player;
        if (!player)
            return nullptr;
        const uintptr_t held = *reinterpret_cast<const uintptr_t*>(player + kHeldObject);
        if (!held)
            return nullptr;
        const uintptr_t slot = *reinterpret_cast<const uintptr_t*>(held + kHeldSlot);
        if (!slot || !*reinterpret_cast<const uintptr_t*>(slot + kSlotInfo))
            return nullptr;
        return reinterpret_cast<const uint8_t*>(slot);
#if defined(_MSC_VER) && !defined(__clang__)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
#endif
}

} // namespace

bool HoldingWeapon()
{
    return HeldSlot() != nullptr;
}

bool DualWielding()
{
    const uint8_t* slot = HeldSlot();
#if defined(_MSC_VER) && !defined(__clang__)
    __try {
#endif
        return slot && (slot[kSlotFlags2] & 1) != 0;
#if defined(_MSC_VER) && !defined(__clang__)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
}

bool WeaponInfo(int* weaponClass, float* range)
{
    const uint8_t* slot = HeldSlot();
    if (!slot)
        return false;
#if defined(_MSC_VER) && !defined(__clang__)
    __try {
#endif
        const uint8_t* info = *reinterpret_cast<const uint8_t* const*>(slot + kSlotInfo);
        *weaponClass = *reinterpret_cast<const int*>(info + kInfoClass);
        *range = *reinterpret_cast<const float*>(info + kInfoRange);
        return true;
#if defined(_MSC_VER) && !defined(__clang__)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
}

void* HeldWeaponSlot()
{
    return const_cast<uint8_t*>(HeldSlot());
}

void* Local()
{
    return g_player ? reinterpret_cast<void*>(*g_player) : nullptr;
}

void* Instance()
{
    if (!g_player || !g_instances || !*g_player)
        return nullptr;
    const uintptr_t character = *reinterpret_cast<const uintptr_t*>(*g_player + kCharacter);
    if (!character)
        return nullptr;
    const int number = *reinterpret_cast<const int*>(character);
    if (number < 0)
        return nullptr;
    return reinterpret_cast<void*>(g_instances + static_cast<uintptr_t>(number % kInstanceCount) * kInstanceSize);
}

bool HeadPosition(float head[3], float root[3], float orient[9])
{
    if (!g_player || !ReadHead(head, root, orient))
        return false;
    if (!std::isfinite(head[0]) || !std::isfinite(head[1]) || !std::isfinite(head[2]))
        return false;
    // Une tête à plus de 5 m de la racine n'est pas une tête
    const float dx = head[0] - root[0], dy = head[1] - root[1], dz = head[2] - root[2];
    return dx * dx + dy * dy + dz * dz < 25.0f;
}

} // namespace engine::player
