#pragma once

namespace render {

// Installe les hooks Present/Reset de Direct3D 9 (MinHook doit être initialisé). Renvoie
// false (et loggue la raison) si l'installation échoue : le jeu continue alors sans VR.
bool InstallD3D9Hooks();

// Retire proprement les hooks (déchargement de la DLL)
void RemoveD3D9Hooks();

// Images présentées par le jeu depuis le démarrage (tout thread)
unsigned long long PresentCount();

} // namespace render
