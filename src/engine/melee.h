#pragma once
// Coups portés au geste : poings et armes blanches.
//
// Dans Saints Row 2, une attaque de mêlée est une animation pendant laquelle le jeu balaie, à
// chaque image, une sphère (ou la forme de l'arme) entre la position précédente et la position
// courante du poing ou de l'arme ; ce que ce balayage rencontre reçoit le coup (dégâts,
// réaction, son, effets), une seule fois par attaque.
//
// En VR les bras du personnage suivent déjà les manettes. On fait donc le même balayage, avec
// la même fonction du jeu, mais sans attendre une animation : dès qu'une main va assez vite, le
// trajet de son poing (ou de l'arme blanche qu'elle tient) est balayé à chaque image de
// simulation, avec l'attaque que le jeu aurait choisie pour cette arme. Le coup part donc quand
// la main ou la lame traverse réellement quelqu'un, et le jeu applique ses propres dégâts.
//
// Avec une arme à feu, le même geste donne le coup de crosse (l'attaque secondaire de l'arme),
// au poing qui la tient.
//
// La gâchette garde les attaques animées du jeu ; pendant l'une d'elles on ne balaie rien.
// Tout se passe sur le thread de simulation.

namespace engine::melee {

// Cherche les fonctions du jeu et détourne la mise à jour de mêlée des personnages. false
// (loggué) : pas de coups au geste.
bool Install();
void Remove();

// À chaque mise à jour de la caméra. active : les bras suivent les manettes (première personne
// à pied) et les coups au geste sont demandés. startSpeed : vitesse de la main (m/s) à partir de
// laquelle un geste est un coup ; le geste finit quand elle retombe à la moitié.
// gunBash : un geste rapide d'une main qui tient une arme à feu donne un coup de crosse.
void SetFrame(bool active, float startSpeed, bool gunBash);

struct Stats {
    unsigned swings[2]; // gestes de chaque main (0 gauche, 1 droite) depuis le dernier relevé
    unsigned hits[2];   // personnages, véhicules ou objets touchés
    unsigned world[2];  // gestes qui ont touché le décor
    float fastest;      // plus grande vitesse de main relevée (m/s)
};
void TakeStats(Stats* stats);

} // namespace engine::melee
