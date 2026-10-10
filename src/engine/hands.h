#pragma once
// Bras et armes du personnage amenés aux manettes VR.
//
// À chaque image, après avoir calculé la pose du squelette du joueur à partir de l'animation,
// le jeu passe en revue ses quatre membres (table en joueur+0x6E0 : bras gauche, bras droit,
// jambes) et demande pour chacun, à une fonction qu'on détourne, s'il a une cible d'IK. Au
// premier de ces appels, on pose nous-mêmes les deux bras avec les fonctions de pose du jeu
// (relever les transformations locales des os, les écrire, recalculer le squelette) :
//  - buste tourné autour de la verticale pour que les épaules fassent face au regard ;
//  - épaule et coude tournés pour que le poignet arrive à la manette (IK à deux os) ;
//  - main orientée pour que l'arme qu'elle tient pointe comme la manette : le canon se retrouve
//    sur le rayon de visée (le trait rouge du casque), et comme le jeu fait partir les balles
//    de la bouche du canon, le tir suit ce rayon.
// La position de l'arme dans la main n'est pas supposée : elle est mesurée à chaque image en
// demandant au jeu où sont la bouche du canon et son orientation avec la pose de l'animation.
//
// Tout se passe sur le thread de simulation.

namespace engine::hands {

// Pose d'une manette demandée au moment de poser les bras (hand : 0 gauche, 1 droite), en
// vecteurs du monde comptés depuis les yeux : grip, le creux de la main ; origin, le départ du
// rayon de visée ; orient, l'orientation de ce rayon (matrice 3x3 du jeu : droite, haut, avant).
// false : manette non suivie.
using FrameFn = bool (*)(int hand, float grip[3], float origin[3], float orient[9]);

// Cherche les fonctions du jeu et détourne celle des cibles d'IK. false (loggué) : pas de bras
// pilotés.
bool Install(FrameFn frame);
void Remove();

// À chaque mise à jour de la caméra. active : les bras suivent les manettes (première personne
// à pied). eyeFromRoot : des pieds du personnage à ses yeux, dans le monde. headRight : la
// droite de la tête dans le monde. torsoMaxDegrees : jusqu'à quel angle le buste se tourne
// vers le regard avant de poser les bras (0 = buste laissé à l'animation).
void SetFrame(bool active, const float eyeFromRoot[3], const float headRight[3], float torsoMaxDegrees);

// Arme blanche : inclinaison de la main vers l'avant, en degrés ([Melee] weapon_tilt)
void SetMeleeTilt(float degrees);

// Poing d'une main dans le monde après la dernière pose des bras (au bout de l'avant-bras).
// false : ce bras n'a pas été posé à l'instant (manette non suivie, bras à l'animation).
bool Fist(int hand, float world[3]);

// Relevé pour le log, depuis le dernier appel : images où chaque bras a été posé, plus grand
// écart constaté entre le poignet et sa cible après la pose (mètres), images où le buste a été
// tourné et plus grand angle appliqué (degrés)
void TakeStats(unsigned posed[2], float* worstError, unsigned* torso, float* torsoWorstDegrees);

} // namespace engine::hands
