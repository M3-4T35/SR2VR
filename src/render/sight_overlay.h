#pragma once
// Repère de visée dessiné dans l'image du casque : un trait de la manette au point visé et un
// point à l'impact.
//
// Le moteur projette le point visé (et le départ du rayon) avec la caméra de chaque œil, au
// moment où il prépare l'image ; le dessin se fait plus tard, dans la copie de l'image relue
// pour le casque (des pixels en mémoire : aucun état Direct3D du jeu n'est touché). Chaque œil
// reçoit sa propre projection : le repère se voit en relief, à la distance de ce qu'il touche.
//
// Les coordonnées sont des tangentes dans le repère de la caméra : x / z vers la droite,
// y / z vers le haut, z le long du regard. Le bord droit de l'image vaut tan(champ de vue / 2).
#include <cstdint>

namespace render {

struct SightOverlay {
    bool dot = false;  // point d'impact visible
    float dotT[2]{};
    bool beam = false; // trait de la manette au point visé
    float beamA[2]{};  // côté manette
    float beamB[2]{};  // côté point visé
};

// Dessine le repère dans une image BGRA 8 bits de lignes contiguës. hfovDeg : champ de vue
// horizontal de l'image, pixels carrés.
void DrawSight(uint8_t* bgra, unsigned width, unsigned height, float hfovDeg, const SightOverlay& sight);

} // namespace render
