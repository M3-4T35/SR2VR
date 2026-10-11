#pragma once
// Messages du jeu : « appuyez sur [touche] » montre le bouton de la manette VR.
//
// Les textes du jeu citent une commande (« [idx:45] ») ; pour l'afficher, le jeu demande à une
// fonction le dessin de la touche du clavier qui lui est attribuée. On détourne cette fonction :
// quand une manette VR fait le même travail (selon la disposition des boutons en vigueur), elle
// rend le nom de ce bouton à la place : une touche dessinée pour A, B, X, Y, un texte vert pour
// le reste (gâchette, poignée, stick...). Les commandes sans bouton VR gardent leur touche.
// [Hud] prompts : 0 touches du clavier (le jeu d'origine), 1 boutons des manettes.

namespace engine::prompts {

// Après controls::Install (qui connaît la case d'entrée de chaque commande). false (loggué) :
// les messages gardent les touches du clavier.
bool Install();
void Remove();

} // namespace engine::prompts
