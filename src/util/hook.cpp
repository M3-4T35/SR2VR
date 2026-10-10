#include "util/hook.h"
#include "util/log.h"

#include <MinHook.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace hook {

std::string Describe(const void* address)
{
    char text[MAX_PATH + 32];
    HMODULE module = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(address), &module) &&
        module) {
        char path[MAX_PATH]{};
        GetModuleFileNameA(module, path, MAX_PATH);
        const char* name = strrchr(path, '\\');
        snprintf(text, sizeof(text), "%s+0x%lX", name ? name + 1 : path,
                 static_cast<unsigned long>(reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(module)));
    } else {
        snprintf(text, sizeof(text), "%p", address);
    }
    return text;
}

bool HookMethod(void* object, int index, void* detour, void** original, const char* name, const char* prefix,
                std::vector<void*>& mhTargets)
{
    void* target = (*reinterpret_cast<void***>(object))[index];
    if (!target)
        return false;
    for (void* t : mhTargets)
        if (t == target) {
            vrlog::Write(prefix, "  %-36s %s (deja detournee par nous, partagee)", name, Describe(target).c_str());
            return true;
        }

    // Saut en tête de fonction : un autre module l'a déjà détournée
    std::string neighbour;
    const auto* code = static_cast<const uint8_t*>(target);
    if (code[0] == 0xE9) {
        int32_t rel = 0;
        memcpy(&rel, code + 1, sizeof(rel));
        neighbour = Describe(code + 5 + rel);
    }

    MH_STATUS st = MH_CreateHook(target, detour, original);
    if (st == MH_OK)
        st = MH_QueueEnableHook(target);
    if (st != MH_OK) {
        vrlog::Write(prefix, "  %-36s %s ECHEC MinHook : %s", name, Describe(target).c_str(), MH_StatusToString(st));
        MH_RemoveHook(target);
        return false;
    }
    mhTargets.push_back(target);
    if (neighbour.empty())
        vrlog::Write(prefix, "  %-36s %s detournee", name, Describe(target).c_str());
    else
        vrlog::Write(prefix, "  %-36s %s detournee, DEJA DETOURNEE vers %s (on passe avant)", name,
                     Describe(target).c_str(), neighbour.c_str());
    return true;
}

bool ApplyQueued(const char* prefix)
{
    const MH_STATUS st = MH_ApplyQueued();
    if (st != MH_OK)
        vrlog::Write(prefix, "ERREUR : activation des detours : %s", MH_StatusToString(st));
    return st == MH_OK;
}

void RemoveAll(std::vector<void*>& mhTargets)
{
    for (void* t : mhTargets) {
        MH_DisableHook(t);
        MH_RemoveHook(t);
    }
    mhTargets.clear();
}

} // namespace hook
