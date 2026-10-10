#pragma once
// Visée du joueur : le rayon de tir vient de la manette (ou du regard), pas de la caméra du jeu.
//
// Saints Row 2 vise avec sa caméra. Pour le joueur, une fonction calcule à chaque image le
// point visé (un lancer de rayon depuis la position de la caméra, le long de son avant ; le
// résultat est rangé dans le joueur : c'est là que vont les balles et que regarde le haut du
// corps). Elle lit la caméra dans des variables globales : position et orientation
// « finales », orientation « idéale » (celle que le mode de caméra calcule avant lissage ;
// c'est elle qui donne la direction du tir), plus un petit décalage de tangage qui place le
// réticule du jeu au-dessus du centre de l'écran.
//
// On détourne cette fonction : le temps de son exécution, et pour le joueur local seulement,
// ces variables décrivent le rayon d'une manette. Le reste du jeu (déplacement, culling, son,
// caméra) continue de voir sa caméra.
//
// Chaque main a son rayon. Le point visé rangé dans le joueur à chaque image est celui de la
// main qui vise (engine/controls) ; quand le personnage tient une arme dans chaque main, le
// rayon de l'autre main est lancé lui aussi, pour son repère de visée. Au moment d'un tir
// (fonction de tir du joueur, détournée elle aussi), le point visé est recalculé avec le rayon
// de la main qui tire, et la caméra décrit ce rayon pendant tout le tir (la balle relance son
// propre rayon depuis la caméra) : la balle va là où cette main pointe à cet instant. Avec une arme dans
// chaque main, c'est aussi là que la main qui tire est choisie : celle de la gâchette
// (engine/controls), à la place de l'alternance du jeu.
//
// Tout se passe sur le thread de simulation.

namespace engine::aim {

// Rayon de visée demandé au moment où le jeu vise. hand : 0 main gauche, 1 main droite, -1
// regard. origin : départ du rayon dans le monde ; orient : son orientation, matrice 3x3 du
// jeu (droite, haut, avant). false : ce rayon n'existe pas en ce moment (manette non suivie).
using RayFn = bool (*)(int hand, float origin[3], float orient[9]);

// cameraPos, cameraOrient : position et orientation finales de la caméra de simulation
// (trouvées par camera_hook). false (loggué) : le jeu vise avec sa caméra, comme avant.
bool Install(float* cameraPos, float* cameraOrient, RayFn ray);
void Remove();

// À chaque mise à jour de la caméra. false : le jeu vise seul (menu, cinématique, casque posé).
void SetActive(bool active);

// Dernier point visé calculé par le jeu avec un de nos rayons
struct Sight {
    float origin[3]{};  // départ du rayon
    float forward[3]{}; // sa direction
    float point[3]{};   // point visé (impact du rayon, ou bout de la portée de l'arme)
    float distance = 0; // du départ au point visé, en unités du jeu
    bool hit = false;   // le rayon a touché quelque chose avant le bout de la portée
};
// hand : 0, 1, ou -1 pour le regard. false : pas de point visé récent pour ce rayon (le jeu ne
// vise pas : mains vides, menu... ou cette manette n'est pas suivie).
bool GetSight(int hand, Sight* sight);

// Relevé pour le log, depuis le dernier appel
struct Stats {
    unsigned points = 0;   // points visés calculés avec nos rayons
    // Par main (gauche, droite) : images où le jeu a tenté un tir de cette main, tirs dont le
    // point visé a été recalculé avec son rayon, balles parties du chargeur
    unsigned calls[2]{};
    unsigned aimed[2]{};
    unsigned bullets[2]{};
    unsigned scoped[2]{}; // tirs faits avec la caméra du jeu décrivant le rayon de cette main
    unsigned deferred = 0; // dont tirs différés (déclenchés par la fin d'une animation)
};
void TakeStats(Stats* stats);

} // namespace engine::aim
