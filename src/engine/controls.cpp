#include "engine/controls.h"
#include "engine/player.h"
#include "input/actions.h"
#include "util/log.h"
#include "util/pattern.h"

#include <MinHook.h>
#include <windows.h>

#include <cstdint>
#include <cstring>

namespace engine::controls {
namespace {

// Lecture des entrées, une fois par image de simulation, sans argument : push ecx ;
// cmp byte [...],0 ; je ... ; call ... ; cmp byte [...],0 ; jne ... ; cmp byte [...],0 ;
// jne ... ; call <clavier et souris>
constexpr char kInputUpdateSig[] =
    "51 80 3D ?? ?? ?? ?? 00 0F 84 A3 00 00 00 E8 ?? ?? ?? ?? 80 3D ?? ?? ?? ?? 00 75 10 80 3D ?? ?? ?? ?? 00 75 07 E8";
// Dans cette fonction, la remise à zéro des cases : xorps xmm0,xmm0 ; mov eax,<cases> ;
// fld [eax] ; movss [eax],xmm0 ; fstp [eax+4] ; add eax,24h ; cmp eax,<fin des cases> ; jl
constexpr char kSlotsSig[] = "0F 57 C0 B8 ?? ?? ?? ?? D9 00 F3 0F 11 00 D9 58 04 83 C0 24 3D ?? ?? ?? ?? 7C ED";
constexpr int kSlotsOffset = 4;
constexpr int kSlotsEndOffset = 21;
constexpr size_t kSlotSize = 0x24;
// Remise à zéro de la table des commandes : mov [eax+<table>],ecx ; mov [eax+<table>+4],ecx ;
// mov [eax+<table>+8],ecx ; mov [eax+<table>+0Ch],edx ; add eax,10h ; cmp eax,840h ; jb
constexpr char kCommandTableSig[] =
    "89 88 ?? ?? ?? ?? 89 88 ?? ?? ?? ?? 89 88 ?? ?? ?? ?? 89 90 ?? ?? ?? ?? 83 C0 10 3D 40 08 00 00 72 DE";
constexpr int kCommandTableOffset = 2;
constexpr int kCommandCount = 0x840 / 0x10;
constexpr size_t kCommandSize = 0x10;
// Fonction de tir des personnages (0x009C0500), avec une arme dans chaque main : trois sauts
// modifiés en mémoire pour que la main qui tire soit toujours celle que demande l'argument
// « autre main » (que engine/aim remplace par la main de la gâchette).
struct Patch {
    const char* name;
    const char* signature;
    int offset;          // du début de la signature à l'instruction modifiée
    int size;
    uint8_t expected[6]; // ce qu'on doit y trouver (0xFF = n'importe quoi)
    uint8_t replacement[6];
    uint8_t* site;       // nullptr tant que la modification n'est pas posée
    uint8_t saved[6];
};
Patch g_patches[] = {
    // L'arme de gauche ne tirait que si le chargeur contient un nombre impair de balles :
    // cmp [edi+0CCh],0 ; jne ... ; cmp byte [ebp+18h],0 ; je ... ; mov eax,[esp+2Ch] ;
    // test byte [eax],1 ; je <pas de tir> -> saut supprimé
    {"alternance du chargeur", "83 BF CC 00 00 00 00 75 ?? 80 7D 18 00 74 ?? 8B 44 24 ?? F6 00 01 0F 84 ?? ?? ?? ??", 22, 6,
     {0x0F, 0x84, 0xFF, 0xFF, 0xFF, 0xFF}, {0x90, 0x90, 0x90, 0x90, 0x90, 0x90}, nullptr, {}},
    // Détente autre que « coup par coup » (rafale, rapide : Kobra, mitraillettes) : les deux
    // armes partaient à chaque coup. cmp [esi+0CCh],0 ; je <main demandée> ; ... -> jmp
    {"deux armes ensemble", "83 BE CC 00 00 00 00 74 0E 8B 44 24 ?? 83 38 01 0F 9F 44 24 ?? EB ?? 80 7D 18 00", 7, 1,
     {0x74}, {0xEB}, nullptr, {}},
    // Même cas, drapeaux « cette main a tiré » : cmp [edx+0CCh],0 ; je <main demandée> ;
    // or byte [eax+0Ch],6 -> jmp
    {"drapeaux des deux armes", "83 BA CC 00 00 00 00 74 06 80 48 0C 06 EB ?? 80 7D 18 00 75 ?? 80 48 0C 02", 7, 1,
     {0x74}, {0xEB}, nullptr, {}},
};

// Numéros des commandes (table des noms du jeu : PICKUP_RELOAD, ATTACK_PRIMARY, ATTACK_SECONDARY ;
// en véhicule FIREARM et, juste après, le tir secondaire)
enum : int {
    kFastSwitch = 1,
    kReload = 2,
    kAttackPrimary = 6,
    kAttackSecondary = 7,
    kTauntOne = 0x0E,
    kTauntTwo = 0x0F,
    kGrab = 0x12,
    kVehicleFire = 0x1E,
    kVehicleFireSecondary = 0x1F,
    kRecruit = 0x2E,
    kCancelActivity = 0x2F,
    kMapMenu = 0x31,
};

using InputUpdateFn = void(__cdecl*)();
InputUpdateFn g_inputUpdate = nullptr;
void* g_target = nullptr;
float* g_slots = nullptr;
int g_slotCount = 0;
const int* g_commands = nullptr;

// Thread de simulation
// Une gâchette vue par la simulation. Le casque tourne bien plus vite qu'elle : un appui bref
// tient au moins deux images de simulation, et une gâchette relâchée puis reprise entre deux
// images retombe une image, pour que le jeu voie deux appuis.
struct Trigger {
    bool down = false;   // enfoncée, pour la simulation
    bool edge = false;   // vient de l'être
    unsigned pending = 0;
    int hold = 0;
};
Trigger g_left, g_right;
int g_aimHand = 1;
int g_fireWanted = -1; // deux armes : main de la seule gâchette tenue (ou de celle qui vient d'être pressée), -1 sinon
int g_fireTurn = 1;    // deux gâchettes tenues : main du prochain coup
bool g_announced[kCommandCount]{};
bool g_dualShown = false;
bool g_handShown[2]{};

void Sample(input::actions::Action action, Trigger& trigger)
{
    // L'état d'abord, les appuis ensuite (voir input/actions.h)
    const bool level = input::actions::Get(action);
    trigger.pending += input::actions::TakePresses(action);
    bool down = false;
    if (!trigger.down) {
        down = level || trigger.pending > 0;
        trigger.pending = 0;
        trigger.hold = down ? 1 : 0;
    } else if (trigger.hold > 0) {
        --trigger.hold;
        down = true;
    } else if (trigger.pending > 0) {
        trigger.pending = 1; // reprise à l'image suivante
    } else {
        down = level;
    }
    trigger.edge = down && !trigger.down;
    trigger.down = down;
}

// Pose la commande à 1 pour cette image. Sans effet si le jeu ne lui a pas donné de case.
void Press(int command)
{
    const int slot = g_commands[command * (kCommandSize / sizeof(int))];
    if (slot < 0 || slot >= g_slotCount) {
        if (!g_announced[command]) {
            g_announced[command] = true;
            LOG_ENGINE("commandes : la commande %d n'a pas de case d'entree dans le jeu (%d), elle reste sans effet", command, slot);
        }
        return;
    }
    float* value = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(g_slots) + static_cast<size_t>(slot) * kSlotSize);
    *value = 1.0f;
    if (!g_announced[command]) {
        g_announced[command] = true;
        LOG_ENGINE("commandes : premiere commande %d posee dans le jeu (case %d des entrees)", command, slot);
    }
}

void Apply()
{
    namespace actions = input::actions;
    Sample(actions::kTriggerLeft, g_left);
    Sample(actions::kTriggerRight, g_right);
    const bool left = g_left.down, right = g_right.down;
    const bool vehicle = player::InVehicle();
    const bool dual = player::DualWielding();
    actions::SetDualWield(dual);
    if (dual != g_dualShown) {
        LOG_ENGINE("commandes : %s", dual ? "une arme dans chaque main, chaque gachette tire avec la sienne"
                                           : "une seule arme, la gachette gauche fait l'attaque secondaire");
        g_dualShown = dual;
    }

    // Une arme dans chaque main, à pied : tant que l'attaque principale est tenue, le jeu
    // appelle le tir du joueur à chaque image (chaque arme a sa cadence). engine/aim, qui
    // détourne ce tir, demande ici quelle main doit tirer : celle de la seule gâchette tenue ;
    // les deux tenues, chacune à son tour, en commençant par celle qui vient d'être pressée.
    const bool twoGuns = dual && !vehicle;
    bool primary = false;
    g_fireWanted = -1;
    if (twoGuns) {
        primary = left || right;
        if (g_left.edge != g_right.edge)
            g_fireWanted = g_left.edge ? 0 : 1;
        else if (left != right)
            g_fireWanted = left ? 0 : 1;
        if (g_fireWanted >= 0) {
            g_aimHand = g_fireWanted;
            if (!g_handShown[g_fireWanted]) {
                g_handShown[g_fireWanted] = true;
                LOG_ENGINE("commandes : premier tir demande a la main %s", g_fireWanted ? "droite" : "gauche");
            }
        }
    } else {
        g_aimHand = 1;
        primary = right || (left && dual);
    }

    // À pied et en véhicule, le jeu n'écoute pas les mêmes commandes
    if (primary)
        Press(vehicle ? kVehicleFire : kAttackPrimary);
    if (left && !dual)
        Press(vehicle ? kVehicleFireSecondary : kAttackSecondary);
    // Les autres commandes à pied : tenues tant que le bouton l'est. Un appui plus bref qu'une
    // image de simulation est quand même vu une fois.
    struct Simple {
        actions::Action action;
        int command;
        bool footOnly;
    };
    static const Simple kSimple[] = {
        {actions::kReload, kReload, false},        {actions::kGrab, kGrab, true},
        {actions::kRecruit, kRecruit, true},       {actions::kTauntOne, kTauntOne, true},
        {actions::kTauntTwo, kTauntTwo, true},     {actions::kCancel, kCancelActivity, false},
        {actions::kMap, kMapMenu, false},
    };
    for (const Simple& simple : kSimple) {
        const bool down = actions::Get(simple.action);
        const bool tapped = actions::TakePresses(simple.action) > 0;
        if ((down || tapped) && !(simple.footOnly && vehicle))
            Press(simple.command);
    }
}

bool WriteCode(uint8_t* at, const uint8_t* bytes, size_t size)
{
    DWORD protection = 0;
    if (!VirtualProtect(at, size, PAGE_EXECUTE_READWRITE, &protection))
        return false;
    memcpy(at, bytes, size);
    VirtualProtect(at, size, protection, &protection);
    FlushInstructionCache(GetCurrentProcess(), at, size);
    return true;
}

// Réglages de pilotage du jeu (un octet de drapeaux) : bit 0 = s'accroupir par bascule. Le bit
// mis à 0, le jeu accroupit le personnage tant que la commande est tenue et le relève quand
// elle est relâchée (c'est son propre mode « maintenu », lu par le pilotage à pied).
constexpr char kCrouchOptionSig[] =
    "F6 05 ?? ?? ?? ?? 01 0F 84 ?? ?? ?? ?? F7 45 00 00 80 00 00 0F 84 ?? ?? ?? ?? B9 11 00 00 00";
uint8_t* g_options = nullptr;
bool g_crouchHold = false;    // demandé par [Controls] crouch_hold
bool g_crouchChanged = false; // nous avons retiré la bascule : à rendre

void ApplyCrouchOption()
{
    if (!g_options)
        return;
    if (g_crouchHold) {
        if (*g_options & 1) {
            *g_options &= static_cast<uint8_t>(~1);
            if (!g_crouchChanged)
                LOG_ENGINE("commandes : accroupissement maintenu (bascule du jeu retiree en memoire)");
            g_crouchChanged = true;
        }
    } else if (g_crouchChanged) {
        *g_options |= 1;
        g_crouchChanged = false;
        LOG_ENGINE("commandes : accroupissement par bascule, comme dans le jeu");
    }
}

void __cdecl HkInputUpdate()
{
    g_inputUpdate();
    ApplyCrouchOption();
#if defined(_MSC_VER) && !defined(__clang__)
    __try {
        Apply();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
#else
    Apply();
#endif
}

} // namespace

bool Install()
{
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto absolute = [](const uint8_t* operand) {
        uint32_t address = 0;
        memcpy(&address, operand, sizeof(address));
        return static_cast<uintptr_t>(address);
    };
    auto* update = static_cast<uint8_t*>(pattern::Find("lecture des entrees", kInputUpdateSig));
    auto* table = static_cast<uint8_t*>(pattern::Find("table des commandes", kCommandTableSig));
    uint8_t* reset = update ? pattern::FindIn(update, 0xC0, kSlotsSig) : nullptr;
    if (!update || !table || !reset) {
        LOG_ENGINE("commandes : lecture des entrees ou tables introuvables, les gachettes passent par la manette simulee");
        return false;
    }
    const uintptr_t slots = absolute(reset + kSlotsOffset), end = absolute(reset + kSlotsEndOffset);
    if (end <= slots || (end - slots) % kSlotSize != 0 || (end - slots) / kSlotSize > 256) {
        LOG_ENGINE("commandes : tableau des entrees inattendu (%p a %p)", reinterpret_cast<void*>(slots),
                   reinterpret_cast<void*>(end));
        return false;
    }
    g_slots = reinterpret_cast<float*>(slots);
    g_slotCount = static_cast<int>((end - slots) / kSlotSize);
    g_commands = reinterpret_cast<const int*>(absolute(table + kCommandTableOffset));

    MH_STATUS st =
        MH_CreateHook(update, reinterpret_cast<void*>(&HkInputUpdate), reinterpret_cast<void**>(&g_inputUpdate));
    if (st == MH_OK)
        st = MH_EnableHook(update);
    if (st != MH_OK) {
        LOG_ENGINE("commandes : ECHEC MinHook sur la lecture des entrees : %s", MH_StatusToString(st));
        MH_RemoveHook(update);
        return false;
    }
    g_target = update;
    input::actions::SetDirect(true);
    // Deux armes : la main qui tire devient celle que demande la gâchette
    int applied = 0;
    for (Patch& patch : g_patches) {
        auto* found = static_cast<uint8_t*>(pattern::Find(patch.name, patch.signature));
        if (!found)
            continue;
        uint8_t* site = found + patch.offset;
        bool expected = true;
        for (int i = 0; i < patch.size; ++i)
            expected = expected && (patch.expected[i] == 0xFF || patch.expected[i] == site[i]);
        if (!expected) {
            LOG_ENGINE("commandes : %s : code inattendu en exe+0x%lX, laisse tel quel", patch.name,
                       static_cast<unsigned long>(reinterpret_cast<uintptr_t>(site) - base));
            continue;
        }
        memcpy(patch.saved, site, static_cast<size_t>(patch.size));
        if (WriteCode(site, patch.replacement, static_cast<size_t>(patch.size))) {
            patch.site = site;
            ++applied;
        }
    }
    if (auto* option = static_cast<uint8_t*>(pattern::Find("reglages de pilotage du jeu", kCrouchOptionSig)))
        memcpy(&g_options, option + 2, sizeof(g_options));
    const int wanted = static_cast<int>(sizeof(g_patches) / sizeof(g_patches[0]));
    LOG_ENGINE("commandes : tir a deux armes, %d modification(s) sur %d posee(s)%s", applied, wanted,
               applied == wanted ? " : chaque gachette tire avec son arme, quelle que soit l'arme"
                                 : " : certaines armes tenues en double tireront encore ensemble ou en alternance");
    LOG_ENGINE("commandes : lecture des entrees detournee (exe+0x%lX), %d cases en exe+0x%lX, table des commandes "
               "exe+0x%lX : les gachettes sont posees directement dans le jeu",
               static_cast<unsigned long>(reinterpret_cast<uintptr_t>(update) - base), g_slotCount,
               static_cast<unsigned long>(slots - base),
               static_cast<unsigned long>(reinterpret_cast<uintptr_t>(g_commands) - base));
    return true;
}

void SetCrouchHold(bool hold)
{
    g_crouchHold = hold;
}

void Remove()
{
    input::actions::SetDirect(false);
    g_crouchHold = false;
    ApplyCrouchOption();
    for (Patch& patch : g_patches) {
        if (patch.site)
            WriteCode(patch.site, patch.saved, static_cast<size_t>(patch.size));
        patch.site = nullptr;
    }
    if (g_target) {
        MH_DisableHook(g_target);
        MH_RemoveHook(g_target);
        g_target = nullptr;
    }
}

int SlotOf(int command)
{
    if (!g_commands || command < 0 || command >= kCommandCount)
        return -1;
    const int slot = g_commands[command * (kCommandSize / sizeof(int))];
    return slot >= 0 && slot < g_slotCount ? slot : -1;
}

int AimHand()
{
    return g_aimHand;
}

int FireHand()
{
    if (g_fireWanted >= 0) {
        g_fireTurn = 1 - g_fireWanted; // l'autre main ensuite, si sa gâchette vient s'ajouter
        return g_fireWanted;
    }
    const int hand = g_fireTurn;
    g_fireTurn = 1 - g_fireTurn;
    return hand;
}

} // namespace engine::controls
