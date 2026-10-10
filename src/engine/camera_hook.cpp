// Caméra de Saints Row 2.
//
// Le jeu a deux threads : la simulation et le rendu.
//
// Simulation. La caméra est un ensemble de variables globales, mises à jour une fois par
// image par une fonction sans argument (« UpdateCamera » dans les sources de Juiced,
// 0x004932F0 dans l'exe modifié par Juiced) : le mode de caméra courant calcule une position
// et une orientation idéales, puis la fonction les recopie dans la position et l'orientation
// finales, lues par le culling, le son et la visée. L'orientation est une matrice 3x3 de
// Volition : trois vecteurs du monde, droite (rvec), haut (uvec), avant (fvec), repère main
// gauche, Y vers le haut. On détourne cette fonction : après l'original, l'orientation finale
// est composée avec celle du casque ; avant l'appel suivant on rend au jeu sa propre
// orientation, pour qu'un mode de caméra ne cumule pas la rotation de la tête.
//
// Rendu. Au début de chaque image, le thread du rendu recopie la caméra finale dans la caméra
// Rendu. À la fin de son image, la simulation prépare l'image à dessiner : elle recopie la
// caméra finale dans la caméra de rendu (position, orientation), appelle GetFOV pour le champ
// de vue, puis confie l'image au thread du rendu, qui la dessine et la présente pendant que la
// simulation calcule déjà la suivante. C'est juste après cette copie (dans GetFOV, reconnu par
// son adresse de retour) qu'on écrit la caméra réellement utilisée : orientation du jeu + tête
// au dernier moment, décalage de l'œil en relief, champ de vue. La vue ainsi décidée est mise
// en file et ressort au Present de la même image : au Present de l'image N, la caméra de
// l'image N+1 peut déjà avoir été préparée.
//
// Vrai relief ([Display] mode=3). La fonction qui prépare une image (elle attend le thread du
// rendu, recopie la caméra, parcourt la scène puis lui confie l'image) est détournée et
// appelée deux fois par image de simulation : œil gauche puis œil droit, avec la même pose de
// tête. Les deux yeux montrent donc le même instant du jeu. Chaque appel donne un Present ;
// les deux images portent le même numéro de paire et sont livrées ensemble au casque.
// Entre les deux appels, deux choses sont remises comme au début de l'image :
// - la « caméra en attente » : pendant l'image de simulation, le jeu y note ce que le rendu
//   devra faire (dont un octet « dessiner la scène ») ; la préparation la recopie dans la
//   caméra de rendu puis la remet à zéro. Sans la restaurer, la deuxième image sort noire
//   (constaté le 2026-10-10 : œil droit de luminosité nulle).
// - le compteur d'images du jeu, qui avance d'un cran comme entre deux images de simulation :
//   la préparation ne libère le thread du rendu que si ce compteur a changé.
//
// Menus et cinématiques : on ne touche à rien, et l'image n'a pas de vue, donc elle s'affiche
// sur l'écran virtuel. Trois cas : le jeu en pause (menu de pause et autres menus qui arrêtent
// le jeu) ; les interfaces qui laissent le jeu tourner (garde-robe, garage, boutiques,
// personnalisation de la planque ou du gang), reconnues à un octet que leur fonction d'entrée
// commune met à 0 et leur fonction de sortie à 1 ; les cinématiques, reconnues à l'octet que
// leur lancement met à 1 et leur fin à 0 (« IsInCutscene » dans les sources de Juiced).
//
// Première personne ([FirstPerson] enabled=1). Après la mise à jour de la caméra par le jeu,
// la position finale est remplacée par celle des yeux du personnage : la racine du joueur, plus
// l'écart racine -> repère « head » de son squelette (filtré, pour ne pas subir le balancement
// des animations), plus un petit décalage vers l'avant et vers le haut. L'orientation reste
// celle de la caméra du jeu (son cap) composée avec la tête. Comme pour l'orientation, le jeu
// retrouve sa propre position avant sa mise à jour suivante. La préparation de l'image recopie
// la position finale dans la caméra de rendu : le rendu suit sans rien de plus.
// Vues, changées par le clic du stick gauche : à pied, yeux du personnage ou caméra du jeu ; en
// véhicule, yeux du conducteur, devant le pare-brise (certaines voitures ont des vitres teintées
// opaques de l'intérieur) ou caméra du jeu.
//
// Interface en partie ([Hud]) : en vue immersive, le document de l'interface est rapetissé
// autour du centre de l'image (engine/hud) pour rentrer dans le champ du casque, et décalé
// d'un œil à l'autre pour flotter à quelques mètres. Le décalage de l'œil est posé juste avant
// la préparation de son image.
//
// Visée ([Hands] aim, laser). La caméra de simulation suit le regard : c'est lui qui donne le
// sens de la marche. Le tir, lui, suit un rayon à part (engine/aim) : celui de la manette qui
// tire, ou celui du regard si elle n'est pas suivie. Ce module fournit ces rayons (AimRay) et
// publie pour chaque main la distance du point visé : le casque dessine alors lui-même un
// trait depuis la manette et un point à l'impact (xr/xr_laser). L'ancien dessin dans l'image
// du jeu (render/sight_overlay) reste disponible avec laser=3.
//
// Aucune adresse codée en dur : fonctions trouvées par signature, adresses des variables lues
// dans leur code.
#include "engine/camera_hook.h"
#include "engine/aim.h"
#include "engine/controls.h"
#include "engine/hands.h"
#include "engine/hud.h"
#include "engine/melee.h"
#include "engine/player.h"
#include "input/actions.h"
#include "util/config.h"
#include "util/log.h"
#include "util/pattern.h"
#include "xr/xr_input.h"
#include "xr/xr_laser.h"
#include "xr/xr_session.h"

#include <MinHook.h>
#include <intrin.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace engine {
namespace {

// Début de UpdateCamera : mov eax,[handle de l'objet suivi] ; sub esp,8Ch ; push ebx ; push ebp ;
// xor ebx,ebx ; cmp eax,ebx ; push esi ; push edi ; je ... ; mov ecx,eax ; and ecx,0FFFh ; shl ecx,4
constexpr char kUpdateCameraSig[] =
    "A1 ?? ?? ?? ?? 81 EC 8C 00 00 00 53 55 33 DB 3B C3 56 57 0F 84 ?? ?? ?? ?? 8B C8 81 E1 FF 0F 00 00 C1 E1 04";
// Dans UpdateCamera : mov ecx,9 ; mov esi,<orientation finale> ; mov edi,<orientation précédente>
constexpr char kOrientCopySig[] = "B9 09 00 00 00 BE ?? ?? ?? ?? BF ?? ?? ?? ??";
// Dans UpdateCamera : mov ecx,[pos.y] ; mov eax,[pos.x] ; mov edx,[pos.z] (position finale)
constexpr char kPosReadSig[] = "8B 0D ?? ?? ?? ?? A1 ?? ?? ?? ?? 8B 15 ?? ?? ?? ??";
constexpr size_t kFunctionSpan = 0x520;
// Dans UpdateCamera : mov eax,[mode de caméra] ; lea ecx,[eax+eax*4] ; mov eax,[ecx*4+<table
// des modes>] ; cmp eax,ebx
constexpr char kCameraModeSig[] = "A1 ?? ?? ?? ?? 8D 0C 80 8B 04 8D ?? ?? ?? ?? 3B C3";
// Début du rendu d'une image : la caméra finale est recopiée dans la caméra de rendu.
// mov ecx,9 ; mov esi,<orientation finale> ; mov edi,<orientation de rendu> ; rep movsd ;
// call GetFOV ; push <orientation de rendu> ; fstp [champ de vue de rendu] ; call ...
constexpr char kRenderCameraSig[] =
    "B9 09 00 00 00 BE ?? ?? ?? ?? BF ?? ?? ?? ?? F3 A5 E8 ?? ?? ?? ?? 68 ?? ?? ?? ?? D9 1D ?? ?? ?? ?? E8";
// Fonction qui lève une pause : cmp [compteur de pauses],0 ; je ... ; push ... ; call [...] ;
// sub [compteur de pauses],1 ; cmp esi,-1. Le jeu est en pause tant que le compteur est > 0.
constexpr char kUnpauseSig[] =
    "83 3D ?? ?? ?? ?? 00 0F 84 ?? ?? ?? ?? 68 ?? ?? ?? ?? FF 15 ?? ?? ?? ?? 83 2D ?? ?? ?? ?? 01 83 FE FF";
// Fonction d'entrée commune des magasins et des interfaces de planque : or ecx,-1 ; add esp,4 ;
// cmp eax,esi ; mov byte [jeu libre],0 ; mov [ebp+...],ecx. La fonction de sortie y remet 1.
constexpr char kStoreEnterSig[] = "83 C9 FF 83 C4 04 3B C6 C6 05 ?? ?? ?? ?? 00 89 8D";
constexpr int kFreePlayOffset = 10;     // opérande du « mov byte »
// Lancement d'une cinématique : mov byte [...],0 ; fst [...] ; mov byte [cinématique],1 ;
// fst [...] ; mov byte [...],0 ; fstp [...]
constexpr char kCutsceneStartSig[] =
    "C6 05 ?? ?? ?? ?? 00 D9 15 ?? ?? ?? ?? C6 05 ?? ?? ?? ?? 01 D9 15 ?? ?? ?? ?? C6 05 ?? ?? ?? ?? 00 D9 1D";
constexpr int kCutsceneOffset = 15;     // opérande du deuxième « mov byte »
// Préparation d'une image par la simulation (un seul appelant, la boucle du jeu), sans
// argument : movzx eax,byte [...] ; sub esp,14h ; push ebx/ebp/esi/edi ; xor ebx,ebx ;
// push eax ; mov [...],bl ; call ...
constexpr char kRenderFrameSig[] = "0F B6 05 ?? ?? ?? ?? 83 EC 14 53 55 56 57 33 DB 50 88 1D ?? ?? ?? ?? E8";
// Dans l'horloge du jeu, appelée une fois par image de simulation : mov ebx,1 ;
// add [compteur d'images],ebx ; cmp byte [...],0 ; push esi ; push edi ; mov esi,eax
constexpr char kFrameCounterSig[] = "BB 01 00 00 00 01 1D ?? ?? ?? ?? 80 3D ?? ?? ?? ?? 00 56 57 8B F0";
constexpr int kFrameCounterOffset = 7;  // opérande du « add »
// Dans la préparation d'une image : mov eax,<caméra de rendu> ; call <recopie de la caméra en
// attente> ; mov eax,<caméra en attente> ; call <remise à zéro> ; cmp [...],bl
constexpr char kPendingCameraSig[] = "B8 ?? ?? ?? ?? E8 ?? ?? ?? ?? B8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 38 1D";
constexpr int kPendingCameraOffset = 11; // opérande du deuxième « mov eax »
constexpr size_t kPendingCameraSize = 0xAC; // dernier champ recopié : +0xA8
constexpr int kRenderOrientOffset = 11; // opérande du « mov edi »
constexpr int kGetFovCallOffset = 17;   // l'instruction call GetFOV
constexpr int kRenderFovOffset = 29;    // opérande du « fstp »

using UpdateCameraFn = void(__cdecl*)();
using GetFovFn = double(__cdecl*)(); // résultat dans st(0), comme l'original (float) et celui de Juiced
using RenderFrameFn = int(__cdecl*)();
UpdateCameraFn g_updateCamera = nullptr;
GetFovFn g_getFov = nullptr;
RenderFrameFn g_renderFrame = nullptr;
void* g_updateCameraTarget = nullptr;
void* g_getFovTarget = nullptr;
void* g_renderFrameTarget = nullptr;
void* g_renderReturn = nullptr; // adresse de retour de l'appel à GetFOV dans la copie de caméra

float* g_orient = nullptr;       // simulation : rvec, uvec, fvec (9 floats)
float* g_pos = nullptr;          // simulation : x, y, z
float* g_renderOrient = nullptr; // rendu : rvec, uvec, fvec
float* g_renderPos = nullptr;    // rendu : x, y, z (juste avant l'orientation)
const int* g_pauseCount = nullptr; // nombre de pauses en cours (0 : le jeu tourne)
const uint8_t* g_freePlay = nullptr; // 1 en jeu, 0 dans un magasin ou une interface de planque
int* g_frameCounter = nullptr;       // compteur d'images du jeu (un cran par image de simulation)
uint8_t* g_pendingCamera = nullptr;  // caméra en attente : demandes de la simulation au rendu
const int* g_cameraMode = nullptr;   // mode de caméra du jeu (0 : caméra normale à la 3e personne)
bool g_playerReady = false;          // engine::player installé
bool g_hudReady = false;             // engine::hud installé

bool Paused()
{
    return g_pauseCount && *g_pauseCount > 0;
}

const uint8_t* g_cutscene = nullptr; // 1 pendant une cinématique

// Garde-robe, garage, boutique, personnalisation : le jeu tourne, mais c'est un menu
bool InInterface()
{
    return g_freePlay && *g_freePlay == 0;
}

bool InCutscene()
{
    return g_cutscene && *g_cutscene != 0;
}

// --- Simulation -----------------------------------------------------------------------
float g_written[9]{}; // ce que nous avons écrit à la place de l'orientation du jeu
bool g_applied = false;
// Première personne : position du jeu mise de côté, position écrite à sa place
float g_gamePos[3]{};
float g_writtenPos[3]{};
bool g_posApplied = false;
float g_headOffset[3]{};  // écart racine -> tête, filtré
bool g_headOffsetValid = false;
LARGE_INTEGER g_headTime{};
float g_headHeight = 0.0f; // relevé : hauteur de la tête au-dessus de la racine
float g_eyeFromRoot[3]{};  // des pieds du personnage à la caméra à la première personne
bool g_handsReady = false; // engine::hands installé
bool g_meleeReady = false; // engine::melee installé
// Orientation calculée par le jeu à la dernière mise à jour, lue aussi par le thread du rendu
std::mutex g_gameMutex;
float g_gameOrient[9]{};
bool g_gameOrientValid = false;

// --- Rendu : vues des images préparées, en attente de leur Present ------------------------
struct PendingView {
    bool valid = false;
    float quat[4]{0.0f, 0.0f, 0.0f, 1.0f};
    int eye = 0;
    unsigned pair = 0; // vrai relief : numéro de la paire, 0 sinon
    float fov = 0.0f;
    ULONGLONG tick = 0;
    render::SightOverlay sight; // repère de visée projeté avec la caméra de cette image
};

// Vrai relief : paire en cours de préparation (thread de simulation uniquement). Les deux
// yeux d'une paire partagent la pose de tête et l'écart des yeux relevés à son début.
struct Pair {
    bool active = false;
    bool viewed = false; // la préparation en cours a bien recopié une caméra de jeu
    unsigned id = 0;
    int eye = 0;
    float quat[4]{0.0f, 0.0f, 0.0f, 1.0f};
    float ipd = 0.063f;
};
Pair g_pair;
unsigned g_pairCounter = 0;
std::atomic<unsigned> g_pairsRendered{0};
constexpr ULONGLONG kViewMaxAgeMs = 250;
// La simulation n'a jamais plus d'une image d'avance sur le rendu : au-delà de deux vues en
// attente, la plus ancienne appartient à une image qui n'a pas été présentée
constexpr int kMaxPending = 2;
std::mutex g_viewMutex;
PendingView g_pending[kMaxPending];
int g_pendingCount = 0;
unsigned g_eyeCounter = 0;
std::atomic<unsigned> g_renderCopies{0};
// Relevés : vues en attente au moment du Present (0, 1, 2), images préparées sans pose de
// tête, vues perdues (image jamais présentée)
std::atomic<unsigned> g_pendingAtPresent[kMaxPending + 1]{};
std::atomic<unsigned> g_copiesWithoutPose{0};
std::atomic<unsigned> g_viewsDropped{0};

void PushView(const PendingView& view)
{
    std::lock_guard lock(g_viewMutex);
    if (g_pendingCount == kMaxPending) {
        g_pending[0] = g_pending[1];
        --g_pendingCount;
        ++g_viewsDropped;
    }
    g_pending[g_pendingCount++] = view;
}

// --- Réglages de sr2_vr.ini, relus une fois par seconde -------------------------------
struct Settings {
    bool headTracking = true;
    bool mirror[3]{};          // miroir de l'axe x, y, z du repère du casque
    float hfov = 0.0f;         // champ de vue horizontal imposé au rendu (0 = celui du jeu)
    bool levelHorizon = false; // caméra du jeu remise à plat
    bool stereo = false;       // relief : un œil par image
    bool pairs = false;        // vrai relief : deux images (gauche, droite) par image de simulation
    bool pairCounter = true;   // vrai relief : le compteur d'images avance entre les deux yeux
    float worldScale = 1.0f;   // unités du jeu par mètre
    float ipdScale = 1.0f;     // correction de l'écart entre les yeux
    bool swapEyes = false;
    bool interfaceScreen = true; // magasins et interfaces de planque sur l'écran virtuel
    bool cutsceneScreen = true;  // cinématiques sur l'écran virtuel
    bool firstPerson = false;    // caméra aux yeux du personnage
    float fpForward = 0.12f;     // décalage des yeux vers l'avant du regard, en mètres
    float fpUp = 0.06f;          // décalage vers le haut
    float fpSmooth = 0.12f;      // constante de temps du filtre sur la tête, en secondes (0 : aucun)
    int fpVehicleView = 0;       // en véhicule : 0 yeux du conducteur, 1 devant le pare-brise, 2 caméra du jeu
    float fpVehicleForward = 0.9f; // vue « devant le pare-brise » : avance le long du véhicule, en mètres
    float fpVehicleUp = 0.15f;     // et hauteur au-dessus de la tête
    float hudScale = 1.0f;       // échelle de l'interface en partie (1 = celle du jeu)
    float hudOffsetY = 0.0f;     // décalage vertical de l'interface, en unités de 720 (positif = vers le bas)
    float hudDistance = 0.0f;    // distance à laquelle l'interface flotte en relief, en mètres (0 = infini)
    bool immersive = false;      // [Display] mode >= 1
    bool handAim = true;         // le tir suit le rayon de la manette (sinon celui du regard)
    // repère de visée : 0 aucun (réticule du jeu), 1 trait et point, 2 point seul (dessinés par
    // le casque), 3 trait et point dessinés dans l'image du jeu (ancien mode)
    int laser = 1;
    bool handArms = false;       // les bras du personnage sont amenés aux manettes
    float torsoMax = 90.0f;      // le buste se tourne vers le regard jusqu'à cet angle, en degrés (0 = non)
    bool melee = true;           // coups portés au geste (poings, armes blanches)
    float meleeSpeed = 1.8f;     // vitesse de la main qui fait d'un geste un coup, en m/s
    float meleeTilt = 0.0f;      // arme blanche : inclinaison de la main vers l'avant, en degrés
};
std::mutex g_settingsMutex;
Settings g_settings;
ULONGLONG g_lastConfig = 0;

Settings CurrentSettings()
{
    std::lock_guard lock(g_settingsMutex);
    return g_settings;
}

void ReadConfig()
{
    Settings s;
    const int mode = config::Int("Display", "mode", 0);
    s.headTracking = config::Int("Camera", "head_tracking", 1) != 0;
    s.mirror[0] = config::Int("Camera", "mirror_x", 0) != 0;
    s.mirror[1] = config::Int("Camera", "mirror_y", 0) != 0;
    s.mirror[2] = config::Int("Camera", "mirror_z", 0) != 0;
    s.hfov = config::Float("Camera", "hfov", 0.0f);
    if (!(s.hfov >= 30.0f && s.hfov <= 150.0f))
        s.hfov = 0.0f;
    // Par défaut : horizon à plat dès que l'image entoure la tête (c'est la tête qui lève ou
    // baisse le regard ; la caméra 3e personne du jeu, elle, plonge de 10° environ), caméra du
    // jeu intacte sur l'écran virtuel
    s.levelHorizon = config::Int("Camera", "level_horizon", mode >= 1 ? 1 : 0) != 0;
    s.immersive = mode >= 1;
    s.stereo = mode >= 2;
    s.pairs = mode == 3;
    s.pairCounter = config::Int("Stereo", "pair_counter", 1) != 0;
    s.worldScale = config::Float("Stereo", "world_scale", 1.0f);
    s.ipdScale = config::Float("Stereo", "ipd_scale", 1.0f);
    if (!(s.worldScale > 0.01f && s.worldScale < 100.0f))
        s.worldScale = 1.0f;
    if (!(s.ipdScale >= 0.0f && s.ipdScale < 10.0f))
        s.ipdScale = 1.0f;
    s.swapEyes = config::Int("Stereo", "swap_eyes", 0) != 0;
    s.interfaceScreen = config::Int("Display", "interface_screen", 1) != 0;
    s.cutsceneScreen = config::Int("Display", "cutscene_screen", 1) != 0;
    s.handAim = config::Int("Hands", "aim", 1) != 0;
    s.laser = config::Int("Hands", "laser", 2);
    if (s.laser < 0 || s.laser > 3)
        s.laser = 2;
    s.handArms = config::Int("Hands", "arms", 0) != 0;
    s.torsoMax = config::Float("Hands", "torso", 90.0f);
    if (!(s.torsoMax >= 0.0f && s.torsoMax <= 180.0f))
        s.torsoMax = 90.0f;
    s.melee = config::Int("Melee", "enabled", 1) != 0;
    s.meleeSpeed = config::Float("Melee", "speed", 1.8f);
    if (!(s.meleeSpeed >= 0.5f && s.meleeSpeed <= 10.0f))
        s.meleeSpeed = 1.8f;
    s.meleeTilt = config::Float("Melee", "weapon_tilt", 0.0f);
    if (!(s.meleeTilt >= -90.0f && s.meleeTilt <= 90.0f))
        s.meleeTilt = 0.0f;
    s.firstPerson = config::Int("FirstPerson", "enabled", 0) != 0;
    s.fpForward = config::Float("FirstPerson", "forward", 0.12f);
    s.fpUp = config::Float("FirstPerson", "up", 0.06f);
    s.fpSmooth = config::Float("FirstPerson", "smooth", 0.12f);
    if (!(s.fpForward > -2.0f && s.fpForward < 2.0f))
        s.fpForward = 0.12f;
    if (!(s.fpUp > -2.0f && s.fpUp < 2.0f))
        s.fpUp = 0.06f;
    if (!(s.fpSmooth >= 0.0f && s.fpSmooth < 5.0f))
        s.fpSmooth = 0.12f;
    s.fpVehicleView = config::Int("FirstPerson", "vehicle_view", 0);
    if (s.fpVehicleView < 0 || s.fpVehicleView > 2)
        s.fpVehicleView = 0;
    s.fpVehicleForward = config::Float("FirstPerson", "vehicle_forward", 0.9f);
    s.fpVehicleUp = config::Float("FirstPerson", "vehicle_up", 0.15f);
    if (!(s.fpVehicleForward > -5.0f && s.fpVehicleForward < 5.0f))
        s.fpVehicleForward = 0.9f;
    if (!(s.fpVehicleUp > -3.0f && s.fpVehicleUp < 3.0f))
        s.fpVehicleUp = 0.15f;
    s.hudScale = config::Float("Hud", "scale", 0.38f);
    if (!(s.hudScale >= 0.1f && s.hudScale <= 1.0f))
        s.hudScale = 0.38f;
    s.hudOffsetY = config::Float("Hud", "offset_y", 0.0f);
    if (!(s.hudOffsetY > -720.0f && s.hudOffsetY < 720.0f))
        s.hudOffsetY = 0.0f;
    s.hudDistance = config::Float("Hud", "distance", 3.0f);
    if (!(s.hudDistance > -100.0f && s.hudDistance < 100.0f))
        s.hudDistance = 3.0f;
    std::lock_guard lock(g_settingsMutex);
    g_settings = s;
}

// --- Géométrie ------------------------------------------------------------------------

// Quaternion OpenXR -> matrice de rotation rangée par lignes, colonnes = axes du casque
void QuatToMatrix(const float q[4], float out[9])
{
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    out[0] = 1.0f - 2.0f * (y * y + z * z);
    out[1] = 2.0f * (x * y - z * w);
    out[2] = 2.0f * (x * z + y * w);
    out[3] = 2.0f * (x * y + z * w);
    out[4] = 1.0f - 2.0f * (x * x + z * z);
    out[5] = 2.0f * (y * z - x * w);
    out[6] = 2.0f * (x * z - y * w);
    out[7] = 2.0f * (y * z + x * w);
    out[8] = 1.0f - 2.0f * (x * x + y * y);
}

// Garde la direction horizontale de la caméra du jeu, sans tangage ni roulis : avant à plat,
// haut = Y du monde, droite = haut x avant (repère main gauche).
void LevelOrientation(const float in[9], float out[9])
{
    float fx = in[6], fz = in[8];
    float length = std::sqrt(fx * fx + fz * fz);
    if (length < 0.05f) {
        // Caméra à la verticale : son « haut » donne le cap (vers l'avant si elle plonge)
        const float sign = in[7] < 0.0f ? 1.0f : -1.0f;
        fx = sign * in[3];
        fz = sign * in[5];
        length = std::sqrt(fx * fx + fz * fz);
        if (length < 0.05f) {
            memcpy(out, in, 9 * sizeof(float));
            return;
        }
    }
    fx /= length;
    fz /= length;
    const float level[9] = {fz, 0.0f, -fx, 0.0f, 1.0f, 0.0f, fx, 0.0f, fz};
    memcpy(out, level, sizeof(level));
}

// head : rotation du casque, axes du casque en colonnes dans le repère OpenXR (x droite,
// y haut, z arrière). La caméra du jeu a pour axes (droite, haut, avant) : avant = -z.
void ComposeWithHead(const float game[9], const float head[9], const bool mirror[3], float out[9])
{
    float h[9];
    memcpy(h, head, sizeof(h));
    // Miroir d'un axe : S * R * S, avec S diagonale (-1 sur l'axe)
    for (int axis = 0; axis < 3; ++axis) {
        if (!mirror[axis])
            continue;
        for (int k = 0; k < 3; ++k) {
            h[axis * 3 + k] = -h[axis * 3 + k];
            h[k * 3 + axis] = -h[k * 3 + axis];
        }
    }
    const float* r = game;     // droite
    const float* u = game + 3; // haut
    const float* f = game + 6; // avant
    // Axes du casque exprimés dans (droite, haut, avant) : colonne i de h, z changé de signe
    const float right[3] = {h[0], h[3], -h[6]};
    const float up[3] = {h[1], h[4], -h[7]};
    const float forward[3] = {-h[2], -h[5], h[8]};
    for (int k = 0; k < 3; ++k) {
        out[k] = right[0] * r[k] + right[1] * u[k] + right[2] * f[k];
        out[3 + k] = up[0] * r[k] + up[1] * u[k] + up[2] * f[k];
        out[6 + k] = forward[0] * r[k] + forward[1] * u[k] + forward[2] * f[k];
    }
}

// Repère du monde dans lequel vit l'espace du casque : la caméra du jeu, remise à plat si
// demandé (droite, haut, avant)
void HeadBase(const float game[9], const Settings& s, float base[9])
{
    if (s.levelHorizon)
        LevelOrientation(game, base);
    else
        memcpy(base, game, 9 * sizeof(float));
}

// Orientation de la caméra avec la tête : celle du jeu (remise à plat si demandé) composée
// avec le casque
void HeadedOrientation(const float game[9], const float quat[4], const Settings& s, float out[9])
{
    float head[9], base[9];
    QuatToMatrix(quat, head);
    HeadBase(game, s, base);
    ComposeWithHead(base, head, s.mirror, out);
}

// --- Simulation : mise à jour de la caméra -----------------------------------------------

// État du jeu : 0 en partie, 1 en pause, 2 dans une interface qui laisse le jeu tourner,
// 3 dans une cinématique
enum : int { kPlaying = 0, kPaused = 1, kInterface = 2, kCutscene = 3 };
int GameState(const Settings& s)
{
    if (Paused())
        return kPaused;
    if (s.interfaceScreen && InInterface())
        return kInterface;
    return s.cutsceneScreen && InCutscene() ? kCutscene : kPlaying;
}

// Modes de caméra où la vue reste celle du jeu : 2 caméra libre, 5 cinématique, 10 magasin,
// 11 personnalisation de planque ou de gang
bool FirstPersonAllowed()
{
    if (!g_playerReady || !g_pos)
        return false;
    const int mode = g_cameraMode ? *g_cameraMode : 0;
    return mode != 2 && mode != 5 && mode != 10 && mode != 11;
}

// Vue choisie, changée par le clic du stick gauche (ou par sr2_vr.ini quand sa valeur change)
enum : int { kFootEyes = 0, kFootGame = 1 };
enum : int { kVehicleEyes = 0, kVehicleFront = 1, kVehicleGame = 2 };
int g_footView = kFootEyes;
int g_vehicleView = kVehicleEyes;
int g_iniFoot = -1;    // dernières valeurs lues dans sr2_vr.ini
int g_iniVehicle = -1;
bool g_inVehicle = false;

const char* ViewName(bool vehicle, int view)
{
    if (!vehicle)
        return view == kFootEyes ? "yeux du personnage" : "camera du jeu (3e personne)";
    return view == kVehicleEyes    ? "yeux du conducteur"
           : view == kVehicleFront ? "devant le pare-brise"
                                   : "camera du jeu (3e personne)";
}

// Réglages et clics : met à jour la vue à pied et la vue en véhicule
void UpdateViewChoice(const Settings& s, bool vehicle)
{
    const int iniFoot = s.firstPerson ? kFootEyes : kFootGame;
    if (iniFoot != g_iniFoot) {
        g_iniFoot = g_footView = iniFoot;
    }
    // [FirstPerson] enabled=0 : caméra du jeu partout
    const int iniVehicle = s.firstPerson ? s.fpVehicleView : static_cast<int>(kVehicleGame);
    if (iniVehicle != g_iniVehicle) {
        g_iniVehicle = g_vehicleView = iniVehicle;
    }
    for (unsigned clicks = xr::TakeViewCycleRequests(); clicks; --clicks) {
        if (vehicle)
            g_vehicleView = (g_vehicleView + 1) % 3;
        else
            g_footView = g_footView == kFootEyes ? kFootGame : kFootEyes;
        LOG_ENGINE("vue demandee aux manettes, %s : %s", vehicle ? "en vehicule" : "a pied",
                   ViewName(vehicle, vehicle ? g_vehicleView : g_footView));
    }
}

// Position de la caméra à la première personne. game : orientation de la caméra du jeu (son
// cap donne l'avant à pied). false : vue du jeu demandée, pas de joueur ou pas de squelette ;
// la caméra du jeu reste en place.
bool FirstPersonEye(const float game[9], const float* headRight, const Settings& s, float eye[3])
{
    float head[3], root[3], body[9];
    if (!player::HeadPosition(head, root, body)) {
        g_headOffsetValid = false;
        return false;
    }
    g_inVehicle = player::InVehicle();
    UpdateViewChoice(s, g_inVehicle);
    const int view = g_inVehicle ? g_vehicleView : g_footView;
    const int gameView = g_inVehicle ? static_cast<int>(kVehicleGame) : static_cast<int>(kFootGame);
    if (view == gameView) {
        g_headOffsetValid = false;
        return false;
    }

    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    const float dt = static_cast<float>(now.QuadPart - g_headTime.QuadPart) / static_cast<float>(frequency.QuadPart);
    g_headTime = now;

    const float offset[3] = {head[0] - root[0], head[1] - root[1], head[2] - root[2]};
    float jump = 0.0f;
    for (int k = 0; k < 3; ++k)
        jump += (offset[k] - g_headOffset[k]) * (offset[k] - g_headOffset[k]);
    // Filtre passe-bas sur l'écart racine -> tête : la racine avance sans à-coups, c'est
    // l'animation qui secoue la tête. Recalé d'un coup au départ, après une interruption ou
    // un saut de plus de 1 m (entrée en véhicule, chute).
    if (!g_headOffsetValid || !(dt > 0.0f && dt < 0.25f) || jump > 1.0f || s.fpSmooth <= 0.0f) {
        memcpy(g_headOffset, offset, sizeof(offset));
        g_headOffsetValid = true;
    } else {
        const float alpha = 1.0f - std::exp(-dt / s.fpSmooth);
        for (int k = 0; k < 3; ++k)
            g_headOffset[k] += (offset[k] - g_headOffset[k]) * alpha;
    }
    g_headHeight = g_headOffset[1];

    if (g_inVehicle && view == kVehicleFront) {
        // Devant le pare-brise : le long de l'avant du siège (donc du véhicule), pas du regard
        for (int k = 0; k < 3; ++k)
            eye[k] = root[k] + g_headOffset[k] + s.fpVehicleForward * body[6 + k] + s.fpVehicleUp * body[3 + k];
    } else {
        float level[9];
        LevelOrientation(game, level);
        // Les yeux sont devant le centre de la tête, du côté où elle regarde : l'avant à plat de
        // la tête (à 90 degrés de sa droite), à défaut celui de la caméra du jeu
        float forward[3] = {level[6], level[7], level[8]};
        if (headRight) {
            const float flat[3] = {-headRight[2], 0.0f, headRight[0]};
            const float length = std::sqrt(flat[0] * flat[0] + flat[2] * flat[2]);
            if (length > 0.3f)
                for (int k = 0; k < 3; ++k)
                    forward[k] = flat[k] / length;
        }
        for (int k = 0; k < 3; ++k)
            eye[k] = root[k] + g_headOffset[k] + s.fpForward * forward[k] + s.fpUp * level[3 + k];
    }
    for (int k = 0; k < 3; ++k)
        g_eyeFromRoot[k] = eye[k] - root[k];
    return true;
}

// --- Interface en partie ------------------------------------------------------------------

float g_hudApplied[3]{1.0f, 0.0f, 0.0f}; // échelle, x, y posés en dernier
int g_reticleShown = -1;                 // réticule du jeu : 1 montré, 0 caché, -1 pas encore décidé
ULONGLONG g_reticleTick = 0;
bool g_sightDrawn = false;               // le mod dessine son repère de visée en ce moment
float g_lastFov = 64.0f;                  // dernier champ de vue horizontal du rendu, en degrés
ULONGLONG g_hudAppliedTick = 0;

// Place l'interface pour l'image qui va être préparée. eye : -1 hors relief (pas de décalage),
// sinon 0 ou 1. hfov : champ de vue horizontal du rendu, en degrés. Thread de simulation.
void PlaceHud(const Settings& s, bool immersed, int eye, float ipd, float hfov)
{
    if (!g_hudReady)
        return;
    // Réticule du jeu : caché tant que le mod dessine son propre repère (reposé une fois par
    // seconde, le document étant recréé à chaque chargement)
    const int reticle = immersed && s.laser != 0 && g_sightDrawn ? 0 : 1;
    const ULONGLONG tick = GetTickCount64();
    if (reticle != g_reticleShown || tick - g_reticleTick >= 1000) {
        // Rien à remontrer si on ne l'a jamais caché
        if (reticle == 0 || g_reticleShown == 0)
            hud::ShowReticle(reticle != 0);
        g_reticleShown = reticle;
        g_reticleTick = tick;
    }
    // Seulement quand l'image entoure la tête : sur l'écran virtuel (menus, casque posé),
    // l'interface garde sa taille
    const bool shrink = s.immersive && immersed && s.hudScale < 0.999f;
    const float scale = shrink ? s.hudScale : 1.0f;
    float x = 0.0f, y = 0.0f;
    if (shrink) {
        x = 640.0f * (1.0f - scale);
        y = 360.0f * (1.0f - scale) + s.hudOffsetY;
        if (eye >= 0 && s.hudDistance != 0.0f && hfov > 1.0f && hfov < 179.0f) {
            // Un point à la distance d, droit devant, se voit décalé de (écart / 2) / d dans
            // chaque œil ; la demi-largeur de l'image (640 unités) vaut tan(hfov / 2).
            const float shift = 0.5f * ipd * s.ipdScale / s.hudDistance / std::tan(hfov * 0.5f * 0.01745329f) * 640.0f;
            const bool left = (eye == 0) == !s.swapEyes;
            x += left ? shift : -shift;
        }
    }
    // Reposé quand il change, et une fois par seconde : le document est recréé à chaque
    // chargement de partie
    const ULONGLONG now = GetTickCount64();
    if (scale == g_hudApplied[0] && x == g_hudApplied[1] && y == g_hudApplied[2] && now - g_hudAppliedTick < 1000)
        return;
    hud::Place(scale, x, y);
    g_hudApplied[0] = scale;
    g_hudApplied[1] = x;
    g_hudApplied[2] = y;
    g_hudAppliedTick = now;
}

ULONGLONG g_lastStats = 0;
int g_lastState = -1;
int g_lastCameraMode = -1;
int g_lastFirstPerson = -1;
int g_lastAimSource = -1;
bool g_aimReady = false;   // engine::aim installé
bool g_aimAllowed = false; // en partie, tête suivie : le jeu vise avec nos rayons
bool g_laserShown = false; // le casque dessine le repère d'au moins une main
bool g_handsAllowed = false; // première personne à pied, tête suivie : les bras suivent les manettes

// Rayon de visée demandé par engine/aim au moment où le jeu vise : celui d'une manette (hand 0
// ou 1 : pose « aim » la plus récente, départ = caméra, donc les yeux du personnage à la
// première personne, plus l'écart manette - tête) ou celui du regard (hand -1). Thread de
// simulation.
bool AimRay(int hand, float origin[3], float orient[9])
{
    if (!g_aimAllowed || !g_pos)
        return false;
    const Settings s = CurrentSettings();
    float game[9];
    {
        std::lock_guard lock(g_gameMutex);
        if (!g_gameOrientValid)
            return false;
        memcpy(game, g_gameOrient, sizeof(game));
    }
    memcpy(origin, g_pos, 3 * sizeof(float));
    float quat[4];
    if (hand < 0) {
        if (!xr::GetHeadPose(quat, nullptr))
            return false;
        HeadedOrientation(game, quat, s, orient);
        return true;
    }
    float offset[3];
    if (!s.handAim || !xr::GetAimRay(hand, quat, offset))
        return false;
    float base[9];
    HeadBase(game, s, base);
    HeadedOrientation(game, quat, s, orient);
    for (int k = 0; k < 3; ++k)
        origin[k] += s.worldScale * (offset[0] * base[k] + offset[1] * base[3 + k] - offset[2] * base[6 + k]);
    return true;
}

// Pose d'une manette demandée par engine/hands au moment de poser les bras : creux de la main et
// départ du rayon de visée, en vecteurs du monde comptés depuis les yeux, et orientation du
// rayon. Thread de simulation.
bool HandFrame(int hand, float grip[3], float origin[3], float orient[9])
{
    if (!g_handsAllowed)
        return false;
    const Settings s = CurrentSettings();
    float game[9];
    {
        std::lock_guard lock(g_gameMutex);
        if (!g_gameOrientValid)
            return false;
        memcpy(game, g_gameOrient, sizeof(game));
    }
    float quat[4], aimOffset[3], gripOffset[3];
    if (!xr::GetAimRay(hand, quat, aimOffset) || !xr::GetHandOffset(hand, gripOffset))
        return false;
    float base[9];
    HeadBase(game, s, base);
    HeadedOrientation(game, quat, s, orient);
    for (int k = 0; k < 3; ++k) {
        grip[k] = s.worldScale * (gripOffset[0] * base[k] + gripOffset[1] * base[3 + k] - gripOffset[2] * base[6 + k]);
        origin[k] = s.worldScale * (aimOffset[0] * base[k] + aimOffset[1] * base[3 + k] - aimOffset[2] * base[6 + k]);
    }
    return true;
}
unsigned long long g_calls = 0;
unsigned long long g_callsAtLastStats = 0;
unsigned g_copiesAtLastStats = 0;

void __cdecl HkUpdateCamera()
{
    // Le jeu retrouve sa propre caméra, sauf s'il l'a déjà réécrite lui-même
    if (g_applied && memcmp(g_orient, g_written, sizeof(g_written)) == 0) {
        std::lock_guard lock(g_gameMutex);
        memcpy(g_orient, g_gameOrient, sizeof(g_gameOrient));
    }
    g_applied = false;
    if (g_posApplied && memcmp(g_pos, g_writtenPos, sizeof(g_writtenPos)) == 0)
        memcpy(g_pos, g_gamePos, sizeof(g_gamePos));
    g_posApplied = false;

    g_updateCamera();

    float game[9];
    memcpy(game, g_orient, sizeof(game));
    {
        std::lock_guard lock(g_gameMutex);
        memcpy(g_gameOrient, game, sizeof(game));
        g_gameOrientValid = true;
    }
    const ULONGLONG now = GetTickCount64();
    if (now - g_lastConfig >= 1000) {
        g_lastConfig = now;
        ReadConfig();
    }
    const Settings s = CurrentSettings();

    // Côté simulation aussi, pour que le culling, le son et la visée suivent le regard
    float quat[4];
    const bool tracked = xr::GetHeadPose(quat, nullptr);
    const int state = GameState(s);
    if (state != g_lastState) {
        static const char* const kNames[] = {"en partie", "en pause : ecran virtuel",
                                             "interface ouverte (garde-robe, garage, boutique...) : ecran virtuel",
                                             "cinematique : ecran virtuel"};
        LOG_ENGINE("jeu : %s", kNames[state]);
        g_lastState = state;
    }
    // Lacet de la caméra du jeu, pour la rotation par crans (mesure de ce que le jeu a tourné)
    if (state == kPlaying)
        input::actions::SetGameYaw(std::atan2(game[6], game[8]) * 57.29578f, !player::InVehicle());
    // La caméra de la simulation suit le regard : culling, son et sens de la marche
    const bool headed = tracked && s.headTracking && state == kPlaying;
    if (headed) {
        HeadedOrientation(game, quat, s, g_written);
        memcpy(g_orient, g_written, sizeof(g_written));
        g_applied = true;
    }

    // Première personne : la caméra finale passe aux yeux du personnage
    if (g_cameraMode && *g_cameraMode != g_lastCameraMode) {
        LOG_ENGINE("mode de camera du jeu : %d", *g_cameraMode);
        g_lastCameraMode = *g_cameraMode;
    }
    bool firstPerson = false;
    if (state == kPlaying && FirstPersonAllowed()) {
        float eye[3];
        if (FirstPersonEye(game, headed ? g_written : nullptr, s, eye)) {
            memcpy(g_gamePos, g_pos, sizeof(g_gamePos));
            memcpy(g_writtenPos, eye, sizeof(eye));
            memcpy(g_pos, eye, sizeof(eye));
            g_posApplied = true;
            firstPerson = true;
        }
    } else {
        g_headOffsetValid = false;
    }
    // Visée : le jeu vise avec nos rayons (engine/aim les demande à AimRay au moment où il en
    // a besoin). Pour chaque main dont le rayon a servi, le casque dessine le repère.
    int aimSource = 0; // 0 le jeu vise seul, 1 regard, 2 manette gauche, 3 manette droite
    if (g_aimReady) {
        g_aimAllowed = headed && g_pos;
        aim::SetActive(g_aimAllowed);
        bool shown = false;
        if (g_aimAllowed) {
            const int hand = controls::AimHand();
            float aimQuat[4], aimOffset[3];
            aimSource = s.handAim && xr::GetAimRay(hand, aimQuat, aimOffset) ? 2 + hand : 1;
        }
        // (une arme blanche ne vise pas : pas de repère)
        const bool armed = g_aimAllowed && s.handAim && (s.laser == 1 || s.laser == 2) && player::HeldMeleeAttack() == 0xFF;
        for (int hand = 0; hand < 2; ++hand) {
            aim::Sight sight;
            const bool visible = armed && aim::GetSight(hand, &sight);
            xr::SetSight(hand, visible, visible ? sight.distance / s.worldScale : 0.0f, visible && sight.hit);
            shown |= visible;
        }
        g_laserShown = shown;
    }
    // (la main qui vise change à chaque gâchette : le log ne retient que la nature du rayon)
    const int aimKind = aimSource >= 2 ? 2 : aimSource;
    if (aimKind != g_lastAimSource && state == kPlaying) {
        static const char* const kSources[] = {"camera du jeu", "regard (manette qui vise non suivie)", "manettes"};
        LOG_ENGINE("visee : %s", kSources[aimKind]);
        g_lastAimSource = aimKind;
    }
    // Bras amenés aux manettes : à la première personne et à pied seulement (en véhicule, les
    // mains restent sur le volant)
    if (g_handsReady) {
        g_handsAllowed = s.handArms && firstPerson && !g_inVehicle && tracked;
        hands::SetFrame(g_handsAllowed, g_eyeFromRoot, headed ? g_written : game, s.torsoMax);
        hands::SetMeleeTilt(s.meleeTilt);
    }
    // Coups au geste : seulement quand les bras suivent les manettes
    if (g_meleeReady)
        melee::SetFrame(g_handsReady && g_handsAllowed && s.melee, s.meleeSpeed);
    // Vue réellement appliquée : 0 caméra du jeu, sinon 1 + vue (à pied) ou 10 + vue (en véhicule)
    const int shownView = !firstPerson ? 0 : g_inVehicle ? 10 + g_vehicleView : 1 + g_footView;
    if (shownView != g_lastFirstPerson && state == kPlaying) {
        LOG_ENGINE("vue : %s%s", firstPerson ? ViewName(g_inVehicle, g_inVehicle ? g_vehicleView : g_footView)
                                             : "camera du jeu (3e personne)",
                   firstPerson && g_inVehicle ? " (en vehicule)" : "");
        g_lastFirstPerson = shownView;
    }

    // Relevé : première image, puis toutes les 10 s
    if (++g_calls == 1 || now - g_lastStats >= 10000) {
        const unsigned copies = g_renderCopies;
        if (g_calls > 1) {
            const double seconds = static_cast<double>(now - g_lastStats) / 1000.0;
            LOG_ENGINE("camera : %.1f mises a jour par seconde (thread %lu), %.1f cameras de rendu par seconde, "
                       "%.1f paires par seconde, horizon %s, %s%s",
                       static_cast<double>(g_calls - g_callsAtLastStats) / seconds, GetCurrentThreadId(),
                       static_cast<double>(copies - g_copiesAtLastStats) / seconds,
                       static_cast<double>(g_pairsRendered.exchange(0)) / seconds, s.levelHorizon ? "a plat" : "du jeu",
                       !s.stereo  ? "sans relief"
                       : !s.pairs ? "relief (un oeil par image)"
                       : g_pendingCamera ? "vrai relief (deux images par image de simulation)"
                                         : "relief (un oeil par image : vrai relief indisponible)",
                       state == kPaused      ? ", JEU EN PAUSE"
                       : state == kInterface ? ", INTERFACE DU JEU OUVERTE"
                       : state == kCutscene  ? ", CINEMATIQUE"
                                             : "");
            LOG_ENGINE("  vues en attente au Present : aucune %u, une %u, deux %u ; images sans pose de tete %u, "
                       "vues perdues %u",
                       g_pendingAtPresent[0].exchange(0), g_pendingAtPresent[1].exchange(0),
                       g_pendingAtPresent[2].exchange(0), g_copiesWithoutPose.exchange(0), g_viewsDropped.exchange(0));
        }
        g_callsAtLastStats = g_calls;
        g_copiesAtLastStats = copies;
        g_lastStats = now;
        LOG_ENGINE("  position (%.1f, %.1f, %.1f), avant du jeu (%.2f, %.2f, %.2f), haut (%.2f, %.2f, %.2f)",
                   g_pos ? g_pos[0] : 0.0f, g_pos ? g_pos[1] : 0.0f, g_pos ? g_pos[2] : 0.0f, game[6], game[7], game[8],
                   game[3], game[4], game[5]);
        if (g_aimReady) {
            aim::Stats stats;
            aim::TakeStats(&stats);
            LOG_ENGINE("  visee : %u points vises avec nos rayons%s", stats.points,
                       player::DualWielding() ? " ; une arme dans chaque main" : "");
            if (stats.calls[0] + stats.calls[1])
                LOG_ENGINE("    tir : main gauche %u images de tir, %u avec son rayon, %u balles ; main droite %u, %u, %u ; "
                           "%u tir(s) differe(s)",
                           stats.calls[0], stats.scoped[0], stats.bullets[0], stats.calls[1], stats.scoped[1],
                           stats.bullets[1], stats.deferred);
            static const char* const kRays[] = {"regard", "main gauche", "main droite"};
            for (int hand = -1; hand < 2; ++hand) {
                aim::Sight sight;
                if (aim::GetSight(hand, &sight))
                    LOG_ENGINE("    %s : point vise (%.1f, %.1f, %.1f) a %.1f m%s, depart (%.1f, %.1f, %.1f), direction "
                               "(%.2f, %.2f, %.2f)",
                               kRays[hand + 1], sight.point[0], sight.point[1], sight.point[2], sight.distance,
                               sight.hit ? "" : " (bout de la portee)", sight.origin[0], sight.origin[1],
                               sight.origin[2], sight.forward[0], sight.forward[1], sight.forward[2]);
            }
        }
        if (g_handsReady) {
            unsigned posed[2], torso = 0;
            float worst = 0.0f, torsoWorst = 0.0f;
            hands::TakeStats(posed, &worst, &torso, &torsoWorst);
            if (posed[0] + posed[1] + torso)
                LOG_ENGINE("  mains : bras gauche pose sur %u images, bras droit sur %u, plus grand ecart poignet - cible "
                           "%.3f m ; buste tourne sur %u images, jusqu'a %.0f degres",
                           posed[0], posed[1], worst, torso, torsoWorst);
        }
        if (g_meleeReady) {
            melee::Stats m;
            melee::TakeStats(&m);
            if (m.swings[0] + m.swings[1])
                LOG_ENGINE("  melee : %u geste(s) de la main gauche (%u touche(s), %u dans le decor), %u de la droite "
                           "(%u, %u) ; main la plus rapide %.1f m/s, seuil %.1f",
                           m.swings[0], m.hits[0], m.world[0], m.swings[1], m.hits[1], m.world[1], m.fastest,
                           s.meleeSpeed);
        }
        if (firstPerson)
            LOG_ENGINE("  premiere personne : tete a %.2f m au-dessus de la racine, camera du jeu a %.2f m des yeux",
                       g_headHeight,
                       std::sqrt((g_gamePos[0] - g_pos[0]) * (g_gamePos[0] - g_pos[0]) +
                                 (g_gamePos[1] - g_pos[1]) * (g_gamePos[1] - g_pos[1]) +
                                 (g_gamePos[2] - g_pos[2]) * (g_gamePos[2] - g_pos[2])));
        if (tracked) {
            // Lacet et tangage du casque, en degrés (0 = face à l'écran virtuel)
            float head[9];
            QuatToMatrix(quat, head);
            const float yaw = std::atan2(-head[2], head[8]) * 57.29578f;
            const float pitch = std::asin(std::fmax(-1.0f, std::fmin(1.0f, -head[5]))) * 57.29578f;
            LOG_ENGINE("  casque : lacet %.0f, tangage %.0f degres, %s", yaw, pitch,
                       s.headTracking ? "applique" : "non applique (head_tracking=0)");
        } else {
            LOG_ENGINE("  casque : pas de pose (session OpenXR inactive ou casque pas sur la tete)");
        }
    }
}

// --- Rendu : caméra de l'image ----------------------------------------------------------

// Repère de visée de l'image en préparation : le point visé et le départ du rayon, vus par la
// caméra de rendu (position et orientation déjà posées pour cet œil). false : rien à dessiner.
bool ProjectSight(const Settings& s, render::SightOverlay* overlay)
{
    // Ancien mode (laser=3) : un seul repère, celui de la main qui vise, à défaut du regard
    aim::Sight sight;
    const bool fromHand = aim::GetSight(controls::AimHand(), &sight);
    if ((!fromHand && !aim::GetSight(-1, &sight)) || !player::HoldingWeapon())
        return false;
    constexpr float kNear = 0.05f;
    const auto toCamera = [](const float world[3], float camera[3]) {
        const float d[3] = {world[0] - g_renderPos[0], world[1] - g_renderPos[1], world[2] - g_renderPos[2]};
        for (int axis = 0; axis < 3; ++axis)
            camera[axis] = d[0] * g_renderOrient[axis * 3] + d[1] * g_renderOrient[axis * 3 + 1] +
                           d[2] * g_renderOrient[axis * 3 + 2];
    };
    float point[3];
    toCamera(sight.point, point);
    if (point[2] > kNear) {
        overlay->dot = true;
        overlay->dotT[0] = point[0] / point[2];
        overlay->dotT[1] = point[1] / point[2];
    }
    if (fromHand) {
        // Le trait part un peu devant la manette, pas du poignet
        constexpr float kStart = 0.12f;
        float start[3], from[3];
        for (int k = 0; k < 3; ++k)
            start[k] = sight.origin[k] + kStart * s.worldScale * sight.forward[k];
        toCamera(start, from);
        float to[3]{point[0], point[1], point[2]};
        // Segment coupé au plan proche de la caméra
        if (from[2] > kNear || to[2] > kNear) {
            if (from[2] <= kNear || to[2] <= kNear) {
                float* behind = from[2] <= kNear ? from : to;
                const float* front = from[2] <= kNear ? to : from;
                const float t = (kNear - behind[2]) / (front[2] - behind[2]);
                for (int k = 0; k < 3; ++k)
                    behind[k] += (front[k] - behind[k]) * t;
                behind[2] = kNear;
            }
            overlay->beam = true;
            overlay->beamA[0] = from[0] / from[2];
            overlay->beamA[1] = from[1] / from[2];
            overlay->beamB[0] = to[0] / to[2];
            overlay->beamB[1] = to[1] / to[2];
        }
    }
    return overlay->dot || overlay->beam;
}

// Appelé juste après la copie de la caméra finale dans la caméra de rendu. fov : champ de vue
// horizontal que le rendu va utiliser (modifiable).
void OnRenderCamera(double* fov)
{
    const Settings s = CurrentSettings();
    if (++g_renderCopies == 1)
        LOG_ENGINE("camera de rendu : premiere image preparee sur le thread %lu", GetCurrentThreadId());
    g_pair.viewed = true;
    PendingView view;
    view.tick = GetTickCount64();
    if (GameState(s) != kPlaying) {
        // Menu de pause, magasin, garde-robe, garage, cinématique : caméra et champ de vue du
        // jeu, image sur l'écran virtuel
        PushView(view);
        return;
    }
    // Une image de jeu est en préparation : les manettes pilotent le personnage, pas un menu
    xr::NoteGameplayFrame();
    if (s.hfov > 0.0f)
        *fov = s.hfov;
    g_lastFov = static_cast<float>(*fov);

    float quat[4];
    float ipd = 0.063f;
    bool posed = false;
    if (g_pair.active) {
        // Les deux yeux d'une paire : même pose de tête, relevée au début de la paire
        memcpy(quat, g_pair.quat, sizeof(quat));
        ipd = g_pair.ipd;
        posed = true;
    } else {
        posed = xr::GetHeadPose(quat, &ipd);
    }
    if (!posed)
        ++g_copiesWithoutPose;
    const bool tracked = posed && s.headTracking;
    int eye = 0;
    if (tracked) {
        // Orientation propre du jeu la plus récente ; à défaut, ce qui vient d'être copié
        float game[9];
        {
            std::lock_guard lock(g_gameMutex);
            if (g_gameOrientValid)
                memcpy(game, g_gameOrient, sizeof(game));
            else
                memcpy(game, g_renderOrient, sizeof(game));
        }
        float headed[9];
        HeadedOrientation(game, quat, s, headed);
        memcpy(g_renderOrient, headed, sizeof(headed));

        if (s.stereo) {
            // Un œil par image : la caméra se décale d'un demi-écart le long de sa droite
            eye = g_pair.active ? g_pair.eye : static_cast<int>(g_eyeCounter++ & 1);
            const float side = (eye == 0) == !s.swapEyes ? -1.0f : 1.0f;
            const float offset = side * 0.5f * ipd * s.ipdScale * s.worldScale;
            for (int k = 0; k < 3; ++k)
                g_renderPos[k] += offset * headed[k];
        }
    }

    bool sightDrawn = false;
    if (tracked) {
        view.valid = true;
        memcpy(view.quat, quat, sizeof(view.quat));
        view.eye = eye;
        view.pair = g_pair.active && s.stereo ? g_pair.id : 0;
        view.fov = static_cast<float>(*fov);
        if (s.laser == 3 && g_aimReady)
            sightDrawn = ProjectSight(s, &view.sight);
    }
    // Le réticule du jeu est caché tant qu'un repère du mod est dessiné
    g_sightDrawn = sightDrawn || g_laserShown;
    PushView(view);
}

// Préparation d'une image. En vrai relief et en partie : deux fois de suite, œil gauche puis
// œil droit, sans que la simulation avance entre les deux.
int __cdecl HkRenderFrame()
{
    const Settings s = CurrentSettings();
    Pair pair;
    const bool immersed = xr::GetHeadPose(pair.quat, &pair.ipd) && s.headTracking && GameState(s) == kPlaying;
    const float hfov = s.hfov > 0.0f ? s.hfov : g_lastFov;
    if (!s.pairs || !g_pendingCamera || !immersed) {
        // Un œil par image : l'interface se décale pour l'œil qu'OnRenderCamera va choisir
        PlaceHud(s, immersed, s.stereo && immersed ? static_cast<int>(g_eyeCounter & 1) : -1, pair.ipd, hfov);
        return g_renderFrame();
    }

    // Ce que la simulation a demandé au rendu pour cette image, à redonner à l'œil droit
    uint8_t pending[kPendingCameraSize];
    memcpy(pending, g_pendingCamera, sizeof(pending));

    pair.active = true;
    if (++g_pairCounter == 0)
        ++g_pairCounter; // 0 veut dire « image seule »
    pair.id = g_pairCounter;
    pair.eye = 0;
    g_pair = pair;
    PlaceHud(s, true, 0, pair.ipd, hfov);
    const int result = g_renderFrame();
    // Image sans caméra de jeu (chargement, menu principal) : rien à rendre pour l'autre œil
    if (g_pair.viewed) {
        memcpy(g_pendingCamera, pending, sizeof(pending));
        // Pour le thread du rendu, l'œil droit est l'image suivante
        if (s.pairCounter && g_frameCounter)
            ++*g_frameCounter;
        g_pair.eye = 1;
        PlaceHud(s, true, 1, pair.ipd, hfov);
        g_renderFrame();
        ++g_pairsRendered;
    }
    g_pair.active = false;
    return result;
}

double __cdecl HkGetFov()
{
    double fov = g_getFov();
    if (_ReturnAddress() == g_renderReturn)
        OnRenderCamera(&fov);
    else if (const Settings s = CurrentSettings(); s.hfov > 0.0f && GameState(s) == kPlaying)
        fov = s.hfov; // même champ de vue pour les autres appelants (projection 3D -> écran)
    return fov;
}

} // namespace

bool InstallCameraHook()
{
    ReadConfig();
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto rel = [base](const void* p) { return static_cast<unsigned long>(reinterpret_cast<uintptr_t>(p) - base); };
    const auto absolute = [](const uint8_t* operand) {
        uint32_t address = 0;
        memcpy(&address, operand, sizeof(address));
        return reinterpret_cast<float*>(static_cast<uintptr_t>(address));
    };

    // Simulation
    auto* update = static_cast<uint8_t*>(pattern::Find("UpdateCamera", kUpdateCameraSig));
    if (update) {
        uint8_t* copy = pattern::FindIn(update, kFunctionSpan, kOrientCopySig);
        uint8_t* read = pattern::FindIn(update, kFunctionSpan, kPosReadSig);
        if (uint8_t* mode = pattern::FindIn(update, kFunctionSpan, kCameraModeSig))
            g_cameraMode = reinterpret_cast<const int*>(absolute(mode + 1));
        if (copy) {
            g_orient = absolute(copy + 6);
            g_pos = read ? absolute(read + 7) : nullptr; // l'opérande du « mov eax,[x] »
            LOG_ENGINE("camera de simulation : orientation exe+0x%lX, position exe+0x%lX (Juiced : orientation "
                       "ideale 0x025F5B5C, position ideale 0x025F5B20, a la base 0x00400000)",
                       rel(g_orient), g_pos ? rel(g_pos) : 0ul);
            MH_STATUS st = MH_CreateHook(update, reinterpret_cast<void*>(&HkUpdateCamera),
                                         reinterpret_cast<void**>(&g_updateCamera));
            if (st == MH_OK)
                st = MH_EnableHook(update);
            if (st == MH_OK) {
                g_updateCameraTarget = update;
            } else {
                LOG_ENGINE("camera : ECHEC MinHook sur UpdateCamera : %s", MH_StatusToString(st));
                MH_RemoveHook(update);
            }
        } else {
            LOG_ENGINE("camera : orientation finale introuvable dans UpdateCamera");
        }
    }

    // Rendu
    if (auto* site = static_cast<uint8_t*>(pattern::Find("camera de rendu", kRenderCameraSig))) {
        g_renderOrient = absolute(site + kRenderOrientOffset);
        g_renderPos = g_renderOrient - 3; // la position précède l'orientation
        int32_t displacement = 0;
        memcpy(&displacement, site + kGetFovCallOffset + 1, sizeof(displacement));
        uint8_t* getFov = site + kGetFovCallOffset + 5 + displacement;
        g_renderReturn = site + kGetFovCallOffset + 5;
        LOG_ENGINE("camera de rendu : position exe+0x%lX, orientation exe+0x%lX, champ de vue exe+0x%lX, GetFOV "
                   "exe+0x%lX%s",
                   rel(g_renderPos), rel(g_renderOrient), rel(absolute(site + kRenderFovOffset)), rel(getFov),
                   getFov[0] == 0xE9 ? " (deja remplacee, par Juiced : on passe avant)" : "");
        MH_STATUS st =
            MH_CreateHook(getFov, reinterpret_cast<void*>(&HkGetFov), reinterpret_cast<void**>(&g_getFov));
        if (st == MH_OK)
            st = MH_EnableHook(getFov);
        if (st == MH_OK) {
            g_getFovTarget = getFov;
        } else {
            LOG_ENGINE("camera : ECHEC MinHook sur GetFOV : %s", MH_StatusToString(st));
            MH_RemoveHook(getFov);
        }
    }

    // Vrai relief : la préparation d'une image, à appeler deux fois par image de simulation
    if (g_getFovTarget) {
        if (auto* frame = static_cast<uint8_t*>(pattern::Find("preparation d'une image", kRenderFrameSig))) {
            MH_STATUS st = MH_CreateHook(frame, reinterpret_cast<void*>(&HkRenderFrame),
                                         reinterpret_cast<void**>(&g_renderFrame));
            if (st == MH_OK)
                st = MH_EnableHook(frame);
            if (st == MH_OK) {
                g_renderFrameTarget = frame;
            } else {
                LOG_ENGINE("camera : ECHEC MinHook sur la preparation d'une image : %s", MH_StatusToString(st));
                MH_RemoveHook(frame);
            }
        }
        if (g_renderFrameTarget) {
            if (uint8_t* site = pattern::FindIn(static_cast<uint8_t*>(g_renderFrameTarget), 0x200, kPendingCameraSig)) {
                g_pendingCamera = reinterpret_cast<uint8_t*>(absolute(site + kPendingCameraOffset));
                LOG_ENGINE("camera en attente : exe+0x%lX (octet de dessin de la scene : %d)", rel(g_pendingCamera),
                           static_cast<int>(g_pendingCamera[0x6D]));
            } else {
                LOG_ENGINE("camera en attente introuvable dans la preparation d'une image");
            }
        }
        if (!g_renderFrameTarget || !g_pendingCamera)
            LOG_ENGINE("vrai relief indisponible : [Display] mode=3 donnera un oeil par image, comme mode=2");
        if (auto* clock = static_cast<uint8_t*>(pattern::Find("compteur d'images", kFrameCounterSig))) {
            g_frameCounter = reinterpret_cast<int*>(absolute(clock + kFrameCounterOffset));
            LOG_ENGINE("compteur d'images du jeu : exe+0x%lX (vaut %d)", rel(g_frameCounter), *g_frameCounter);
        } else {
            LOG_ENGINE("compteur d'images introuvable : en vrai relief, l'oeil droit risque de rester vide");
        }
    }

    // Pause
    if (auto* unpause = static_cast<uint8_t*>(pattern::Find("fin de pause", kUnpauseSig))) {
        g_pauseCount = reinterpret_cast<const int*>(absolute(unpause + 2));
        LOG_ENGINE("compteur de pauses : exe+0x%lX (vaut %d)", rel(g_pauseCount), *g_pauseCount);
    } else {
        LOG_ENGINE("compteur de pauses introuvable : le menu de pause restera en affichage immersif");
    }

    // Magasins et interfaces de planque
    if (auto* enter = static_cast<uint8_t*>(pattern::Find("entree de magasin", kStoreEnterSig))) {
        g_freePlay = reinterpret_cast<const uint8_t*>(absolute(enter + kFreePlayOffset));
        LOG_ENGINE("interfaces du jeu (magasins, garde-robe, garage) : octet exe+0x%lX (vaut %d, 1 = en jeu)",
                   rel(g_freePlay), static_cast<int>(*g_freePlay));
    } else {
        LOG_ENGINE("octet des interfaces introuvable : magasins, garde-robe et garage resteront en affichage immersif");
    }

    // Cinématiques
    if (auto* start = static_cast<uint8_t*>(pattern::Find("lancement de cinematique", kCutsceneStartSig))) {
        g_cutscene = reinterpret_cast<const uint8_t*>(absolute(start + kCutsceneOffset));
        LOG_ENGINE("cinematiques : octet exe+0x%lX (vaut %d, 1 = cinematique en cours)", rel(g_cutscene),
                   static_cast<int>(*g_cutscene));
    } else {
        LOG_ENGINE("octet des cinematiques introuvable : elles resteront en affichage immersif");
    }

    // Première personne : le joueur et son squelette
    if (g_updateCameraTarget && g_pos) {
        g_playerReady = player::Install();
        // Table des objets du jeu : juste après la signature de UpdateCamera, mov ebp,[ecx+<table>]
        constexpr int kObjectTableOffset = 36;
        if (update[kObjectTableOffset] == 0x8B && update[kObjectTableOffset + 1] == 0xA9)
            player::SetObjectTable(absolute(update + kObjectTableOffset + 2));
        else
            LOG_ENGINE("table des objets introuvable dans UpdateCamera : vehicules non reconnus");
        g_hudReady = hud::Install();
        g_handsReady = hands::Install(&HandFrame);
        g_meleeReady = g_handsReady && melee::Install();
        g_aimReady = aim::Install(g_pos, g_orient, &AimRay);
        controls::Install();
        LOG_ENGINE("mode de camera du jeu : %s", g_cameraMode ? "trouve" : "introuvable (premiere personne dans tous les modes)");
    }

    if (!g_updateCameraTarget && !g_getFovTarget) {
        LOG_ENGINE("camera : rien n'a pu etre detourne, suivi de tete desactive");
        return false;
    }
    LOG_ENGINE("camera : simulation %s, rendu %s, preparation d'image %s, suivi de tete %s",
               g_updateCameraTarget ? "detournee" : "INDISPONIBLE",
               g_getFovTarget ? "detourne" : "INDISPONIBLE (pas d'affichage immersif ni de relief)",
               g_renderFrameTarget ? "detournee" : "INDISPONIBLE (pas de vrai relief)",
               CurrentSettings().headTracking ? "actif" : "coupe");
    return true;
}

void RemoveCameraHook()
{
    for (void* target : {g_updateCameraTarget, g_getFovTarget, g_renderFrameTarget}) {
        if (target) {
            MH_DisableHook(target);
            MH_RemoveHook(target);
        }
    }
    g_updateCameraTarget = g_getFovTarget = g_renderFrameTarget = nullptr;
    hands::Remove();
    melee::Remove();
    aim::Remove();
    controls::Remove();
    g_aimReady = false;
    if (g_applied && g_orient)
        memcpy(g_orient, g_gameOrient, sizeof(g_gameOrient));
    g_applied = false;
    if (g_posApplied && g_pos)
        memcpy(g_pos, g_gamePos, sizeof(g_gamePos));
    g_posApplied = false;
}

bool TakeFrameView(float headQuat[4], int* eye, float* hfovDeg, unsigned* pair, render::SightOverlay* sight)
{
    PendingView view;
    {
        std::lock_guard lock(g_viewMutex);
        // Vues trop vieilles : leur image n'est jamais arrivée (retour au menu principal...)
        const ULONGLONG now = GetTickCount64();
        while (g_pendingCount && now > g_pending[0].tick && now - g_pending[0].tick > kViewMaxAgeMs) {
            g_pending[0] = g_pending[1];
            --g_pendingCount;
        }
        ++g_pendingAtPresent[g_pendingCount];
        if (!g_pendingCount)
            return false;
        view = g_pending[0];
        g_pending[0] = g_pending[1];
        --g_pendingCount;
    }
    if (!view.valid || !(view.fov > 1.0f && view.fov < 179.0f))
        return false;
    memcpy(headQuat, view.quat, sizeof(view.quat));
    *eye = view.eye;
    *hfovDeg = view.fov;
    *pair = view.pair;
    *sight = view.sight;
    return true;
}

} // namespace engine
