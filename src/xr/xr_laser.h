#pragma once
// Repère de visée des manettes, dessiné par le casque : un trait qui part de la manette et un
// point à l'endroit visé.
//
// Ce sont des calques OpenXR (des rectangles posés dans l'espace LOCAL, par-dessus l'image du
// jeu) : le compositeur les replace à chaque image du casque, avec la pose de la manette de
// cet instant. Le trait reste donc collé à la main, même quand le jeu ne rend que 20 images
// par seconde. Le moteur ne fournit que la distance du point visé, mesurée par le lancer de
// rayon du jeu (engine/aim) ; la direction vient de la pose « aim » de la manette, la même que
// celle donnée au jeu.
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_PLATFORM_WIN32
#include <windows.h>
#include <d3d11.h>
#include <openxr/openxr.h>

#include <cstdint>

namespace xr {

// Thread de simulation, à chaque image du jeu. hand : 0 gauche, 1 droite. visible : le jeu vise
// avec le rayon de cette manette. distance : de la manette au point visé, en mètres. hit : le
// rayon touche quelque chose (sinon le point est au bout de la portée : pas de point dessiné).
void SetSight(int hand, bool visible, float distance, bool hit);

class Laser {
public:
    static constexpr uint32_t kMaxLayers = 4; // trait et point, pour chaque main

    // Crée les deux petites textures (trait, point). false : pas de repère, le reste continue.
    bool Init(XrSession session, ID3D11DeviceContext* context, int64_t format);
    void Destroy();

    // Calques des repères visibles pour cette image du casque. head : pose de la tête ; aim :
    // pose « aim » de chaque manette, nullptr si elle n'est pas suivie (toutes dans `space`).
    // Renvoie le nombre de calques écrits dans out.
    uint32_t Build(XrSpace space, const XrPosef& head, const XrPosef* const aim[2], XrCompositionLayerQuad out[kMaxLayers]);

    // Relevé pour le log : images du casque où le repère de chaque main a été dessiné depuis le
    // dernier appel
    void TakeStats(unsigned shown[2]);

private:
    struct Texture {
        XrSwapchain swapchain = XR_NULL_HANDLE;
        int32_t width = 0, height = 0;
    };
    bool CreateTexture(XrSession session, ID3D11DeviceContext* context, int64_t format, int32_t width, int32_t height,
                       const uint8_t* bgra, Texture* texture);
    void ReadConfig();

    Texture beam_, dot_;
    bool ready_ = false;
    int mode_ = 1;          // [Hands] laser : 0 aucun, 1 trait et point, 2 point seul
    float length_ = 3.0f;   // longueur du trait, en mètres ([Hands] laser_length)
    ULONGLONG lastConfig_ = 0;
    unsigned shown_[2]{};
};

} // namespace xr
