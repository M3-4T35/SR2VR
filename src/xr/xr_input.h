#pragma once
// Manettes VR dans les menus : les menus de Saints Row 2 se pilotent au clavier (pas à la
// souris), les manettes envoient donc des touches.
//   stick (gauche ou droit)          -> flèches, avec répétition tant qu'il est tenu
//   A                                -> Entrée
//   B, ou bouton menu de la gauche   -> Échap
//   gâchette gauche                  -> Ctrl (tenue)
//   gâchette droite                  -> Maj (tenue)
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_PLATFORM_WIN32
#include <windows.h>
#include <d3d11.h>
#include <openxr/openxr.h>

namespace xr {

class MenuInput {
public:
    // Crée les actions et les attache à la session (une seule fois par session). false :
    // manettes indisponibles, le reste de la VR continue.
    bool Init(XrInstance instance, XrSession session);
    // À chaque image du thread XR. focused : la session a le focus des entrées.
    void Update(XrSession session, bool focused);
    // Relâche les touches tenues (perte de focus, arrêt)
    void ReleaseAll();

private:
    struct Key {
        WORD vk = 0;
        bool held = false;
        ULONGLONG nextRepeat = 0;
    };
    void SetKey(Key& key, bool pressed, bool repeat);

    XrActionSet set_ = XR_NULL_HANDLE;
    XrAction navigate_ = XR_NULL_HANDLE;
    XrAction confirm_ = XR_NULL_HANDLE;
    XrAction back_ = XR_NULL_HANDLE;
    XrAction leftTrigger_ = XR_NULL_HANDLE;
    XrAction rightTrigger_ = XR_NULL_HANDLE;
    bool ready_ = false;
    bool announced_ = false;
    int direction_ = -1; // 0 haut, 1 bas, 2 gauche, 3 droite, -1 neutre
    Key arrows_[4];
    Key enter_;
    Key escape_;
    Key control_;
    Key shift_;
};

} // namespace xr
