#pragma once

namespace xr {

// Démarre le thread OpenXR : instance, session D3D11, puis affichage de l'image du jeu
// sur un écran virtuel. En cas d'échec (pas de runtime 32 bits, casque absent...) on
// loggue et le jeu continue normalement.
void Start();

// Demande l'arrêt du thread (non bloquant : utilisable depuis DllMain)
void RequestStop();

} // namespace xr
