#pragma once

namespace xr {

// Démarre le thread OpenXR : instance, session D3D11, puis affichage de l'image du jeu
// sur un écran virtuel. En cas d'échec (pas de runtime 32 bits, casque absent...) on
// loggue et le jeu continue normalement.
void Start();

// Demande l'arrêt du thread (non bloquant : utilisable depuis DllMain)
void RequestStop();

// Orientation du casque dans l'espace LOCAL (0 = face à l'écran virtuel), prédite pour la
// prochaine image affichée : quaternion OpenXR (x, y, z, w ; repère x droite, y haut,
// z arrière). ipd (facultatif) : écart entre les yeux mesuré par le casque, en mètres.
// false si la session ne tourne pas, si le casque n'est pas sur la tête ou n'est pas suivi
// (pose de plus de 0,5 s).
bool GetHeadPose(float quat[4], float* ipd);

} // namespace xr
