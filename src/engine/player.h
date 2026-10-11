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

// Table des objets du jeu (16 octets par entrée, un objet se retrouve par son numéro) : sert
// à savoir si le joueur est dans un véhicule. Lue par le hook de caméra dans UpdateCamera.
void SetObjectTable(const void* table);

// Position du monde du repère « head » du joueur et de sa racine, orientation de la racine
// (matrice 3x3 de Volition : droite, haut, avant ; en véhicule, c'est celle du siège). false :
// pas de joueur, pas de squelette prêt, ou résultat aberrant.
bool HeadPosition(float head[3], float root[3], float orient[9]);

// p est le personnage du joueur local
bool IsLocal(const void* p);

// Le joueur est dans un véhicule (même test que la fonction Lua du jeu
// « is_local_player_in_vehicle » : numéro de véhicule du joueur en +0xD74, objet de type 5).
bool InVehicle();

// Arme en main. Le joueur garde en +0x10CC l'objet qu'il tient ; cet objet pointe en +0xC4 sur
// sa case d'inventaire : description de l'arme en +8, drapeaux en +0xC et +0xD (bit 0 de +0xD :
// l'arme est tenue en double, une dans chaque main), munitions en +0x10.
bool HoldingWeapon();
bool DualWielding();
// Classe (description+0x34) et portée en mètres (description+0xE0) de l'arme en main. false :
// mains vides.
bool WeaponInfo(int* weaponClass, float* range);

// Attaque de mêlée principale de l'arme en main (description+0x2BC, numéro dans la table des
// attaques de melee.xtbl) : -1 les mains vides, 0xFF pour une arme à feu (elle n'a qu'un coup de
// crosse en attaque secondaire), autre valeur pour une arme blanche (sabre, batte, couteau...).
// secondary : l'attaque secondaire debout (description+0x2BD), le coup de crosse des armes à feu.
int HeldMeleeAttack(bool secondary = false);

// Case d'inventaire de l'arme en main (description en +8, munitions du chargeur en +0x10),
// nullptr les mains vides
void* HeldWeaponSlot();

// Le personnage du joueur local, nullptr hors partie
void* Local();

// Son personnage animé (squelette, matrices des os), nullptr s'il n'en a pas
void* Instance();

} // namespace engine::player
