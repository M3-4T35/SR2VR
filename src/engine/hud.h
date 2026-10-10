#pragma once
// Interface du jeu en partie (vie, carte, armes, messages, roue des armes).
//
// L'interface de Saints Row 2 est faite de documents « vint » pilotés en Lua, dessinés dans
// un repère de 1280 x 720. Chaque document range ses éléments dans un groupe « safe_frame » :
// lui donner une échelle et une position déplace tout le document d'un coup (c'est ainsi que
// Juiced recentre l'interface sur les écrans très larges). En vue immersive, l'image couvre
// plus de 120° : les éléments posés aux bords de l'écran sortent du champ du casque. On
// rapetisse donc le document autour du centre de l'image, et en relief on le décale un peu
// d'un œil à l'autre pour qu'il flotte à quelques mètres au lieu de l'infini.
//
// À n'appeler que depuis le thread de simulation.

namespace engine::hud {

// Cherche la fonction qui exécute du Lua et l'état Lua de l'interface. false (loggué) :
// l'interface reste telle que le jeu la dessine.
bool Install();

// Échelle du document (1 = taille du jeu) et position de son coin haut gauche dans le repère
// 1280 x 720. Sans effet si l'interface n'est pas chargée.
void Place(float scale, float x, float y);

// Montre ou cache le réticule du jeu (le groupe « reticules » du document, dont le script de
// l'interface garde la poignée dans Hud_reticules.elements.reticule_h). Caché quand le mod
// dessine son propre repère de visée : celui du jeu reste au centre de l'interface, alors que
// le tir suit la manette.
void ShowReticle(bool visible);

} // namespace engine::hud
