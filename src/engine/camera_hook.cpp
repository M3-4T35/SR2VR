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
// Aucune adresse codée en dur : fonctions trouvées par signature, adresses des variables lues
// dans leur code.
#include "engine/camera_hook.h"
#include "util/config.h"
#include "util/log.h"
#include "util/pattern.h"
#include "xr/xr_input.h"
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

// Orientation de la caméra avec la tête : celle du jeu (remise à plat si demandé) composée
// avec le casque
void HeadedOrientation(const float game[9], const float quat[4], const Settings& s, float out[9])
{
    float head[9], base[9];
    QuatToMatrix(quat, head);
    if (s.levelHorizon)
        LevelOrientation(game, base);
    else
        memcpy(base, game, sizeof(base));
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

ULONGLONG g_lastStats = 0;
int g_lastState = -1;
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
    if (tracked && s.headTracking && state == kPlaying) {
        HeadedOrientation(game, quat, s, g_written);
        memcpy(g_orient, g_written, sizeof(g_written));
        g_applied = true;
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

    if (tracked) {
        view.valid = true;
        memcpy(view.quat, quat, sizeof(view.quat));
        view.eye = eye;
        view.pair = g_pair.active && s.stereo ? g_pair.id : 0;
        view.fov = static_cast<float>(*fov);
    }
    PushView(view);
}

// Préparation d'une image. En vrai relief et en partie : deux fois de suite, œil gauche puis
// œil droit, sans que la simulation avance entre les deux.
int __cdecl HkRenderFrame()
{
    const Settings s = CurrentSettings();
    Pair pair;
    if (!s.pairs || !g_pendingCamera || !s.headTracking || GameState(s) != kPlaying ||
        !xr::GetHeadPose(pair.quat, &pair.ipd))
        return g_renderFrame();

    // Ce que la simulation a demandé au rendu pour cette image, à redonner à l'œil droit
    uint8_t pending[kPendingCameraSize];
    memcpy(pending, g_pendingCamera, sizeof(pending));

    pair.active = true;
    if (++g_pairCounter == 0)
        ++g_pairCounter; // 0 veut dire « image seule »
    pair.id = g_pairCounter;
    pair.eye = 0;
    g_pair = pair;
    const int result = g_renderFrame();
    // Image sans caméra de jeu (chargement, menu principal) : rien à rendre pour l'autre œil
    if (g_pair.viewed) {
        memcpy(g_pendingCamera, pending, sizeof(pending));
        // Pour le thread du rendu, l'œil droit est l'image suivante
        if (s.pairCounter && g_frameCounter)
            ++*g_frameCounter;
        g_pair.eye = 1;
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
    if (g_applied && g_orient)
        memcpy(g_orient, g_gameOrient, sizeof(g_gameOrient));
    g_applied = false;
}

bool TakeFrameView(float headQuat[4], int* eye, float* hfovDeg, unsigned* pair)
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
    return true;
}

} // namespace engine
