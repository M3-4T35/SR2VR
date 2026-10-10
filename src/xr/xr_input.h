#pragma once
// Manettes VR. Saints Row 2 se pilote au clavier et à la souris : les manettes envoient donc
// des touches et des mouvements de souris.
//
// En partie (le jeu prépare des images et n'est pas en pause) :
//   stick gauche                     -> déplacement (touches aux positions W A S D, donc
//                                       Z Q S D sur un clavier AZERTY)
//   stick droit, gauche / droite     -> tourner (mouvement horizontal de la souris)
//   poignée droite                   -> interagir (E)
// Dans les menus (pause, menu principal) :
//   stick gauche ou droit            -> flèches, avec répétition tant qu'il est tenu
// Partout :
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

// À appeler par le moteur à chaque image de jeu préparée hors pause : tant que ces appels
// arrivent, les manettes sont en mode « partie » ; sinon en mode « menus ».
void NoteGameplayFrame();

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
        WORD code = 0;
        bool scan = false; // code de balayage (position de la touche) au lieu d'un code VK_*
        bool held = false;
        ULONGLONG nextRepeat = 0;
    };
    void SetKey(Key& key, bool pressed, bool repeat);
    void ReadConfig();

    XrActionSet set_ = XR_NULL_HANDLE;
    XrAction leftStick_ = XR_NULL_HANDLE;
    XrAction rightStick_ = XR_NULL_HANDLE;
    XrAction confirm_ = XR_NULL_HANDLE;
    XrAction back_ = XR_NULL_HANDLE;
    XrAction leftTrigger_ = XR_NULL_HANDLE;
    XrAction rightTrigger_ = XR_NULL_HANDLE;
    XrAction rightGrip_ = XR_NULL_HANDLE;
    bool ready_ = false;
    bool announced_ = false;
    int gameplayShown_ = -1; // dernier mode annoncé dans le log (0 menus, 1 partie)
    int direction_ = -1;     // menus : 0 haut, 1 bas, 2 gauche, 3 droite, -1 neutre
    Key arrows_[4];
    Key move_[4]; // avant, arrière, gauche, droite
    Key enter_;
    Key escape_;
    Key control_;
    Key shift_;
    Key interact_;

    // Tourner : pixels de souris par seconde, stick à fond ([Controls] turn_speed)
    float turnSpeed_ = 1200.0f;
    float turnRemainder_ = 0.0f;
    LARGE_INTEGER lastUpdate_{};
    ULONGLONG lastConfig_ = 0;
};

} // namespace xr
