#pragma once
// Commandes du joueur posées directement dans le jeu.
//
// Saints Row 2 range l'état de ses 87 entrées (touche du clavier ou bouton de manette, selon
// les réglages du joueur) dans un tableau de cases de 36 octets : valeur courante (float, 0 à
// 1) en +0, valeur de l'image précédente en +4. Une fois par image de simulation, une fonction
// relit le clavier, la souris et la manette et remplit ce tableau. Les commandes du joueur
// (« attaque principale », « recharger »...) sont une seconde table, 132 entrées de 16 octets,
// qui dit pour chacune quelle case lire.
//
// On détourne la fonction de lecture : après elle, la case de chaque commande demandée par les
// manettes VR passe à 1. Le jeu ne voit ni clavier ni manette simulés : pas de touche à
// connaître, pas de fenêtre à garder au premier plan, et l'aide à la visée que Juiced réserve à
// la manette reste coupée.
//
// Gâchettes : la droite est l'attaque principale. La gauche est l'attaque secondaire (coup de
// crosse, lancer), sauf quand le personnage tient une arme dans chaque main : chaque gâchette
// tire alors avec son arme (engine/aim, qui détourne le tir du joueur, demande la main à
// FireHand).

namespace engine::controls {

// Cherche la lecture des entrées et les deux tables, détourne la lecture. false (loggué) : les
// manettes gardent leurs touches et leur manette simulées.
bool Install();
void Remove();

// Main qui vise pour le haut du corps : 1 droite, 0 gauche (seulement à pied avec une arme dans
// chaque main, quand la gauche a tiré en dernier). Thread de simulation.
int AimHand();

// Une arme dans chaque main, à pied : main qui doit tirer à cet appel du tir du joueur (0 gauche,
// 1 droite). Une seule gâchette tenue : sa main ; les deux : chacune à son tour. Thread de
// simulation.
int FireHand();

} // namespace engine::controls
