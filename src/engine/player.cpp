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
constexpr uint8_t kTagTransformProlog[4] = {0x85, 0xC9, 0x7C, 0x46}; // test ecx,ecx ; jl ...

// Champs de l'objet joueur (exe Steam modifié par Juiced ; noms d'après les sources de Juiced)
constexpr size_t kRootPos = 0x14;     // 3 floats
constexpr size_t kRootOrient = 0x20;  // 9 floats
constexpr size_t kCharacter = 0x5E0;  // pointeur ; son premier champ est le numéro du personnage
constexpr size_t kHeadTag = 0xCBE;    // short : indice du repère « head », -1 s'il manque

const uintptr_t* g_player = nullptr; // adresse du pointeur global
void* g_tagTransform = nullptr;

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
bool ReadHead(float head[3], float root[3])
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
        const auto* rootPos = reinterpret_cast<const float*>(player + kRootPos);
        memcpy(root, rootPos, 3 * sizeof(float));
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

bool HeadPosition(float head[3], float root[3])
{
    if (!g_player || !ReadHead(head, root))
        return false;
    if (!std::isfinite(head[0]) || !std::isfinite(head[1]) || !std::isfinite(head[2]))
        return false;
    // Une tête à plus de 5 m de la racine n'est pas une tête
    const float dx = head[0] - root[0], dy = head[1] - root[1], dz = head[2] - root[2];
    return dx * dx + dy * dy + dz * dz < 25.0f;
}

} // namespace engine::player
