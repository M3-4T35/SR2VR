#pragma once

namespace engine {

// Détourne la mise à jour de la caméra du jeu pour lui appliquer l'orientation du casque.
// false (loggué) si la fonction ou la caméra sont introuvables : le jeu continue sans.
bool InstallCameraHook();
void RemoveCameraHook();

} // namespace engine
