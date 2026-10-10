#pragma once
#include <windows.h>

#include <cstdint>
#include <vector>

namespace pattern {

// Cherche une signature du type "8B C4 ?? 89" dans les sections exécutables du module
// (l'exe du jeu par défaut). Renvoie nullptr si absente ou ambiguë (loggué).
void* Find(const char* name, const char* signature, HMODULE module = nullptr);

// Première occurrence de la signature dans la plage [start, start + size), nullptr si absente
uint8_t* FindIn(uint8_t* start, size_t size, const char* signature);

} // namespace pattern
