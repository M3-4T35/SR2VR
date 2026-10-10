#pragma once
// Manettes VR. Saints Row 2 se pilote au clavier et à la souris : les manettes envoient donc
// des touches et des mouvements de souris.
//
// En partie (le jeu prépare des images et n'est pas en pause) :
//   stick gauche                     -> déplacement (touches aux positions W A S D, donc
//                                       Z Q S D sur un clavier AZERTY)
//   stick droit, gauche / droite     -> tourner (mouvement horizontal de la souris)
//   stick droit, haut / bas          -> sauter (Espace) / s'accroupir (C)
//   poignée droite                   -> interagir (E)
//   clic du stick droit              -> roue des armes : elle s'ouvre, le stick droit choisit
//                                       l'arme, le relâcher la prend (manette virtuelle :
//                                       bouton B tenu, stick droit envoyé comme stick gauche)
//   clic du stick gauche             -> change de vue (yeux du personnage, caméra du jeu ;
//                                       en véhicule : conducteur, devant le pare-brise, jeu)
// Dans les menus (pause, menu principal, garde-robe, garage, boutiques) :
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

// Direction pointée par la manette droite (pose de visée OpenXR), dans le même espace LOCAL
// que la tête : quaternion x, y, z, w, l'avant est -z. false si la manette n'est pas suivie
// (pose de plus de 0,3 s).
bool GetAimPose(float quat[4]);

// Nombre de clics du stick gauche (changement de vue) depuis le dernier appel
unsigned TakeViewCycleRequests();

class MenuInput {
public:
    // Crée les actions et les attache à la session (une seule fois par session). false :
    // manettes indisponibles, le reste de la VR continue.
    bool Init(XrInstance instance, XrSession session);
    // À chaque image du thread XR. focused : la session a le focus des entrées. base, time :
    // espace et instant d'affichage pour situer la manette droite.
    void Update(XrSession session, bool focused, XrSpace base, XrTime time);
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
    XrAction aimPose_ = XR_NULL_HANDLE;
    XrAction leftClick_ = XR_NULL_HANDLE;
    XrAction rightClick_ = XR_NULL_HANDLE;
    XrSpace aimSpace_ = XR_NULL_HANDLE;
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
    Key jump_;
    Key crouch_;

    // Roue des armes, par la manette virtuelle
    void UpdateWheel(bool gameplay, bool click, float rx, float ry, ULONGLONG now);
    bool wheelOpen_ = false;
    bool wheelPointed_ = false;      // le stick a désigné une arme depuis l'ouverture
    float wheelDir_[2]{};            // dernière direction désignée (x droite, y haut)
    ULONGLONG wheelCloseTick_ = 0;   // bouton relâché : le stick reste pointé jusque-là
    bool wheelWaitCenter_ = false;   // roue fermée stick encore incliné : il ne commande rien avant son retour au centre
    bool rightClickHeld_ = false;
    bool leftClickHeld_ = false;
    int wheelButton_ = 1;            // bouton de la manette qui ouvre la roue ([Controls] wheel_button)
    bool wheelToggle_ = true;        // 1 : un clic ouvre, relâcher le stick choisit ; 0 : tenir le clic
    ULONGLONG lastAttach_ = 0;

    // Tourner : pixels de souris par seconde, stick à fond ([Controls] turn_speed)
    float turnSpeed_ = 1200.0f;
    float turnRemainder_ = 0.0f;
    LARGE_INTEGER lastUpdate_{};
    ULONGLONG lastConfig_ = 0;
};

} // namespace xr
