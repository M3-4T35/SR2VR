#pragma once
// Interface du jeu en partie sur un calque à part.
//
// En vrai relief, le jeu dessine son interface (vie, carte, argent, messages) une seule fois par
// instant du jeu : elle n'apparaît que dans l'image de l'œil gauche. Le jeu la dessine en
// dernier, dans le back buffer, juste après avoir recopié la scène finie (un ou deux dessins
// dont la texture est une cible de rendu de la taille de l'écran). À partir de ce moment, et
// jusqu'au Present, la cible de rendu est remplacée par une texture transparente à nous :
// l'interface n'est plus dans l'image d'aucun œil, et le casque montre cette texture par-dessus
// les deux yeux (xr_session), un peu décalée d'un œil à l'autre pour la poser à distance.
//
// [Hud] panel : 1 = calque (par défaut), 0 = comme avant (interface dans l'image de l'œil gauche).
// [Hud] panel_rate : lectures du calque par seconde (15 par défaut).
#include "render/frame_capture.h"

struct IDirect3DDevice9;

namespace render::ui_panel {

// Thread qui appelle Direct3D, dans le Present, avant la capture de l'image. `view` : la vue
// de l'image présentée.
void OnPresent(IDirect3DDevice9* device, const FrameView& view);

// Même thread, avant un Reset du device
void ReleaseResources();

// Retrait de la DLL
void Remove();

// Thread de simulation : l'interface est-elle redirigée en ce moment (elle n'a alors plus à
// être décalée d'un œil à l'autre) ?
bool Active();

} // namespace render::ui_panel
