#pragma once
// Script d'entrées pour les essais sans casque : si `sr2_vr_test.txt` existe à côté de la DLL,
// ses commandes sont rejouées une fois, par les mêmes chemins que les manettes VR (manette
// virtuelle, clavier). Absent : rien ne se passe. Posé et retiré par tools/smoke_test.ps1.
//
// Une commande par ligne, `#` = commentaire :
//   wait <ms>               attendre
//   pad <bouton> [ms]       bouton de la manette virtuelle, tenu ms (150 par défaut) : a b x y
//                           back start ls rs lb rb up down left right
//   axis <axe> <valeur> [ms]  stick ou gâchette tenu ms (500 par défaut) : lx ly rx ry lt rt,
//                           sticks de -1 à 1 (y vers le bas), gâchettes de 0 à 1
//   key <vk> [ms]           touche du clavier (code VK décimal ou 0x..), tenue ms (100 par défaut)
//   shot                    capture des deux yeux (captures\essai_NN_gauche.bmp / _droit.bmp),
//                           seulement quand un casque, même simulé, reçoit les images
//   log <texte>             ligne dans le journal

namespace input {

void StartTestScript();

// Thread du casque : true une fois par commande `shot`, avec le numéro de la capture
bool TakeShotRequest(int* number);

} // namespace input
