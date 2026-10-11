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
//   gâchette droite                  -> tirer (attaque principale, posée dans les commandes du
//                                       jeu par engine/controls)
//   gâchette gauche                  -> attaque secondaire ; avec une arme dans chaque main,
//                                       elle tire aussi et c'est la main gauche qui vise
//   poignée gauche                   -> courir (Maj, tenue)
//   X                                -> recharger / ramasser
//   clic du stick droit              -> roue des armes : elle s'ouvre, le stick droit choisit
//                                       l'arme, le relâcher la prend (manette virtuelle :
//                                       bouton B tenu, stick droit envoyé comme stick gauche)
//   clic du stick gauche             -> change de vue (yeux du personnage, caméra du jeu ;
//                                       en véhicule : conducteur, devant le pare-brise, jeu)
// Dans les menus (pause, menu principal, garde-robe, garage, boutiques) :
//   stick gauche ou droit            -> flèches, avec répétition tant qu'il est tenu
//   gâchette gauche / droite         -> Ctrl / Maj (tenues), pour les onglets des menus
//   A -> Entrée ; B, menu (« option » du Steam Frame) ou view -> Échap
// Partout : le bouton menu -> Échap (pause en partie).
// Les autres boutons, en partie, selon la disposition ([Controls] layout : 0 selon les manettes
// tenues, 1 Steam Frame, 2 Quest 2) :
//   Steam Frame (comme une manette classique) : A courir, B roue des armes, Y action (E),
//     view carte, croix haut recruter / renvoyer, gauche et droite gestes, bas abandonner
//     l'activité, bouton haut gauche otage, bouton haut droit sauter
//   Quest 2 (Touch) : Y recruter / renvoyer, A court otage, A long carte, B court geste,
//     B long abandonner l'activité
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_PLATFORM_WIN32
#include <windows.h>
#include <d3d11.h>
#include <openxr/openxr.h>

namespace xr {

// À appeler par le moteur à chaque image de jeu préparée hors pause : tant que ces appels
// arrivent, les manettes sont en mode « partie » ; sinon en mode « menus ».
void NoteGameplayFrame();

// Rayon de visée d'une manette (pose « aim » d'OpenXR ; hand : 0 gauche, 1 droite). quat :
// orientation dans le même espace LOCAL que la tête (x, y, z, w ; l'avant est -z). offset :
// départ du rayon par rapport à la tête, en mètres, dans les axes de l'espace LOCAL (x à
// droite, y en haut, z vers l'arrière). false si la manette ou la tête ne sont pas suivies
// (pose de plus de 0,3 s).
bool GetAimRay(int hand, float quat[4], float offset[3]);

// Nombre de clics du stick gauche (changement de vue) depuis le dernier appel
unsigned TakeViewCycleRequests();

// Position d'une manette par rapport à la tête (hand : 0 gauche, 1 droite), en mètres, dans
// les axes de l'espace LOCAL : x à droite, y en haut, z vers l'arrière. C'est la pose de
// prise (« grip ») : le creux de la main. false si la manette ou la tête ne sont pas suivies.
bool GetHandOffset(int hand, float offset[3]);

class MenuInput {
public:
    // Crée les actions et les attache à la session (une seule fois par session). false :
    // manettes indisponibles, le reste de la VR continue.
    bool Init(XrInstance instance, XrSession session);
    // Avant Init : le runtime connaît le profil des manettes du Steam Frame (extension activée)
    void SetFrameProfile(bool available) { frameProfile_ = available; }
    // À chaque image du thread XR. focused : la session a le focus des entrées. base, time :
    // espace et instant d'affichage pour situer les manettes. head : pose de la tête dans le
    // même espace, nullptr si elle n'est pas suivie.
    void Update(XrSession session, bool focused, XrSpace base, XrTime time, const XrPosef* head);
    // Relâche les touches tenues (perte de focus, arrêt)
    void ReleaseAll();
    // Pose « aim » d'une manette relevée par le dernier Update, dans son espace `base` (hand :
    // 0 gauche, 1 droite). nullptr si elle n'est pas suivie.
    const XrPosef* AimPose(int hand) const { return aimPoseValid_[hand] ? &aimPoseLocal_[hand] : nullptr; }
    // Les manettes pilotent le personnage (le jeu prépare des images hors pause)
    bool Gameplay() const { return gameplayShown_ == 1; }

private:
    struct Key {
        WORD code = 0;
        bool scan = false; // code de balayage (position de la touche) au lieu d'un code VK_*
        bool held = false;
        ULONGLONG nextRepeat = 0;
    };
    void SetKey(Key& key, bool pressed, bool repeat);
    void SetMouseButton(int button, bool pressed); // 0 gauche, 1 droit
    void ReadConfig();

    XrActionSet set_ = XR_NULL_HANDLE;
    XrAction leftStick_ = XR_NULL_HANDLE;
    XrAction rightStick_ = XR_NULL_HANDLE;
    // Boutons, liés pour ce qu'ils sont ; leur rôle en partie dépend de la disposition
    XrAction btnA_ = XR_NULL_HANDLE;
    XrAction btnB_ = XR_NULL_HANDLE;
    XrAction btnX_ = XR_NULL_HANDLE;
    XrAction btnY_ = XR_NULL_HANDLE;
    XrAction btnMenu_ = XR_NULL_HANDLE; // menu gauche (Touch) ou « option » à droite (Steam Frame)
    XrAction btnView_ = XR_NULL_HANDLE; // Steam Frame seulement
    XrAction dpad_[4]{XR_NULL_HANDLE, XR_NULL_HANDLE, XR_NULL_HANDLE, XR_NULL_HANDLE}; // haut, bas, gauche, droite
    XrAction bumper_[2]{XR_NULL_HANDLE, XR_NULL_HANDLE};                               // gauche, droite
    XrAction leftTrigger_ = XR_NULL_HANDLE;
    XrAction rightTrigger_ = XR_NULL_HANDLE;
    XrAction rightGrip_ = XR_NULL_HANDLE;
    XrAction aimPose_ = XR_NULL_HANDLE;     // main droite
    XrAction leftAimPose_ = XR_NULL_HANDLE;
    XrAction leftClick_ = XR_NULL_HANDLE;
    XrAction rightClick_ = XR_NULL_HANDLE;
    XrAction leftGrip_ = XR_NULL_HANDLE;
    XrSpace aimSpace_[2]{XR_NULL_HANDLE, XR_NULL_HANDLE}; // gauche, droite
    XrPosef aimPoseLocal_[2]{};
    bool aimPoseValid_[2]{};
    XrAction gripPose_[2]{XR_NULL_HANDLE, XR_NULL_HANDLE}; // gauche, droite
    XrSpace gripSpace_[2]{XR_NULL_HANDLE, XR_NULL_HANDLE};
    // Vitesse des mains (coups portés au geste) : celle du runtime, sinon l'écart de position
    XrVector3f gripLast_[2]{};
    bool gripSeen_[2]{};
    XrAction haptic_[2]{XR_NULL_HANDLE, XR_NULL_HANDLE}; // vibration, gauche et droite
    bool frameProfile_ = false;
    // Disposition des boutons en partie ([Controls] layout) : 0 selon les manettes, 1 Steam Frame
    // (proche d'une manette classique), 2 Quest 2 (Touch : moins de boutons, appuis courts et longs)
    int layout_ = 0;
    bool frameLayout_ = false;   // disposition Steam Frame en vigueur
    int layoutShown_ = -1;
    XrPath framePath_ = XR_NULL_PATH;
    XrPath rightHandPath_ = XR_NULL_PATH;
    ULONGLONG lastProfileCheck_ = 0;
    // Appui court / appui long d'un bouton (disposition Quest 2)
    struct Hold {
        bool down = false;
        bool fired = false;
        ULONGLONG since = 0;
    };
    int ShortLong(Hold& hold, bool down, ULONGLONG now); // 1 : appui court relâché, 2 : appui long atteint
    Hold holdA_, holdB_;
    // Boutons : enfoncé, et mode (partie ou menus) au moment de l'appui. Un bouton n'agit que
    // dans le mode où il a été enfoncé : celui qui ferme un menu n'agit pas dans la partie qui
    // reprend, celui qui ouvre la carte ne la referme pas aussitôt.
    enum Button { kA, kB, kX, kY, kMenu, kView, kUp, kDown, kLeft, kRight, kBumperLeft, kBumperRight, kButtonCount };
    bool buttonDown_[kButtonCount]{};
    bool buttonGame_[kButtonCount]{};
    ULONGLONG pulse_[16]{};      // commandes du jeu tenues jusqu'à cet instant (un appui bref)
    bool stickJump_ = false;     // saut commandé au stick droit (bloque la rotation)
    bool ready_ = false;
    bool announced_ = false;
    int gameplayShown_ = -1; // dernier mode annoncé dans le log (0 menus, 1 partie)
    bool firedLogged_ = false; // premier tir à la gâchette annoncé dans le log
    // Tir : 0 commandes posées dans le jeu, 1 boutons de la souris, 2 manette virtuelle
    // ([Controls] fire_mode)
    int fireMode_ = 0;
    // Accroupi tant que le stick droit est tenu vers le bas ([Controls] crouch_hold) : on peut
    // alors tourner en l'inclinant de côté sans le relâcher
    bool crouchHold_ = true;
    bool mouseHeld_[2]{};
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
    // click : clic du stick droit (armes) ; foodClick : clic du stick gauche (nourriture et
    // soins, choisis au stick gauche par la croix de la manette virtuelle). Renvoie true quand
    // la roue de la nourriture, tenue sans rien choisir, vaut un changement de vue.
    bool UpdateWheel(bool gameplay, bool click, bool foodClick, float lx, float ly, float rx, float ry, ULONGLONG now);
    bool wheelOpen_ = false;
    bool wheelFood_ = false;         // la roue ouverte est celle de la nourriture
    int wheelDpad_ = -1;             // croix tenue : bouton de la manette virtuelle, -1 aucune
    ULONGLONG wheelOpenTick_ = 0;
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

    // Rotation par crans ([Controls] turn_mode=1, snap_angle) : une poussée du stick = un cran.
    // Le cran part en un déplacement de souris ; le lacet de la caméra du jeu, relu ensuite,
    // dit de combien le jeu a réellement tourné : le rapport pixels / degré s'apprend ainsi, et
    // l'écart restant est rattrapé par un envoi de plus.
    void UpdateSnap(float rx, float ry, bool blocked, ULONGLONG now);
    void SendSnapBurst(float degrees, float yaw, unsigned sequence);
    void FinishSnap(const char* why, float yaw, ULONGLONG now);
    int turnMode_ = 0;       // 0 continue, 1 par crans
    float snapAngle_ = 30.0f;
    float pixelsPerDegree_ = 0.0f; // appris en jouant, gardé dans [Learned] turn_pixels_per_degree
    bool snapLearned_ = false;     // false : valeur supposée, le premier cran commence par un essai
    bool snapLoaded_ = false;      // la valeur gardée a été relue
    float snapSaved_ = 0.0f;       // dernière valeur écrite dans le fichier
    bool snapArmed_ = true;  // le stick est revenu au centre depuis le dernier cran
    struct Snap {
        bool active = false;
        bool closed = false;    // le lacet du jeu est mesuré : écart rattrapé
        float target = 0.0f;    // degrés voulus, positif = droite
        float startYaw = 0.0f;
        float burstYaw = 0.0f;  // lacet au moment du dernier envoi
        unsigned burstSequence = 0;
        int burstPixels = 0;
        float burstDegrees = 0.0f; // ce que cet envoi devait faire tourner
        ULONGLONG burstTick = 0;
        float lastYaw = 0.0f;
        unsigned lastSequence = 0;
        int bursts = 0;
        int pixels = 0;
        ULONGLONG startTick = 0;
    } snap_;
    unsigned snapCount_ = 0;
    LARGE_INTEGER lastUpdate_{};
    ULONGLONG lastConfig_ = 0;
};

} // namespace xr
