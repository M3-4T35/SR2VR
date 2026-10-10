// Caméra de Saints Row 2.
//
// La caméra du jeu est un ensemble de variables globales, mises à jour une fois par image par
// une fonction sans argument (« UpdateCamera » dans les sources de Juiced, 0x004932F0 dans
// l'exe modifié par Juiced) : le mode de caméra courant calcule une position et une
// orientation idéales, puis la fonction les recopie dans la position et l'orientation
// finales, lues ensuite par le rendu (copie vers la caméra de rendu), le culling, le son et
// la visée. L'orientation est une matrice 3x3 de Volition : trois vecteurs du monde, droite
// (rvec), haut (uvec), avant (fvec), repère main gauche, Y vers le haut.
//
// On détourne cette fonction : après l'original, l'orientation finale est composée avec celle
// du casque. Avant l'appel suivant, on rend au jeu sa propre orientation, pour qu'un mode de
// caméra qui repartirait de l'orientation finale ne cumule pas la rotation de la tête.
//
// Aucune adresse codée en dur : la fonction est trouvée par signature, et l'adresse de
// l'orientation finale est lue dans son code (première copie de 9 mots : orientation finale
// vers orientation précédente).
#include "engine/camera_hook.h"
#include "util/log.h"
#include "util/pattern.h"
#include "xr/xr_session.h"

#include <MinHook.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

namespace engine {
namespace {

// Début de la fonction : mov eax,[handle de l'objet suivi] ; sub esp,8Ch ; push ebx ; push ebp ;
// xor ebx,ebx ; cmp eax,ebx ; push esi ; push edi ; je ... ; mov ecx,eax ; and ecx,0FFFh ; shl ecx,4
constexpr char kUpdateCameraSig[] =
    "A1 ?? ?? ?? ?? 81 EC 8C 00 00 00 53 55 33 DB 3B C3 56 57 0F 84 ?? ?? ?? ?? 8B C8 81 E1 FF 0F 00 00 C1 E1 04";
// Dans la fonction : mov ecx,9 ; mov esi,<orientation finale> ; mov edi,<orientation précédente>
constexpr char kOrientCopySig[] = "B9 09 00 00 00 BE ?? ?? ?? ?? BF ?? ?? ?? ??";
// Dans la fonction : mov ecx,[pos.y] ; mov eax,[pos.x] ; mov edx,[pos.z] (position finale)
constexpr char kPosReadSig[] = "8B 0D ?? ?? ?? ?? A1 ?? ?? ?? ?? 8B 15 ?? ?? ?? ??";
constexpr size_t kFunctionSpan = 0x520;

using UpdateCameraFn = void(__cdecl*)();
UpdateCameraFn g_original = nullptr;
void* g_target = nullptr;

float* g_orient = nullptr; // rvec, uvec, fvec (9 floats)
float* g_pos = nullptr;    // x, y, z

float g_gameOrient[9]{}; // orientation calculée par le jeu à la dernière image
float g_written[9]{};    // ce que nous avons écrit à la place
bool g_applied = false;

// Réglages de mise au point, relus dans sr2_vr.ini (section [Camera]) une fois par seconde
std::string g_iniPath;
bool g_headTracking = true;
bool g_mirror[3]{}; // miroir de l'axe x, y, z du repère du casque (inverse deux angles sur trois)
ULONGLONG g_lastConfig = 0;
ULONGLONG g_lastStats = 0;
unsigned long long g_calls = 0;

void ReadConfig()
{
    g_headTracking = GetPrivateProfileIntA("Camera", "head_tracking", 1, g_iniPath.c_str()) != 0;
    g_mirror[0] = GetPrivateProfileIntA("Camera", "mirror_x", 0, g_iniPath.c_str()) != 0;
    g_mirror[1] = GetPrivateProfileIntA("Camera", "mirror_y", 0, g_iniPath.c_str()) != 0;
    g_mirror[2] = GetPrivateProfileIntA("Camera", "mirror_z", 0, g_iniPath.c_str()) != 0;
}

// head : rotation du casque, axes du casque en colonnes dans le repère OpenXR (x droite,
// y haut, z arrière). La caméra du jeu a pour axes (droite, haut, avant) : avant = -z.
void ComposeWithHead(const float game[9], const float head[9], float out[9])
{
    float h[9];
    memcpy(h, head, sizeof(h));
    // Miroir d'un axe : S * R * S, avec S diagonale (-1 sur l'axe)
    for (int axis = 0; axis < 3; ++axis) {
        if (!g_mirror[axis])
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

void __cdecl HkUpdateCamera()
{
    // Le jeu retrouve sa propre caméra, sauf s'il l'a déjà réécrite lui-même
    if (g_applied && memcmp(g_orient, g_written, sizeof(g_written)) == 0)
        memcpy(g_orient, g_gameOrient, sizeof(g_gameOrient));
    g_applied = false;

    g_original();

    memcpy(g_gameOrient, g_orient, sizeof(g_gameOrient));
    const ULONGLONG now = GetTickCount64();
    if (now - g_lastConfig >= 1000) {
        g_lastConfig = now;
        ReadConfig();
    }

    float head[9];
    const bool tracked = xr::GetHeadRotation(head);
    if (tracked && g_headTracking) {
        ComposeWithHead(g_gameOrient, head, g_written);
        memcpy(g_orient, g_written, sizeof(g_written));
        g_applied = true;
    }

    // Relevé : première image, puis toutes les 10 s
    if (++g_calls == 1 || now - g_lastStats >= 10000) {
        g_lastStats = now;
        const float* m = g_gameOrient;
        LOG_ENGINE("camera : position (%.1f, %.1f, %.1f), avant du jeu (%.2f, %.2f, %.2f), haut (%.2f, %.2f, %.2f)",
                   g_pos ? g_pos[0] : 0.0f, g_pos ? g_pos[1] : 0.0f, g_pos ? g_pos[2] : 0.0f, m[6], m[7], m[8], m[3],
                   m[4], m[5]);
        if (tracked) {
            // Lacet et tangage du casque, en degrés (0 = face à l'écran virtuel)
            const float yaw = std::atan2(-head[2], head[8]) * 57.29578f;
            const float pitch = std::asin(std::fmax(-1.0f, std::fmin(1.0f, -head[5]))) * 57.29578f;
            LOG_ENGINE("  casque : lacet %.0f, tangage %.0f degres, %s -> avant affiche (%.2f, %.2f, %.2f)", yaw, pitch,
                       g_headTracking ? "applique" : "non applique (head_tracking=0)", g_orient[6], g_orient[7],
                       g_orient[8]);
        } else {
            LOG_ENGINE("  casque : pas de pose (session OpenXR inactive ou casque non suivi)");
        }
    }
}

} // namespace

bool InstallCameraHook()
{
    char path[MAX_PATH]{};
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&InstallCameraHook), &self);
    GetModuleFileNameA(self, path, MAX_PATH);
    g_iniPath = path;
    g_iniPath.replace(g_iniPath.find_last_of('\\') + 1, std::string::npos, "sr2_vr.ini");
    ReadConfig();

    auto* function = static_cast<uint8_t*>(pattern::Find("UpdateCamera", kUpdateCameraSig));
    if (!function) {
        LOG_ENGINE("camera : fonction de mise a jour introuvable, suivi de tete desactive");
        return false;
    }
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    uint8_t* copy = pattern::FindIn(function, kFunctionSpan, kOrientCopySig);
    uint8_t* read = pattern::FindIn(function, kFunctionSpan, kPosReadSig);
    if (!copy) {
        LOG_ENGINE("camera : orientation finale introuvable dans la fonction, suivi de tete desactive");
        return false;
    }
    uint32_t orient = 0, pos = 0;
    memcpy(&orient, copy + 6, sizeof(orient));
    g_orient = reinterpret_cast<float*>(static_cast<uintptr_t>(orient));
    if (read) {
        memcpy(&pos, read + 7, sizeof(pos)); // l'opérande du « mov eax,[x] »
        g_pos = reinterpret_cast<float*>(static_cast<uintptr_t>(pos));
    }
    LOG_ENGINE("camera : orientation finale exe+0x%lX, position exe+0x%lX (Juiced : orientation ideale 0x025F5B5C, "
               "position ideale 0x025F5B20, a la base 0x00400000)",
               static_cast<unsigned long>(orient - base), static_cast<unsigned long>(pos ? pos - base : 0));

    // La matrice doit être lisible et ressembler à une rotation avant qu'on y écrive
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(g_orient, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        !(info.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY))) {
        LOG_ENGINE("camera : memoire de l'orientation non inscriptible, suivi de tete desactive");
        return false;
    }

    MH_STATUS st = MH_CreateHook(function, reinterpret_cast<void*>(&HkUpdateCamera), reinterpret_cast<void**>(&g_original));
    if (st == MH_OK)
        st = MH_EnableHook(function);
    if (st != MH_OK) {
        LOG_ENGINE("camera : ECHEC MinHook sur UpdateCamera : %s", MH_StatusToString(st));
        MH_RemoveHook(function);
        return false;
    }
    g_target = function;
    LOG_ENGINE("camera : UpdateCamera detournee, suivi de tete %s (sr2_vr.ini, [Camera] head_tracking)",
               g_headTracking ? "actif" : "coupe");
    return true;
}

void RemoveCameraHook()
{
    if (!g_target)
        return;
    MH_DisableHook(g_target);
    MH_RemoveHook(g_target);
    g_target = nullptr;
    if (g_applied && g_orient)
        memcpy(g_orient, g_gameOrient, sizeof(g_gameOrient));
    g_applied = false;
}

} // namespace engine
