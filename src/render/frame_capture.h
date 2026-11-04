#pragma once
// Copie de l'image du jeu vers le thread OpenXR.
//
// Le device du jeu est un IDirect3DDevice9 classique (pas Ex) : il ne sait pas créer de
// texture partagée avec D3D11. L'image passe donc par la mémoire : le back buffer est relu
// (GetRenderTargetData) dans le hook du Present, puis envoyé à D3D11 par le thread XR.
// Par défaut la relecture est asynchrone ([Display] async_capture=1) : copie GPU, transfert et
// événement lancés au Present, image lue à un Present suivant sans attendre le GPU (détail
// dans frame_capture.cpp). async_capture=0 : relecture synchrone, le jeu attend le GPU.
#include "render/sight_overlay.h"

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
    // Vrai relief : numéro de la paire (les deux yeux du même instant du jeu, rendus l'un
    // après l'autre, œil gauche d'abord). 0 : image seule.
    unsigned pair = 0;
    float headQuat[4]{0.0f, 0.0f, 0.0f, 1.0f};
    float hfovDeg = 0.0f;
    SightOverlay sight; // repère de visée à dessiner dans cette image
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

// Thread du jeu : copie le back buffer si une image est demandée (avant le Present d'origine).
// Les deux images d'une paire vont ensemble : l'œil gauche attend son œil droit, qui est
// capturé d'office, et les deux sont publiés en même temps.
void CaptureFrame(IDirect3DDevice9* device, const FrameView& view);

// Thread du jeu, avant un Reset du device : libère les surfaces D3DPOOL_DEFAULT de la capture
// (un Reset échoue tant qu'il en reste) et abandonne les copies en vol
void ReleaseCaptureResources();

// Thread XR : échange `left` et `right` avec la dernière capture de chaque œil quand elle est
// plus récente que la leur, les deux d'un seul geste (jamais une demi-paire). fresh[i] : l'œil
// i a reçu une nouvelle image. Les images sans vue (menus) sont rangées avec l'œil gauche.
void TakeLatestFrames(CpuFrame& left, CpuFrame& right, bool fresh[2]);

// Relevé des captures depuis le dernier appel
struct CaptureStats {
    unsigned count = 0;        // captures publiées
    double averageMs = 0.0;    // temps moyen pris au thread du jeu par capture
    unsigned async = 0;        // captures asynchrones publiées
    double averageLatency = 0.0; // asynchrone : Present écoulés en moyenne entre copie et publication
    unsigned waits = 0;        // asynchrone : copies attendues (pas finies à temps)
    unsigned pairs = 0;        // paires publiées
    unsigned brokenPairs = 0;  // paires restées incomplètes (œil droit jamais présenté)
    // Par œil, pour les images qui ont une vue : nombre d'images et luminosité moyenne
    // (0 à 255, sur un échantillon de pixels). Une image vide se voit à sa luminosité nulle.
    unsigned eyeCount[2]{};
    double eyeLuma[2]{};
};

// Thread XR
void TakeCaptureStats(CaptureStats* stats);

} // namespace render
