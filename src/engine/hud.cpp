#include "engine/hud.h"
#include "util/log.h"
#include "util/pattern.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace engine::hud {
namespace {

// Exécution d'une chaîne Lua (« LuaExecute » / « VintExecute » chez Juiced) : chaîne dans eax,
// état Lua dans esi. La signature commence 9 octets après le début de la fonction : mov ecx,eax ;
// push edi ; lea edi,[ecx+1] ; nop ; boucle de strlen ; ... ; call <chargement> .
constexpr char kLuaExecuteSig[] =
    "8B C8 57 8D 79 01 90 8A 11 83 C1 01 84 D2 75 F7 2B CF 56 8D 54 24 0C 89 44 24 0C 89 4C 24 10 E8";
constexpr int kLuaExecuteStart = 9;
constexpr uint8_t kLuaExecuteProlog[3] = {0x55, 0x8B, 0xEC}; // push ebp ; mov ebp,esp
// Dans la boucle du jeu (une image de simulation) : cmp [esi+48h],eax ; je ... ; mov esi,[esi] ;
// cmp esi,ecx ; je ... ; test esi,esi ; jne ... ; jmp ... ; mov eax,[état Lua de l'interface] ;
// push 0 ; push 0 ; push 0 ; push <nom>. (L'endroit où le jeu enregistre ses fonctions Lua,
// plus parlant, est réécrit par Juiced : sa signature ne se retrouve pas en mémoire.)
constexpr char kVintStateSig[] = "39 46 48 74 0C 8B 36 3B F1 74 3C 85 F6 75 F1 EB 36 A1 ?? ?? ?? ?? 6A 00 6A 00 6A 00 68";
constexpr int kVintStateOffset = 18; // opérande du « mov eax »

void* g_luaExecute = nullptr;
void* const* g_vintState = nullptr; // adresse du pointeur global vers l'état Lua
unsigned g_failures = 0;

#if defined(_MSC_VER) && !defined(__clang__)
void CallLuaExecute(void* fn, void* state, const char* code)
{
    __asm {
        push esi
        mov esi, state
        mov eax, code
        call fn
        pop esi
    }
}
#else
// Compilation de contrôle hors MSVC : pas d'appel (la DLL se compile avec MSVC)
void CallLuaExecute(void*, void*, const char*) {}
#endif

bool Execute(const char* code)
{
#if defined(_MSC_VER) && !defined(__clang__)
    __try {
#endif
        void* state = *g_vintState;
        if (!state)
            return false;
        CallLuaExecute(g_luaExecute, state, code);
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
    if (auto* site = static_cast<uint8_t*>(pattern::Find("execution de Lua", kLuaExecuteSig))) {
        uint8_t* start = site - kLuaExecuteStart;
        // Un détour posé par un autre mod en tête de fonction (saut E9) reste appelable
        if (memcmp(start, kLuaExecuteProlog, sizeof(kLuaExecuteProlog)) == 0 || start[0] == 0xE9)
            g_luaExecute = start;
        else
            LOG_ENGINE("interface : debut inattendu pour l'execution de Lua (%02X %02X %02X)", start[0], start[1], start[2]);
    }
    if (auto* site = static_cast<uint8_t*>(pattern::Find("etat Lua de l'interface", kVintStateSig))) {
        uint32_t address = 0;
        memcpy(&address, site + kVintStateOffset, sizeof(address));
        g_vintState = reinterpret_cast<void* const*>(static_cast<uintptr_t>(address));
    }
    if (!g_luaExecute || !g_vintState) {
        LOG_ENGINE("interface : execution de Lua ou etat Lua introuvable, l'interface restera aux bords de l'image");
        g_luaExecute = nullptr;
        return false;
    }
    LOG_ENGINE("interface : execution de Lua exe+0x%lX, etat Lua de l'interface exe+0x%lX (vaut %p)",
               static_cast<unsigned long>(reinterpret_cast<uintptr_t>(g_luaExecute) - base),
               static_cast<unsigned long>(reinterpret_cast<uintptr_t>(g_vintState) - base), *g_vintState);
    return true;
}

void Place(float scale, float x, float y)
{
    if (!g_luaExecute)
        return;
    // Les documents de la partie : l'interface (« hud ») et ses messages (« hud_msg »). Un
    // document absent (menu principal, chargement) est simplement ignoré.
    char code[640];
    snprintf(code, sizeof(code),
             "for _, n in pairs({\"hud\", \"hud_msg\"}) do "
             "local d = vint_document_find(n) "
             "if d and d ~= 0 and d ~= -1 then "
             "local h = vint_object_find(\"safe_frame\", 0, d) "
             "if h and h ~= 0 and h ~= -1 then "
             "vint_set_property(h, \"scale\", %.4f, %.4f) "
             "vint_set_property(h, \"anchor\", %.2f, %.2f) "
             "end end end",
             static_cast<double>(scale), static_cast<double>(scale), static_cast<double>(x), static_cast<double>(y));
    if (!Execute(code) && ++g_failures == 1)
        LOG_ENGINE("interface : etat Lua absent ou execution en echec, l'interface n'est pas deplacee pour l'instant");
}

void ShowReticle(bool visible)
{
    if (!g_luaExecute)
        return;
    char code[320];
    snprintf(code, sizeof(code),
             "local t = Hud_reticules "
             "local h = t and t.elements and t.elements.reticule_h "
             "if h and h ~= 0 and h ~= -1 then vint_set_property(h, \"visible\", %s) end",
             visible ? "true" : "false");
    Execute(code);
}

} // namespace engine::hud
