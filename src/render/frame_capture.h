#pragma once
// Copie de l'image du jeu vers le thread OpenXR.
//
// Le device du jeu est un IDirect3DDevice9 classique (pas Ex) : il ne sait pas créer de
// texture partagée avec D3D11. L'image passe donc par la mémoire : le back buffer est relu
// (GetRenderTargetData) dans le hook du Present, puis envoyé à D3D11 par le thread XR.
// Coûteux (le GPU doit finir l'image avant la lecture) : suffisant pour l'écran virtuel, à
// remplacer avant la stéréo.
#include <windows.h>

#include <cstdint>
#include <vector>

struct IDirect3DDevice9;

namespace render {

// Vue avec laquelle le jeu a rendu une image : orientation de la tête appliquée à la caméra
// (quaternion OpenXR x, y, z, w dans l'espace LOCAL) et champ de vue horizontal de l'image.
// valid = false : pas de caméra de jeu pour cette image (menu principal, chargement).
struct FrameView {
    bool valid = false;
    int eye = 0; // 0 gauche (ou image commune aux deux yeux), 1 droit
    float headQuat[4]{0.0f, 0.0f, 0.0f, 1.0f};
    float hfovDeg = 0.0f;
};

// Image BGRA 8 bits, lignes contiguës (pitch = largeur * 4)
struct CpuFrame {
    std::vector<uint8_t> pixels;
    UINT width = 0;
    UINT height = 0;
    uint64_t seq = 0; // numéro de la capture
    FrameView view;
};

// Thread XR : demande une image au prochain Present (au plus une par image du casque)
void RequestFrame();

// Thread du jeu : copie le back buffer si une image est demandée (avant le Present d'origine)
void CaptureFrame(IDirect3DDevice9* device, const FrameView& view);

// Thread XR : échange `frame` avec la dernière capture de cet œil si elle est plus récente
// que la sienne. true si `frame` contient une nouvelle image. Les images sans vue (menus)
// sont rangées avec l'œil 0.
bool TakeLatestFrame(int eye, CpuFrame& frame);

// Thread XR : nombre de captures et durée moyenne (ms) depuis le dernier appel
void TakeCaptureStats(unsigned* count, double* averageMs);

} // namespace render
