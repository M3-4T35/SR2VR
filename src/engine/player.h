#pragma once
// Le personnage du joueur : où sont sa racine et sa tête dans le monde.
//
// Le joueur est un objet du jeu (pointeur global, « getplayer » dans les sources de Juiced) :
// position de sa racine en +0x14, orientation (matrice 3x3 de Volition) en +0x20, personnage
// animé en +0x5E0. Son squelette porte des os et des repères nommés (« tags ») ; l'objet garde
// l'indice des plus utiles, dont le repère « head » en +0xCBE. Une fonction du jeu donne la
// position du monde d'un os ou d'un repère à partir de la racine : c'est elle que la caméra
// à la 3e personne appelle pour viser l'os « camTarget ».
//
// À n'appeler que depuis le thread de simulation (là où le jeu l'appelle lui-même).

namespace engine::player {

// Cherche le pointeur du joueur et la fonction des repères. false (loggué) : pas de vue à la
// première personne.
bool Install();

// Position du monde du repère « head » du joueur et de sa racine. false : pas de joueur, pas
// de squelette prêt, ou résultat aberrant.
bool HeadPosition(float head[3], float root[3]);

} // namespace engine::player
