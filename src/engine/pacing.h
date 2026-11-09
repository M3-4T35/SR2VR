#pragma once
// Cadence du jeu : le frein d'images de Saints Row 2, remplacé par un simple plafond.
//
// Après chaque Present, le jeu mesure le temps écoulé depuis le précédent et, dès que la
// cadence dépasse 35 images par seconde, s'endort pendant (28,57 ms - durée de l'image) / 2
// (code en 0x00D20D5F - 0x00D20E45, dans la commande « Present » du thread de rendu). Ce n'est
// pas un plafond à 100 images par seconde mais un frein progressif : une image qui demande
// 10 ms de travail sort en 16 ms (62 par seconde au lieu de 100), une de 15 ms en 19,5 ms
// (51 au lieu de 66). En vrai relief, où chaque image de simulation coûte deux Present, le jeu
// dormait ainsi 5 à 9 ms par paire d'yeux.
//
// On remplace le Sleep du frein par notre fonction : rien tant que la cadence reste sous
// [Display] frame_cap images par seconde (100 par défaut), l'attente juste nécessaire au-delà.
// frame_cap=0 : le frein du jeu, comme avant. Juiced a la même idée en plus radical
// ([Debug] UncapFPS=1 : plus de frein ni de plafond) ; s'il est activé, il n'y a rien à faire.

namespace engine::pacing {

// false (loggué) : frein introuvable ou déjà retiré, le jeu garde sa cadence
bool Install();
void Remove();

} // namespace engine::pacing
