#pragma once
// Détours MinHook sur des méthodes COM, avec de quoi voir qui d'autre les détourne déjà :
// Juiced, l'overlay NVIDIA et Discord posent leurs propres hooks dans le même processus.
#include <windows.h>

#include <string>
#include <vector>

namespace hook {

// "d3d9.dll+0x1A2B3" pour une adresse située dans un module chargé, "0x12345678" sinon
std::string Describe(const void* address);

// Méthode COM n° index de l'objet, détournée par MinHook (cible ajoutée à mhTargets, à
// retirer par RemoveAll). Une méthode déjà détournée par nous n'est pas reprise (*original
// inchangé). Si son code commence par un saut posé par un autre module, on le signale et on
// se chaîne devant lui. prefix : préfixe du log ("[render]"...). true si détournée.
bool HookMethod(void* object, int index, void* detour, void** original, const char* name, const char* prefix,
                std::vector<void*>& mhTargets);

// Désactive et retire les détours de la liste
void RemoveAll(std::vector<void*>& mhTargets);

} // namespace hook
