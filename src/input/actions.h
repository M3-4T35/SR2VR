#pragma once
// Demandes des manettes VR au jeu, déposées par le thread XR et lues par le moteur.
//
// Les gâchettes ne passent plus par un clavier ou une manette simulés : le moteur
// (engine/controls) pose leur état directement dans les commandes du jeu, à chaque image de
// simulation. Ce module n'est que la boîte aux lettres entre les deux threads ; il ne dépend
// ni d'OpenXR ni du jeu.

namespace input::actions {

enum Action {
    kTriggerLeft,  // gâchette gauche enfoncée
    kTriggerRight, // gâchette droite enfoncée
    kReload,       // recharger / ramasser
    kCount
};

// Thread XR
void Set(Action action, bool down);
void ReleaseAll();

// Thread de simulation
bool Get(Action action);
// Nombre d'appuis (passages de relâché à enfoncé) depuis le dernier appel. La simulation tourne
// bien moins vite que le casque : un appui bref peut commencer et finir entre deux de ses
// images, et ne se verrait pas dans Get. Lire Get d'abord, puis TakePresses : un appui en
// cours d'écriture est alors compté une fois, jamais deux.
unsigned TakePresses(Action action);

// Le moteur sait poser les commandes dans le jeu : les manettes n'ont alors rien à simuler
// pour le tir. Faux tant que le moteur n'est pas installé (ou hors du jeu : banc d'essai).
void SetDirect(bool ready);
bool Direct();

// Le personnage tient une arme dans chaque main (dit par le moteur à chaque image)
void SetDualWield(bool dual);
bool DualWield();

// Lacet de la caméra du jeu elle-même (sans le casque), en degrés, croissant vers la droite :
// publié par le moteur à chaque image de simulation, lu par la rotation par crans pour mesurer
// de combien le jeu a réellement tourné. steady : le personnage est à pied, la caméra ne tourne
// que sur commande (en véhicule elle suit aussi la route).
void SetGameYaw(float degrees, bool steady);
// false sans mesure récente (hors du jeu, pause). sequence : numéro de la mesure.
bool GameYaw(float* degrees, unsigned* sequence, bool* steady);

} // namespace input::actions
