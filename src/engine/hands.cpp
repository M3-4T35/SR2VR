#include "engine/hands.h"
#include "engine/player.h"
#include "util/log.h"
#include "util/pattern.h"

#include <MinHook.h>
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace engine::hands {
namespace {

// Cible d'IK d'un membre : sub esp,10h ; mov eax,[esp+14h] ; push ebx ; push ebp ;
// mov ebp,[esp+20h] ; push esi ; push edi ; push ebp ; push eax ; call ... ; test eax,eax ;
// mov [esp+1Ch],eax. Trois arguments sur la pile, nettoyés par la fonction : le membre, le
// personnage, la position de sortie ; résultat dans al.
constexpr char kLimbTargetSig[] = "83 EC 10 8B 44 24 14 53 55 8B 6C 24 20 56 57 55 50 E8 ?? ?? ?? ?? 85 C0 89 44 24 1C";
// Pose du squelette, trois fonctions du personnage animé (__stdcall, le personnage animé en
// premier argument). Il garde pour chaque os une matrice d'articulation (+4) et une matrice
// finale (+0), dans le repère du personnage, et des drapeaux (+8, 12 octets par os : bit 0
// articulation à jour, bit 1 matrice finale à jour, bit 2 orientation imposée par l'IK du jeu).
//  - relever les transformations locales des os (par rapport à leur parent), 48 octets par os ;
//  - recalculer, à partir de transformations locales, les articulations qui ne sont plus à jour ;
//  - recalculer les matrices finales qui ne sont plus à jour.
// C'est la suite que le jeu applique lui-même dans sa passe d'IK : relever, marquer les os du
// membre comme périmés, recalculer.
constexpr char kGetPoseSig[] = "8B 54 24 04 8B 82 28 02 00 00 8B 40 04 81 EC 94 00 00 00 83 F8 01 53 55 56 57";
constexpr char kSetPoseSig[] =
    "55 8B EC 83 E4 F0 81 EC C4 00 00 00 83 7D 10 00 53 8B 5D 08 56 57 75 07 C7 45 10 ?? ?? ?? ??";
constexpr char kUpdatePoseSig[] = "83 EC 48 53 8B 5C 24 50 55 33 ED 39 6C 24 58 56 75 08 C7 44 24 5C ?? ?? ?? ?? 8B 43 18 8B "
                                  "48 38 8B 93 28 02 00 00";
// Départ d'un tir : position et orientation de la bouche du canon de l'arme tenue (personnage
// dans edi ; sur la pile : position de sortie, orientation de sortie, « autre main » ; pile
// nettoyée par l'appelant). push ecx ; test [edi+0CCh],40000000h ; push ebx ; push ebp ;
// push esi ; je ... ; cmp byte [esp+1Ch],0 ; sete al ; mov [esp+1Ch],al ; mov ecx,[edi+0D74h]
constexpr char kMuzzleSig[] = "51 F7 87 CC 00 00 00 00 00 00 40 53 55 56 74 0C 80 7C 24 1C 00 0F 94 C0 88 44 24 1C 8B 8F 74 "
                              "0D 00 00";

// Dans le personnage du joueur
constexpr size_t kRootPos = 0x14;    // position de la racine (3 floats)
constexpr size_t kRootOrient = 0x20; // orientation 3x3 (droite, haut, avant)
constexpr size_t kLimbTable = 0x6E0; // quatre membres de 16 octets
constexpr size_t kLimbIndex = 6;     // dans le membre : 0 bras gauche, 1 bras droit, 2 et 3 jambes
// Dans le personnage animé
constexpr size_t kInstMatrices = 4;  // matrices des articulations dans le repère du personnage, 48 octets
constexpr size_t kInstFlags = 8;     // drapeaux des os, 12 octets par os
constexpr size_t kFlagSize = 12;
constexpr uint32_t kFlagStale = 1 | 2 | 4; // à effacer pour qu'un os soit recalculé d'après sa transformation locale
constexpr size_t kInstRig = 0x18;    // squelette
constexpr size_t kInstMap = 0x228;   // correspondances : nombre d'os en +4, case de chaque os en +0x108
// L'écriture d'une pose réapplique à la racine l'échelle du personnage (si +0x252 : facteurs
// x, y, z en +0x5D0) et lui ajoute une hauteur (+0x5DC). La pose qu'on relève les contient
// déjà : on les retire de la racine avant d'écrire, sinon elles compteraient deux fois.
constexpr size_t kInstScaled = 0x252;
constexpr size_t kInstScale = 0x5D0;
constexpr size_t kInstRootLift = 0x5DC;
constexpr size_t kMapCount = 4;
constexpr size_t kMapSlots = 0x108;
// Dans le squelette
constexpr size_t kRigBoneCount = 0x24;
constexpr size_t kRigBones = 0x38;   // table des os : 36 octets, nom en premier, parent en +0x1C
constexpr size_t kBoneSize = 36;
constexpr size_t kBoneParent = 0x1C;
constexpr int kMaxBones = 256;
constexpr int kFloatsPerBone = 12;   // orientation 3x3 puis position

using LimbTargetFn = bool(__stdcall*)(uint8_t* limb, uint8_t* character, float* target);
using GetPoseFn = void(__stdcall*)(uint8_t* instance, float* locals);
using SetPoseFn = void(__stdcall*)(uint8_t* instance, const float* locals, void* unused, int unused2);
using UpdatePoseFn = void(__stdcall*)(uint8_t* instance, void* unused, int unused2);
LimbTargetFn g_limbTarget = nullptr;
GetPoseFn g_getPose = nullptr;
SetPoseFn g_setPose = nullptr;
UpdatePoseFn g_updatePose = nullptr;
void* g_muzzle = nullptr;
void* g_target = nullptr;
FrameFn g_frameFn = nullptr;

bool g_active = false;
float g_eyeFromRoot[3]{};
float g_headRight[3]{1.0f, 0.0f, 0.0f}; // droite de la tête, dans le monde
float g_torsoMax = 0.0f;                // tour maximal du buste vers le regard, en radians (0 = coupé)
unsigned g_torsoCount = 0;
float g_torsoWorst = 0.0f;              // plus grand tour appliqué depuis le dernier relevé (radians)
bool g_torsoShown = false;

// Os des bras, retrouvés par leur nom dans le squelette du joueur (0 gauche, 1 droite)
struct Arm {
    int shoulder = -1, elbow = -1, hand = -1;                   // os tournés
    int shoulderParent = -1, elbowParent = -1, handParent = -1; // leurs parents
};
uintptr_t g_rig = 0;
bool g_rigOk = false;
Arm g_arms[2];
// Os entraînés par chaque épaule (elle comprise) : ceux qu'il faut recalculer après la pose
constexpr int kMaxArmBones = 64;
int g_armBones[2][kMaxArmBones];
int g_armBoneCount[2]{};
// Buste : l'os « Spine » (au-dessus du bassin), son parent, et tout ce qu'il entraîne (colonne,
// tête, bras)
int g_spine = -1, g_spineParent = -1;
int g_spineBones[kMaxBones];
int g_spineBoneCount = 0;
// Sens des transformations locales, constaté sur la pose du jeu : 1 = local[i][j] est l'axe i
// de l'os le long de l'axe j de son parent, 2 = la transposée, 0 = pas encore constaté,
// -1 = ni l'un ni l'autre (bras laissés à l'animation)
int g_convention = 0;

// Arme dans la main, mesurée sur la pose de l'animation : orientation de la bouche du canon
// dans le repère de la main (k[i][j] = axe i du canon le long de l'axe j de la main) et
// position de la bouche du canon depuis le poignet, dans les axes de la main
struct Grip {
    bool valid = false;
    float k[9]{};
    float offset[3]{};
};
Grip g_grip[2];
// Main vide ou arme blanche : il n'y a pas de canon à poser sur le rayon de visée. La main prend
// l'attitude de celle qui tient un pistolet pointé comme la manette, ce qu'est à peu près une
// main fermée sur une manette. Relevé sur les pistolets du jeu (log de la 0.18.0) : direction
// du canon et haut de l'arme dans les axes de l'os de la main, gauche puis droite.
constexpr float kRestForward[2][3] = {{-0.60f, -0.73f, 0.34f}, {0.59f, -0.73f, 0.34f}};
constexpr float kRestUp[2][3] = {{-0.14f, 0.51f, 0.85f}, {-0.11f, 0.34f, 0.93f}};
Grip g_restGrip[2];
float g_meleeTilt = 0.0f; // arme blanche : inclinaison vers l'avant, en radians
bool g_restShown = false;
// Poing de chaque main dans le monde après la dernière pose (pour les coups portés au geste)
float g_fist[2][3]{};
ULONGLONG g_fistTick[2]{};
constexpr float kFistAhead = 0.09f;         // du poignet aux phalanges, le long de l'avant-bras
constexpr ULONGLONG kFistMaxAgeMs = 150;

float g_locals[kMaxBones * kFloatsPerBone];
bool g_posed[2]{};
bool g_shown[2]{};
bool g_gripShown[2]{};
unsigned g_posedCount[2]{};
float g_worstError = 0.0f;
bool g_faulted = false;
int g_skipShown = 0; // dernière raison annoncée pour laquelle les bras ne sont pas posés

// --- Petits calculs ---------------------------------------------------------------------

float Dot(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

void Cross(const float a[3], const float b[3], float out[3])
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

bool Normalize(float v[3])
{
    const float length = std::sqrt(Dot(v, v));
    if (!(length > 1e-6f))
        return false;
    for (int k = 0; k < 3; ++k)
        v[k] /= length;
    return true;
}

// Plus petite rotation qui amène la direction a sur la direction b (unitaires) : matrice 3x3
// appliquée aux vecteurs colonnes
void RotationBetween(const float a[3], const float b[3], float r[9])
{
    float v[3];
    Cross(a, b, v);
    const float c = Dot(a, b);
    if (c < -0.9999f) {
        // Demi-tour : autour d'un axe quelconque perpendiculaire à a
        float axis[3]{1.0f, 0.0f, 0.0f};
        if (std::fabs(a[0]) > 0.9f) {
            axis[0] = 0.0f;
            axis[1] = 1.0f;
        }
        float perp[3];
        Cross(a, axis, perp);
        Normalize(perp);
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                r[i * 3 + j] = 2.0f * perp[i] * perp[j] - (i == j ? 1.0f : 0.0f);
        return;
    }
    const float k = 1.0f / (1.0f + c);
    r[0] = c + k * v[0] * v[0];
    r[1] = k * v[0] * v[1] - v[2];
    r[2] = k * v[0] * v[2] + v[1];
    r[3] = k * v[1] * v[0] + v[2];
    r[4] = c + k * v[1] * v[1];
    r[5] = k * v[1] * v[2] - v[0];
    r[6] = k * v[2] * v[0] - v[1];
    r[7] = k * v[2] * v[1] + v[0];
    r[8] = c + k * v[2] * v[2];
}

void Rotate(const float r[9], const float v[3], float out[3])
{
    const float x = r[0] * v[0] + r[1] * v[1] + r[2] * v[2];
    const float y = r[3] * v[0] + r[4] * v[1] + r[5] * v[2];
    const float z = r[6] * v[0] + r[7] * v[1] + r[8] * v[2];
    out[0] = x;
    out[1] = y;
    out[2] = z;
}

// Axes d'une matrice du jeu, ramenés à la longueur 1 (un personnage à l'échelle a des axes
// plus longs ou plus courts)
void UnitAxes(const float* matrix, float out[9])
{
    memcpy(out, matrix, 9 * sizeof(float));
    for (int axis = 0; axis < 3; ++axis)
        Normalize(out + axis * 3);
}

// Tourne les trois axes (lignes) d'une orientation
void RotateAxes(const float r[9], const float in[9], float out[9])
{
    for (int axis = 0; axis < 3; ++axis)
        Rotate(r, in + axis * 3, out + axis * 3);
}

// Orientation locale d'un os par rapport à son parent, dans le sens constaté
void ToLocal(const float child[9], const float parent[9], float local[9])
{
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            const float value = Dot(child + i * 3, parent + j * 3);
            if (g_convention == 2)
                local[j * 3 + i] = value;
            else
                local[i * 3 + j] = value;
        }
}

// --- Squelette --------------------------------------------------------------------------

int FindBone(uintptr_t table, int count, const char* name)
{
    for (int i = 0; i < count; ++i) {
        const char* candidate = *reinterpret_cast<const char* const*>(table + static_cast<size_t>(i) * kBoneSize);
        if (candidate && _stricmp(candidate, name) == 0)
            return i;
    }
    return -1;
}

void ResolveRig(uintptr_t rig)
{
    g_rig = rig;
    g_rigOk = false;
    g_convention = 0;
    g_grip[0].valid = g_grip[1].valid = false;
    const int count = *reinterpret_cast<const int*>(rig + kRigBoneCount);
    const uintptr_t table = *reinterpret_cast<const uintptr_t*>(rig + kRigBones);
    if (!table || count < 2 || count > kMaxBones)
        return;
    static const char* const kNames[2][3] = {{"L-UpperArmTwist1", "L-ForeTwist", "L-Hand"},
                                             {"R-UpperArmTwist1", "R-ForeTwist", "R-Hand"}};
    bool ok = true;
    for (int hand = 0; hand < 2; ++hand) {
        Arm& arm = g_arms[hand];
        arm.shoulder = FindBone(table, count, kNames[hand][0]);
        arm.elbow = FindBone(table, count, kNames[hand][1]);
        arm.hand = FindBone(table, count, kNames[hand][2]);
        const auto parent = [table, count](int bone) {
            if (bone < 0)
                return -1;
            const int p = *reinterpret_cast<const int*>(table + static_cast<size_t>(bone) * kBoneSize + kBoneParent);
            return p >= 0 && p < count ? p : -1;
        };
        arm.shoulderParent = parent(arm.shoulder);
        arm.elbowParent = parent(arm.elbow);
        arm.handParent = parent(arm.hand);
        ok = ok && arm.shoulderParent >= 0 && arm.elbowParent >= 0 && arm.handParent >= 0;
        // Descendants de l'épaule : on remonte les parents de chaque os
        g_armBoneCount[hand] = 0;
        for (int bone = 0; ok && bone < count; ++bone) {
            int ancestor = bone;
            for (int depth = 0; depth < 32 && ancestor >= 0 && ancestor != arm.shoulder; ++depth)
                ancestor = parent(ancestor);
            if (ancestor == arm.shoulder && g_armBoneCount[hand] < kMaxArmBones)
                g_armBones[hand][g_armBoneCount[hand]++] = bone;
        }
    }
    // Buste
    g_spine = FindBone(table, count, "Spine");
    g_spineParent = g_spine >= 0 ? *reinterpret_cast<const int*>(table + static_cast<size_t>(g_spine) * kBoneSize + kBoneParent) : -1;
    g_spineBoneCount = 0;
    for (int bone = 0; g_spine >= 0 && g_spineParent >= 0 && g_spineParent < count && bone < count; ++bone) {
        int ancestor = bone;
        for (int depth = 0; depth < 32 && ancestor >= 0 && ancestor < count && ancestor != g_spine; ++depth)
            ancestor = *reinterpret_cast<const int*>(table + static_cast<size_t>(ancestor) * kBoneSize + kBoneParent);
        if (ancestor == g_spine)
            g_spineBones[g_spineBoneCount++] = bone;
    }
    g_rigOk = ok;
    if (ok)
        LOG_ENGINE("mains : os des bras trouves (gauche : epaule %d, coude %d, main %d, %d os entraines ; droite : %d, "
                   "%d, %d, %d os)",
                   g_arms[0].shoulder, g_arms[0].elbow, g_arms[0].hand, g_armBoneCount[0], g_arms[1].shoulder,
                   g_arms[1].elbow, g_arms[1].hand, g_armBoneCount[1]);
    else
        LOG_ENGINE("mains : os des bras introuvables dans ce squelette, bras laisses a l'animation");
}

// La fonction du jeu qui donne la bouche du canon : personnage dans edi, trois arguments sur
// la pile, nettoyée par l'appelant
#if defined(_MSC_VER) && !defined(__clang__)
void CallMuzzle(void* fn, uint8_t* character, float* outPos, float* outOrient, int offhand)
{
    __asm {
        push edi
        mov edi, character
        push offhand
        push outOrient
        push outPos
        call fn
        add esp, 0Ch
        pop edi
    }
}
#else
// Compilation de contrôle hors MSVC : pas d'appel (la DLL se compile avec MSVC)
void CallMuzzle(void*, uint8_t*, float*, float*, int) {}
#endif

// --- Pose des bras ----------------------------------------------------------------------

struct Skeleton {
    uint8_t* instance = nullptr;
    float* matrices = nullptr; // articulations, repère du personnage
    uint8_t* flags = nullptr;
    const uint8_t* map = nullptr;
    int count = 0;

    int Slot(int bone) const
    {
        const int slot = *reinterpret_cast<const int16_t*>(map + kMapSlots + static_cast<size_t>(bone) * 2);
        return slot >= 0 && slot < count ? slot : -1;
    }
    float* Matrix(int slot) const { return matrices + static_cast<size_t>(slot) * kFloatsPerBone; }
};

// Mesure, sur la pose de l'animation, comment l'arme est tenue dans la main
void MeasureGrip(uint8_t* character, const Skeleton& skeleton, int hand)
{
    const int slot = skeleton.Slot(g_arms[hand].hand);
    if (slot < 0)
        return;
    float muzzlePos[3]{NAN, NAN, NAN}, muzzleOrient[9];
    for (float& value : muzzleOrient)
        value = NAN;
    CallMuzzle(g_muzzle, character, muzzlePos, muzzleOrient, hand == 0 ? 1 : 0);
    for (float value : muzzleOrient)
        if (!(value >= -3.0f && value <= 3.0f))
            return;
    if (!(std::fabs(muzzlePos[0]) < 1e6f))
        return;
    for (int axis = 0; axis < 3; ++axis)
        if (!Normalize(muzzleOrient + axis * 3))
            return;

    // Main dans le monde : axes du personnage puis de la racine
    const float* root = reinterpret_cast<const float*>(character + kRootPos);
    const float* rootOrient = reinterpret_cast<const float*>(character + kRootOrient);
    const float* matrix = skeleton.Matrix(slot);
    float axes[9], handOrient[9], handPos[3];
    UnitAxes(matrix, axes);
    for (int axis = 0; axis < 3; ++axis)
        for (int k = 0; k < 3; ++k)
            handOrient[axis * 3 + k] = axes[axis * 3] * rootOrient[k] + axes[axis * 3 + 1] * rootOrient[3 + k] +
                                       axes[axis * 3 + 2] * rootOrient[6 + k];
    for (int k = 0; k < 3; ++k)
        handPos[k] = root[k] + matrix[9] * rootOrient[k] + matrix[10] * rootOrient[3 + k] + matrix[11] * rootOrient[6 + k];

    Grip grip;
    const float delta[3] = {muzzlePos[0] - handPos[0], muzzlePos[1] - handPos[1], muzzlePos[2] - handPos[2]};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j)
            grip.k[i * 3 + j] = Dot(muzzleOrient + i * 3, handOrient + j * 3);
        grip.offset[i] = Dot(delta, handOrient + i * 3);
    }
    if (Dot(delta, delta) > 1.5f * 1.5f)
        return; // bouche du canon à plus de 1,5 m du poignet : mesure aberrante
    grip.valid = true;
    g_grip[hand] = grip;
    if (!g_gripShown[hand]) {
        g_gripShown[hand] = true;
        LOG_ENGINE("mains : arme de la main %s mesuree : bouche du canon a (%.3f, %.3f, %.3f) du poignet dans les axes "
                   "de la main, canon le long de (%.2f, %.2f, %.2f), haut de l'arme (%.2f, %.2f, %.2f)",
                   hand ? "droite" : "gauche", grip.offset[0], grip.offset[1], grip.offset[2], grip.k[6], grip.k[7],
                   grip.k[8], grip.k[3], grip.k[4], grip.k[5]);
    }
}

// Pose un bras dans g_locals. target : où doit arriver le poignet, dans le repère du personnage.
// held : comment la main tient son arme (nullptr : main laissée à l'animation). aligned : arme à
// feu, sa bouche est posée sur le rayon de visée ; sinon le poignet va à la manette.
bool PoseArm(const Skeleton& skeleton, int hand, const float* otherShoulder, const float gripWorld[3],
             const float originWorld[3], const float aimOrient[9], const float* root, const float* rootOrient,
             const Grip* held, bool aligned, float wristTarget[3])
{
    const Arm& arm = g_arms[hand];
    const int slots[6] = {skeleton.Slot(arm.shoulder),       skeleton.Slot(arm.elbow),       skeleton.Slot(arm.hand),
                          skeleton.Slot(arm.shoulderParent), skeleton.Slot(arm.elbowParent), skeleton.Slot(arm.handParent)};
    for (int slot : slots)
        if (slot < 0)
            return false;
    const float* shoulder = skeleton.Matrix(slots[0]);
    const float* elbow = skeleton.Matrix(slots[1]);
    const float* wrist = skeleton.Matrix(slots[2]);

    // Orientation voulue de la main et position voulue du poignet, dans le monde
    float handWorld[9];
    float wristWorld[3]{gripWorld[0], gripWorld[1], gripWorld[2]};
    if (held) {
        // Le canon prend l'orientation du rayon de visée : main = (canon dans la main)^-1 x rayon
        const Grip& grip = *held;
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k)
                handWorld[j * 3 + k] = grip.k[j] * aimOrient[k] + grip.k[3 + j] * aimOrient[3 + k] +
                                       grip.k[6 + j] * aimOrient[6 + k];
    }
    if (held && aligned) {
        // Bouche du canon sur le rayon, au plus près de là où la tient la vraie main
        const Grip& grip = *held;
        float muzzle[3], onRay[3];
        for (int k = 0; k < 3; ++k)
            muzzle[k] = grip.offset[0] * handWorld[k] + grip.offset[1] * handWorld[3 + k] + grip.offset[2] * handWorld[6 + k];
        float along = 0.0f;
        for (int k = 0; k < 3; ++k)
            along += (gripWorld[k] + muzzle[k] - originWorld[k]) * aimOrient[6 + k];
        for (int k = 0; k < 3; ++k) {
            onRay[k] = originWorld[k] + along * aimOrient[6 + k];
            wristWorld[k] = onRay[k] - muzzle[k];
        }
    }
    // Dans le repère du personnage
    float target[3];
    {
        const float delta[3] = {wristWorld[0] - root[0], wristWorld[1] - root[1], wristWorld[2] - root[2]};
        for (int axis = 0; axis < 3; ++axis)
            target[axis] = Dot(delta, rootOrient + axis * 3);
    }

    // IK à deux os : épaule fixe, coude sur le cercle des positions possibles, du côté du bas
    // et de l'extérieur du corps
    const float* s = shoulder + 9;
    float upper[3], fore[3];
    for (int k = 0; k < 3; ++k) {
        upper[k] = elbow[9 + k] - s[k];
        fore[k] = wrist[9 + k] - elbow[9 + k];
    }
    const float l1 = std::sqrt(Dot(upper, upper)), l2 = std::sqrt(Dot(fore, fore));
    if (!(l1 > 0.05f && l1 < 1.0f && l2 > 0.05f && l2 < 1.0f))
        return false;
    float reach[3] = {target[0] - s[0], target[1] - s[1], target[2] - s[2]};
    float distance = std::sqrt(Dot(reach, reach));
    if (!(distance > 1e-4f))
        return false;
    for (int k = 0; k < 3; ++k)
        reach[k] /= distance;
    const float nearest = std::fabs(l1 - l2) + 0.01f, farthest = l1 + l2 - 0.005f;
    distance = distance < nearest ? nearest : distance > farthest ? farthest : distance;
    for (int k = 0; k < 3; ++k)
        target[k] = s[k] + reach[k] * distance; // cible hors de portée : bras tendu vers elle
    const float along = (l1 * l1 - l2 * l2 + distance * distance) / (2.0f * distance);
    const float height = std::sqrt(l1 * l1 - along * along > 0.0f ? l1 * l1 - along * along : 0.0f);
    float pole[3] = {0.0f, -1.0f, -0.35f};
    if (otherShoulder) {
        float outward[3] = {s[0] - otherShoulder[0], s[1] - otherShoulder[1], s[2] - otherShoulder[2]};
        if (Normalize(outward))
            for (int k = 0; k < 3; ++k)
                pole[k] += 0.5f * outward[k];
    }
    const float onReach = Dot(pole, reach);
    for (int k = 0; k < 3; ++k)
        pole[k] -= onReach * reach[k];
    if (!Normalize(pole)) {
        pole[0] = 0.0f;
        pole[1] = 0.0f;
        pole[2] = -1.0f;
    }
    float newUpper[3], newFore[3];
    for (int k = 0; k < 3; ++k) {
        const float joint = s[k] + reach[k] * along + pole[k] * height;
        newUpper[k] = joint - s[k];
        newFore[k] = target[k] - joint;
    }
    if (!Normalize(upper) || !Normalize(newUpper) || !Normalize(newFore))
        return false;

    // Rotation du haut du bras, puis de l'avant-bras (déjà entraîné par la première)
    float r1[9], r2[9], carried[3];
    RotationBetween(upper, newUpper, r1);
    Rotate(r1, fore, carried);
    if (!Normalize(carried))
        return false;
    RotationBetween(carried, newFore, r2);

    float shoulderNew[9], elbowParentNew[9], elbowNew[9], handParentNew[9], handNew[9], scratch[9], axes[9];
    UnitAxes(shoulder, axes);
    RotateAxes(r1, axes, shoulderNew);
    UnitAxes(skeleton.Matrix(slots[4]), axes);
    RotateAxes(r1, axes, elbowParentNew);
    UnitAxes(elbow, axes);
    RotateAxes(r1, axes, scratch);
    RotateAxes(r2, scratch, elbowNew);
    UnitAxes(skeleton.Matrix(slots[5]), axes);
    RotateAxes(r1, axes, scratch);
    RotateAxes(r2, scratch, handParentNew);
    if (held) {
        for (int axis = 0; axis < 3; ++axis)
            for (int k = 0; k < 3; ++k)
                handNew[axis * 3 + k] = Dot(handWorld + axis * 3, rootOrient + k * 3);
    } else {
        // Pas d'arme mesurée : la main garde son attitude par rapport à l'avant-bras
        UnitAxes(wrist, axes);
        RotateAxes(r1, axes, scratch);
        RotateAxes(r2, scratch, handNew);
    }

    UnitAxes(skeleton.Matrix(slots[3]), axes);
    ToLocal(shoulderNew, axes, g_locals + static_cast<size_t>(slots[0]) * kFloatsPerBone);
    ToLocal(elbowNew, elbowParentNew, g_locals + static_cast<size_t>(slots[1]) * kFloatsPerBone);
    ToLocal(handNew, handParentNew, g_locals + static_cast<size_t>(slots[2]) * kFloatsPerBone);
    memcpy(wristTarget, target, 3 * sizeof(float));
    return true;
}

// Constate le sens des transformations locales en recalculant celle d'une main à partir des
// matrices du jeu
void FindConvention(const Skeleton& skeleton)
{
    // Sur les six os des bras : une seule orientation presque symétrique ne tromperait pas
    float direct = 0.0f, transposed = 0.0f;
    for (const Arm& arm : g_arms) {
        const int bones[3][2] = {{arm.shoulder, arm.shoulderParent}, {arm.elbow, arm.elbowParent}, {arm.hand, arm.handParent}};
        for (const auto& pair : bones) {
            const int child = skeleton.Slot(pair[0]), parent = skeleton.Slot(pair[1]);
            if (child < 0 || parent < 0)
                return;
            const float* game = g_locals + static_cast<size_t>(child) * kFloatsPerBone;
            float childAxes[9], parentAxes[9];
            UnitAxes(skeleton.Matrix(child), childAxes);
            UnitAxes(skeleton.Matrix(parent), parentAxes);
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) {
                    const float value = Dot(childAxes + i * 3, parentAxes + j * 3);
                    direct += std::fabs(value - game[i * 3 + j]);
                    transposed += std::fabs(value - game[j * 3 + i]);
                }
        }
    }
    g_convention = direct < 0.6f && direct < 0.5f * transposed ? 1 : transposed < 0.6f && transposed < 0.5f * direct ? 2 : -1;
    LOG_ENGINE("mains : transformations locales du jeu %s (ecarts %.3f et %.3f)",
               g_convention == 1 ? "comprises (os le long des axes du parent)"
               : g_convention == 2 ? "comprises (sens transpose)" : "INCOMPRISES, bras laisses a l'animation",
               direct, transposed);
}

// Écrit g_locals : les os périmés sont recalculés par le jeu, puis leurs matrices finales.
// L'échelle et la hauteur de la racine, que l'écriture réapplique, en sont d'abord retirées.
void WritePose(const Skeleton& skeleton, bool scaled, const float scale[3])
{
    g_locals[10] -= *reinterpret_cast<const float*>(skeleton.instance + kInstRootLift);
    for (int axis = 0; scaled && axis < 3; ++axis) {
        for (int k = 0; k < 3; ++k)
            g_locals[axis * 3 + k] /= scale[axis];
        g_locals[9 + axis] /= scale[axis];
    }
    g_setPose(skeleton.instance, g_locals, nullptr, 0);
    g_updatePose(skeleton.instance, nullptr, 0);
}

void MarkStale(const Skeleton& skeleton, const int* bones, int count)
{
    for (int i = 0; i < count; ++i) {
        const int slot = skeleton.Slot(bones[i]);
        if (slot > 0)
            *reinterpret_cast<uint32_t*>(skeleton.flags + static_cast<size_t>(slot) * kFlagSize) &= ~kFlagStale;
    }
}

// Tourne le buste (os « Spine » et tout ce qu'il porte) autour de la verticale pour que les
// épaules fassent face au regard. Le jeu oriente le corps selon la marche ou sa caméra ; en VR
// la tête regarde ailleurs, et des bras posés sur les manettes depuis des épaules mal tournées
// se tordent. g_locals doit contenir la pose relevée. true : pose écrite (à relever de nouveau).
bool TwistTorso(const Skeleton& skeleton, const float* rootOrient, bool scaled, const float scale[3])
{
    if (!(g_torsoMax > 0.0f) || g_spine < 0 || g_spineBoneCount <= 0)
        return false;
    const int spine = skeleton.Slot(g_spine), parent = skeleton.Slot(g_spineParent);
    const int left = skeleton.Slot(g_arms[0].shoulder), right = skeleton.Slot(g_arms[1].shoulder);
    if (spine <= 0 || parent < 0 || left < 0 || right < 0)
        return false;
    // Sens actuel des épaules et sens voulu (la droite de la tête), à plat dans le repère du
    // personnage : l'avant est à 90 degrés de la droite
    const float* l = skeleton.Matrix(left) + 9;
    const float* r = skeleton.Matrix(right) + 9;
    const float shoulders[2] = {r[0] - l[0], r[2] - l[2]};
    const float head[2] = {Dot(g_headRight, rootOrient), Dot(g_headRight, rootOrient + 6)};
    if (shoulders[0] * shoulders[0] + shoulders[1] * shoulders[1] < 0.01f * 0.01f ||
        head[0] * head[0] + head[1] * head[1] < 0.3f * 0.3f)
        return false; // tête penchée sur le côté à plus de 70 degrés : sens du regard incertain
    float turn = std::atan2(-head[1], head[0]) - std::atan2(-shoulders[1], shoulders[0]);
    const float kPi = 3.14159265f;
    while (turn > kPi)
        turn -= 2.0f * kPi;
    while (turn < -kPi)
        turn += 2.0f * kPi;
    turn = turn > g_torsoMax ? g_torsoMax : turn < -g_torsoMax ? -g_torsoMax : turn;
    if (std::fabs(turn) < 0.01f)
        return false;
    // Rotation autour de la verticale du personnage (x à droite, y en haut, z devant) : l'avant
    // tourne vers la droite pour un angle positif
    const float c = std::cos(turn), sn = std::sin(turn);
    const float rotation[9] = {c, 0.0f, sn, 0.0f, 1.0f, 0.0f, -sn, 0.0f, c};
    float axes[9], turned[9], parentAxes[9];
    UnitAxes(skeleton.Matrix(spine), axes);
    RotateAxes(rotation, axes, turned);
    UnitAxes(skeleton.Matrix(parent), parentAxes);
    ToLocal(turned, parentAxes, g_locals + static_cast<size_t>(spine) * kFloatsPerBone);
    MarkStale(skeleton, g_spineBones, g_spineBoneCount);
    WritePose(skeleton, scaled, scale);
    ++g_torsoCount;
    if (std::fabs(turn) > g_torsoWorst)
        g_torsoWorst = std::fabs(turn);
    if (!g_torsoShown) {
        g_torsoShown = true;
        LOG_ENGINE("mains : buste tourne vers le regard (%.0f degres a cette image, %d os entraines)",
                   static_cast<double>(turn * 57.29578f), g_spineBoneCount);
    }
    return true;
}

// Annonce une fois la raison pour laquelle les bras restent à l'animation (0 : ils sont posés)
void Skip(int reason, const char* text)
{
    if (g_skipShown != reason) {
        g_skipShown = reason;
        if (reason)
            LOG_ENGINE("mains : bras laisses a l'animation, %s", text);
    }
}

void PoseArms(uint8_t* character)
{
    g_posed[0] = g_posed[1] = false;
    if (!g_active || g_faulted)
        return;
    Skeleton skeleton;
    skeleton.instance = static_cast<uint8_t*>(player::Instance());
    if (!skeleton.instance)
        return Skip(1, "pas de personnage anime");
    skeleton.matrices = *reinterpret_cast<float**>(skeleton.instance + kInstMatrices);
    skeleton.flags = *reinterpret_cast<uint8_t**>(skeleton.instance + kInstFlags);
    skeleton.map = *reinterpret_cast<const uint8_t* const*>(skeleton.instance + kInstMap);
    const uintptr_t rig = *reinterpret_cast<const uintptr_t*>(skeleton.instance + kInstRig);
    if (!skeleton.matrices || !skeleton.flags || !skeleton.map || !rig)
        return Skip(2, "squelette incomplet");
    skeleton.count = *reinterpret_cast<const int*>(skeleton.map + kMapCount);
    if (skeleton.count < 2 || skeleton.count > kMaxBones)
        return Skip(3, "nombre d'os inattendu");
    if (rig != g_rig)
        ResolveRig(rig);
    if (!g_rigOk || g_convention < 0)
        return;
    // Échelle du personnage (corpulence, taille) : retirée de la racine avant d'écrire la pose
    const bool scaled = skeleton.instance[kInstScaled] != 0;
    float scale[3]{1.0f, 1.0f, 1.0f};
    if (scaled) {
        memcpy(scale, skeleton.instance + kInstScale, sizeof(scale));
        for (float factor : scale)
            if (!(factor > 0.2f && factor < 5.0f))
                return Skip(4, "echelle du personnage inattendue");
        // La racine relevée porte-t-elle bien cette échelle ? Sinon on ne saurait pas la retirer.
        const float* rootAxes = skeleton.Matrix(0);
        for (int axis = 0; axis < 3; ++axis) {
            const float length = std::sqrt(Dot(rootAxes + axis * 3, rootAxes + axis * 3));
            if (std::fabs(length - scale[axis]) > 0.03f)
                return Skip(7, "echelle du personnage absente de la racine du squelette");
        }
    }

    const float* root = reinterpret_cast<const float*>(character + kRootPos);
    const float* rootOrient = reinterpret_cast<const float*>(character + kRootOrient);
    float eye[3];
    for (int k = 0; k < 3; ++k)
        eye[k] = root[k] + g_eyeFromRoot[k];

    g_getPose(skeleton.instance, g_locals);
    if (g_convention == 0) {
        FindConvention(skeleton);
        if (g_convention < 0)
            return;
        if (scaled)
            LOG_ENGINE("mains : personnage a l'echelle (%.3f, %.3f, %.3f)", scale[0], scale[1], scale[2]);
    }
    // Buste face au regard d'abord : les épaules bougent, les bras sont posés ensuite
    const bool twisted = TwistTorso(skeleton, rootOrient, scaled, scale);

    // Manettes suivies ? Sinon rien à poser, et pas la peine de relever la pose
    float grip[2][3], origin[2][3], orient[2][9];
    bool tracked[2];
    for (int hand = 0; hand < 2; ++hand) {
        tracked[hand] = g_frameFn(hand, grip[hand], origin[hand], orient[hand]);
        for (int k = 0; tracked[hand] && k < 3; ++k) {
            grip[hand][k] += eye[k];
            origin[hand][k] += eye[k];
        }
    }
    if (!tracked[0] && !tracked[1])
        return Skip(5, "manettes non suivies");

    // Ce que tient chaque main. Arme à feu (la gauche n'en a une que tenue en double) : sa prise
    // est mesurée, la mesure ne dépend pas de la pose puisque l'arme est fixe dans la main. Main
    // vide ou arme blanche : main tenue comme la manette.
    const int meleeAttack = player::HeldMeleeAttack();
    const bool firearm[2] = {meleeAttack == 0xFF && player::DualWielding(), meleeAttack == 0xFF};
    const Grip* held[2]{};
    float aim[2][9];
    for (int hand = 0; hand < 2; ++hand) {
        if (!tracked[hand])
            continue;
        memcpy(aim[hand], orient[hand], sizeof(aim[hand]));
        if (firearm[hand]) {
            MeasureGrip(character, skeleton, hand);
            held[hand] = g_grip[hand].valid ? &g_grip[hand] : nullptr;
            continue;
        }
        held[hand] = &g_restGrip[hand];
        // Arme blanche (main droite) : inclinée vers l'avant de l'angle réglé, autour de la droite
        // de la manette (la lame, à peu près le haut de la main, se couche vers où l'on pointe)
        if (hand == 1 && meleeAttack >= 0 && g_meleeTilt != 0.0f) {
            const float c = std::cos(g_meleeTilt), s = std::sin(g_meleeTilt);
            for (int k = 0; k < 3; ++k) {
                const float up = orient[hand][3 + k], forward = orient[hand][6 + k];
                aim[hand][3 + k] = up * c + forward * s;
                aim[hand][6 + k] = forward * c - up * s;
            }
        }
    }
    if (!g_restShown && (held[0] == &g_restGrip[0] || held[1] == &g_restGrip[1])) {
        g_restShown = true;
        LOG_ENGINE("mains : main vide ou arme blanche, la main prend l'attitude de la manette (prise d'un pistolet)");
    }

    if (twisted)
        g_getPose(skeleton.instance, g_locals);
    const int shoulderSlots[2] = {skeleton.Slot(g_arms[0].shoulder), skeleton.Slot(g_arms[1].shoulder)};
    float targets[2][3];
    for (int hand = 0; hand < 2; ++hand) {
        if (!tracked[hand])
            continue;
        const int other = shoulderSlots[1 - hand];
        g_posed[hand] = PoseArm(skeleton, hand, other >= 0 ? skeleton.Matrix(other) + 9 : nullptr, grip[hand],
                                origin[hand], aim[hand], root, rootOrient, held[hand], firearm[hand], targets[hand]);
    }
    if (!g_posed[0] && !g_posed[1])
        return Skip(6, "cible ou os des bras inutilisables");
    Skip(0, "");
    // Les os des bras posés sont périmés : le jeu les recalcule d'après nos transformations
    // locales, puis leurs matrices finales
    for (int hand = 0; hand < 2; ++hand)
        if (g_posed[hand])
            MarkStale(skeleton, g_armBones[hand], g_armBoneCount[hand]);
    WritePose(skeleton, scaled, scale);

    // Contrôle : le poignet est-il arrivé ?
    for (int hand = 0; hand < 2; ++hand) {
        if (!g_posed[hand])
            continue;
        ++g_posedCount[hand];
        const int slot = skeleton.Slot(g_arms[hand].hand);
        const float* wrist = skeleton.Matrix(slot) + 9;
        const float delta[3] = {wrist[0] - targets[hand][0], wrist[1] - targets[hand][1], wrist[2] - targets[hand][2]};
        const float error = std::sqrt(Dot(delta, delta));
        if (error > g_worstError)
            g_worstError = error;
        // Poing dans le monde : au bout de l'avant-bras, un peu après le poignet
        const int elbowSlot = skeleton.Slot(g_arms[hand].elbow);
        if (elbowSlot >= 0) {
            const float* elbow = skeleton.Matrix(elbowSlot) + 9;
            float along[3] = {wrist[0] - elbow[0], wrist[1] - elbow[1], wrist[2] - elbow[2]};
            if (Normalize(along)) {
                for (int k = 0; k < 3; ++k) {
                    const float local[3] = {wrist[0] + kFistAhead * along[0], wrist[1] + kFistAhead * along[1],
                                            wrist[2] + kFistAhead * along[2]};
                    g_fist[hand][k] = root[k] + local[0] * rootOrient[k] + local[1] * rootOrient[3 + k] +
                                      local[2] * rootOrient[6 + k];
                }
                g_fistTick[hand] = GetTickCount64();
            }
        }
        if (!g_shown[hand]) {
            g_shown[hand] = true;
            LOG_ENGINE("mains : bras %s pose sur la manette (poignet a %.3f m de sa cible apres la pose%s)",
                       hand ? "droit" : "gauche", error,
                       firearm[hand] ? (held[hand] ? ", arme alignee sur le rayon de visee" : ", arme pas encore mesuree")
                                     : ", main tenue comme la manette");
        }
    }
}

void PoseArmsGuarded(uint8_t* character)
{
#if defined(_MSC_VER) && !defined(__clang__)
    __try {
        PoseArms(character);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_posed[0] = g_posed[1] = false;
        if (!g_faulted) {
            g_faulted = true;
            LOG_ENGINE("mains : ERREUR pendant la pose des bras, bras laisses a l'animation pour cette partie");
        }
    }
#else
    PoseArms(character);
#endif
}

bool __stdcall HkLimbTarget(uint8_t* limb, uint8_t* character, float* target)
{
    if (limb && character && player::IsLocal(character)) {
        // Premier membre de la passe d'IK : c'est le moment de poser les bras
        if (limb == character + kLimbTable)
            PoseArmsGuarded(character);
        // Un bras qu'on vient de poser n'a plus de cible d'IK pour le jeu
        const unsigned index = limb[kLimbIndex];
        if (index <= 1 && g_posed[index])
            return false;
    }
    return g_limbTarget(limb, character, target);
}

} // namespace

bool Install(FrameFn frame)
{
    auto* site = static_cast<uint8_t*>(pattern::Find("cible d'IK d'un membre", kLimbTargetSig));
    g_getPose = reinterpret_cast<GetPoseFn>(pattern::Find("releve de la pose", kGetPoseSig));
    g_setPose = reinterpret_cast<SetPoseFn>(pattern::Find("ecriture de la pose", kSetPoseSig));
    g_updatePose = reinterpret_cast<UpdatePoseFn>(pattern::Find("calcul du squelette", kUpdatePoseSig));
    g_muzzle = pattern::Find("bouche du canon", kMuzzleSig);
    if (!frame || !site || !g_getPose || !g_setPose || !g_updatePose || !g_muzzle) {
        LOG_ENGINE("mains : fonctions de pose introuvables, les bras resteront ceux de l'animation");
        return false;
    }
    g_frameFn = frame;
    // Prises de repos : repère (droite, haut, avant) du canon dans les axes de la main
    for (int hand = 0; hand < 2; ++hand) {
        float forward[3], up[3], right[3];
        memcpy(forward, kRestForward[hand], sizeof(forward));
        Normalize(forward);
        Cross(kRestUp[hand], forward, right);
        Normalize(right);
        Cross(forward, right, up);
        Grip& grip = g_restGrip[hand];
        memcpy(grip.k, right, sizeof(right));
        memcpy(grip.k + 3, up, sizeof(up));
        memcpy(grip.k + 6, forward, sizeof(forward));
        grip.offset[0] = grip.offset[1] = grip.offset[2] = 0.0f;
        grip.valid = true;
    }
    MH_STATUS st =
        MH_CreateHook(site, reinterpret_cast<void*>(&HkLimbTarget), reinterpret_cast<void**>(&g_limbTarget));
    if (st == MH_OK)
        st = MH_EnableHook(site);
    if (st != MH_OK) {
        LOG_ENGINE("mains : ECHEC MinHook sur la cible d'IK : %s", MH_StatusToString(st));
        MH_RemoveHook(site);
        return false;
    }
    g_target = site;
    LOG_ENGINE("mains : passe d'IK detournee, bras et armes du joueur poses sur les manettes");
    return true;
}

void Remove()
{
    g_active = false;
    if (g_target) {
        MH_DisableHook(g_target);
        MH_RemoveHook(g_target);
        g_target = nullptr;
    }
}

void SetFrame(bool active, const float eyeFromRoot[3], const float headRight[3], float torsoMaxDegrees)
{
    g_active = active && g_target != nullptr;
    if (!g_active) {
        g_shown[0] = g_shown[1] = false; // annoncé de nouveau à la reprise
        return;
    }
    memcpy(g_eyeFromRoot, eyeFromRoot, sizeof(g_eyeFromRoot));
    memcpy(g_headRight, headRight, sizeof(g_headRight));
    g_torsoMax = torsoMaxDegrees > 0.0f ? (torsoMaxDegrees > 180.0f ? 180.0f : torsoMaxDegrees) * 0.01745329f : 0.0f;
}

void SetMeleeTilt(float degrees)
{
    g_meleeTilt = degrees >= -90.0f && degrees <= 90.0f ? degrees * 0.01745329f : 0.0f;
}

bool Fist(int hand, float world[3])
{
    if (hand < 0 || hand > 1 || !g_fistTick[hand] || GetTickCount64() - g_fistTick[hand] > kFistMaxAgeMs)
        return false;
    memcpy(world, g_fist[hand], sizeof(g_fist[hand]));
    return true;
}

void TakeStats(unsigned posed[2], float* worstError, unsigned* torso, float* torsoWorstDegrees)
{
    posed[0] = g_posedCount[0];
    posed[1] = g_posedCount[1];
    *worstError = g_worstError;
    *torso = g_torsoCount;
    *torsoWorstDegrees = g_torsoWorst * 57.29578f;
    g_posedCount[0] = g_posedCount[1] = 0;
    g_worstError = 0.0f;
    g_torsoCount = 0;
    g_torsoWorst = 0.0f;
}

} // namespace engine::hands
