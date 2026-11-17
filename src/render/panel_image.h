#pragma once
// Dernière image du panneau de l'interface (render/ui_panel), passée du thread du rendu au
// thread du casque. BGRA 8 bits, couleurs déjà multipliées par l'alpha.
#include "render/frame_capture.h"

namespace render {

// Thread du jeu : `height` lignes de `pitch` octets, recopiées, avec la vue de l'image de l'œil
// gauche dont l'interface a été retirée
void PublishPanel(const uint8_t* pixels, int pitch, UINT width, UINT height, const FrameView& view);

// Thread du casque : copie l'image dans `frame` si son numéro diffère de `since` (0 : toujours,
// s'il y en a une). tick : GetTickCount64 de sa publication.
bool TakePanel(CpuFrame& frame, uint64_t since, ULONGLONG* tick);

// Compteurs du calque (thread du jeu) : image de l'œil gauche vue, image redirigée (avec le
// temps pris, en microsecondes), image relue
enum class PanelEvent { LeftFrame, Redirected, Read };
void CountPanel(PanelEvent event, long long microseconds = 0);

// Thread du casque, relevé depuis le dernier appel : images de l'œil gauche vues, redirigées,
// relues, temps moyen pris au thread du jeu par image redirigée (ms)
void TakePanelStats(unsigned* left, unsigned* redirected, unsigned* read, double* averageMs);

} // namespace render
