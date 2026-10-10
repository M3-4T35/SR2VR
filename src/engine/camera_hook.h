#pragma once

namespace engine {

// Détourne la caméra du jeu pour lui appliquer l'orientation du casque : côté simulation
// (mise à jour de la caméra) et côté rendu (copie de la caméra au début de chaque image).
// false (loggué) si rien n'a pu être détourné : le jeu continue sans.
bool InstallCameraHook();
void RemoveCameraHook();

// Vue avec laquelle l'image présentée a été rendue : orientation de tête appliquée à la
// caméra de rendu, œil (0 gauche, 1 droit ; toujours 0 hors relief) et champ de vue
// horizontal du rendu (degrés). false si cette image n'a pas de vue : pas de caméra de rendu
// (menu principal, chargement), jeu en pause (menu de pause : il s'affiche sur l'écran
// virtuel) ou tête non suivie. À appeler une fois par Present, sur le thread du rendu : les
// vues sont rendues dans l'ordre où les images ont été préparées.
bool TakeFrameView(float headQuat[4], int* eye, float* hfovDeg);

} // namespace engine
