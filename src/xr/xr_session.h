#pragma once

namespace xr {

// Démarre le thread OpenXR : instance, session D3D11, puis affichage de l'image du jeu
// sur un écran virtuel. En cas d'échec (pas de runtime 32 bits, casque absent...) on
// loggue et le jeu continue normalement.
void Start();

// Demande l'arrêt du thread (non bloquant : utilisable depuis DllMain)
void RequestStop();

// Orientation du casque dans l'espace LOCAL (0 = face à l'écran virtuel), prédite pour la
// prochaine image affichée : matrice de rotation 3x3 rangée par lignes, dont les colonnes
// sont les axes du casque (x droite, y haut, z arrière). false si la session ne tourne pas
// ou si le casque n'est pas suivi (pose de plus de 0,5 s).
bool GetHeadRotation(float out[9]);

} // namespace xr
