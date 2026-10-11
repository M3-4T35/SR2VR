#include "engine/prompts.h"
#include "engine/controls.h"
#include "input/actions.h"
#include "util/config.h"
#include "util/log.h"
#include "util/pattern.h"

#include <MinHook.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <cwchar>

namespace engine::prompts {
namespace {

// Dessin des touches attribuées à une liste d'entrées (0x00C11C00), cdecl (entrées, nombre) :
// sub esp,474h ; mov eax,[cookie] ; xor eax,esp ; mov [esp+470h],eax ; mov eax,[esp+478h] ;
// mov ecx,[esp+47Ch] ; push ebp ; push esi ; mov esi,[curseur] ; cmp esi,251Ch
constexpr char kKeyTextSig[] = "81 EC 74 04 00 00 A1 ?? ?? ?? ?? 33 C4 89 84 24 70 04 00 00 8B 84 24 78 04 00 00 8B 8C 24 7C 04 "
                               "00 00 55 56 8B 35 ?? ?? ?? ?? 81 FE 1C 25 00 00";
// Dans cette fonction : mov edi,[eax*8+<touches>] (table des touches : 8 octets par entrée,
// code de balayage en +0, 0x100 / 0x101 / 0x102 pour les boutons de la souris, -1 sans touche)
constexpr int kBindingsInsn = 0xD3;
constexpr int kSlotCount = 0x57;

// Numéros des commandes du jeu (table des noms)
enum : int {
    kCmdWeaponMenu = 0x00,
    kCmdReload = 0x02,
    kCmdAttackPrimary = 0x06,
    kCmdAttackSecondary = 0x07,
    kCmdJump = 0x08,
    kCmdSprint = 0x0C,
    kCmdTauntOne = 0x0E,
    kCmdTauntTwo = 0x0F,
    kCmdCrouch = 0x11,
    kCmdGrab = 0x12,
    kCmdVehicleWeaponMenu = 0x15,
    kCmdVehicleFire = 0x1E,
    kCmdVehicleFireSecondary = 0x1F,
    kCmdAction = 0x2D,
    kCmdRecruit = 0x2E,
    kCmdCancel = 0x2F,
    kCmdPause = 0x30,
    kCmdMap = 0x31,
    kCmdMenuSelect = 0x48,
    kCmdMenuAbort = 0x49,
};

enum Control {
    kNone,
    kA, kB, kX, kY, kAHeld, kBHeld,
    kTriggerLeft, kTriggerRight, kGripLeft, kGripRight,
    kStickLeft, kStickLeftUp, kStickLeftDown, kStickLeftLeft, kStickLeftRight,
    kStickRight, kStickRightUp, kStickRightDown, kClickLeft, kClickRight,
    kBumperLeft, kBumperRight, kDpadUp, kDpadDown, kDpadLeft, kDpadRight,
    kMenu, kOption, kView,
    kControlCount
};

struct Label {
    const wchar_t* fr;
    const wchar_t* en;
    const char* log;
};
const Label kLabels[kControlCount] = {
    {L"", L"", "aucun"},
    {L"a", L"a", "A"},
    {L"b", L"b", "B"},
    {L"x", L"x", "X"},
    {L"y", L"y", "Y"},
    {L"a", L"a", "A tenu"},
    {L"b", L"b", "B tenu"},
    {L"Gâchette gauche", L"Left trigger", "gachette gauche"},
    {L"Gâchette droite", L"Right trigger", "gachette droite"},
    {L"Poignée gauche", L"Left grip", "poignee gauche"},
    {L"Poignée droite", L"Right grip", "poignee droite"},
    {L"Stick gauche", L"Left stick", "stick gauche"},
    {L"Stick gauche haut", L"Left stick up", "stick gauche haut"},
    {L"Stick gauche bas", L"Left stick down", "stick gauche bas"},
    {L"Stick gauche à gauche", L"Left stick left", "stick gauche a gauche"},
    {L"Stick gauche à droite", L"Left stick right", "stick gauche a droite"},
    {L"Stick droit", L"Right stick", "stick droit"},
    {L"Stick droit haut", L"Right stick up", "stick droit haut"},
    {L"Stick droit bas", L"Right stick down", "stick droit bas"},
    {L"Clic du stick gauche", L"Left stick click", "clic du stick gauche"},
    {L"Clic du stick droit", L"Right stick click", "clic du stick droit"},
    {L"Bouton haut gauche", L"Left bumper", "bouton haut gauche"},
    {L"Bouton haut droit", L"Right bumper", "bouton haut droit"},
    {L"Croix haut", L"D-pad up", "croix haut"},
    {L"Croix bas", L"D-pad down", "croix bas"},
    {L"Croix gauche", L"D-pad left", "croix gauche"},
    {L"Croix droite", L"D-pad right", "croix droite"},
    {L"Menu", L"Menu", "menu"},
    {L"Option", L"Option", "option"},
    {L"View", L"View", "view"},
};

// Bouton de chaque commande dans les deux dispositions (voir xr/xr_input). Plusieurs commandes
// partagent parfois une même entrée du jeu (recruter et le premier geste) : la première de la
// liste l'emporte.
struct Entry {
    int command;
    Control frame;
    Control quest;
};
const Entry kEntries[] = {
    {kCmdRecruit, kDpadUp, kY},
    {kCmdTauntOne, kDpadLeft, kY},
    {kCmdTauntTwo, kDpadRight, kB},
    {kCmdCancel, kDpadDown, kBHeld},
    {kCmdReload, kX, kX},
    {kCmdGrab, kBumperLeft, kA},
    {kCmdAttackPrimary, kTriggerRight, kTriggerRight},
    {kCmdAttackSecondary, kTriggerLeft, kTriggerLeft},
    {kCmdVehicleFire, kTriggerRight, kTriggerRight},
    {kCmdVehicleFireSecondary, kTriggerLeft, kTriggerLeft},
    {kCmdJump, kBumperRight, kStickRightUp},
    {kCmdSprint, kA, kGripLeft},
    {kCmdCrouch, kStickRightDown, kStickRightDown},
    {kCmdAction, kY, kGripRight},
    {kCmdWeaponMenu, kB, kClickRight},
    {kCmdVehicleWeaponMenu, kB, kClickRight},
    {kCmdPause, kOption, kMenu},
    {kCmdMap, kView, kAHeld},
    {kCmdMenuSelect, kA, kA},
    {kCmdMenuAbort, kB, kB},
};

using KeyTextFn = const wchar_t*(__cdecl*)(const int* slots, int count);
KeyTextFn g_keyText = nullptr;
void* g_target = nullptr;
const int32_t* g_bindings = nullptr;

// Réglages, relus une fois par seconde
std::atomic<ULONGLONG> g_configTick{0};
std::atomic<int> g_mode{1};       // [Hud] prompts
std::atomic<bool> g_french{true}; // [Tool] language
std::atomic<int> g_jumpScan{0x39}, g_crouchScan{0x2E};

// Textes rendus au jeu, qui les recopie dans son message : un anneau, comme le sien
constexpr int kRingSize = 128;
constexpr int kTextSize = 256;
wchar_t g_ring[kRingSize][kTextSize];
std::atomic<unsigned> g_ringNext{0};
std::atomic<bool> g_logged[kSlotCount]{};
std::atomic<bool> g_loggedKept[kSlotCount]{};

void ReadConfig()
{
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG last = g_configTick.load();
    if (last && now - last < 1000)
        return;
    g_configTick = now;
    int mode = config::Int("Hud", "prompts", 1);
    g_mode = mode < 0 || mode > 2 ? 1 : mode;
    g_jumpScan = config::Int("Controls", "jump_scan", 0x39);
    g_crouchScan = config::Int("Controls", "crouch_scan", 0x2E);
    char language[16] = "";
    if (config::Path()[0])
        GetPrivateProfileStringA("Tool", "language", "", language, sizeof(language), config::Path());
    if (language[0])
        g_french = language[0] == 'f' || language[0] == 'F';
    else
        g_french = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_FRENCH;
}

// Manette VR qui produit la touche attribuée à une entrée (les touches que xr_input envoie)
Control ByKey(int key, bool frame)
{
    if (key == g_jumpScan.load())
        return frame ? kBumperRight : kStickRightUp;
    if (key == g_crouchScan.load())
        return kStickRightDown;
    switch (key) {
    case 0x11: return kStickLeftUp;    // W
    case 0x1F: return kStickLeftDown;  // S
    case 0x1E: return kStickLeftLeft;  // A
    case 0x20: return kStickLeftRight; // D
    case 0x12: return frame ? kY : kGripRight; // E
    case 0x2A: return frame ? kA : kGripLeft;  // Maj gauche
    case 0x100: return kTriggerRight;  // bouton gauche de la souris
    case 0x101: return kTriggerLeft;
    case 0x1C: return kA;              // Entrée
    case 0x01: return frame ? kOption : kMenu; // Échap
    case 0xC8: case 0xD0: case 0xCB: case 0xCD: return kStickLeft; // flèches (menus)
    default: return kNone;
    }
}

Control ForSlot(int slot, bool frame)
{
    if (slot < 0 || slot >= kSlotCount)
        return kNone;
    for (const Entry& entry : kEntries)
        if (controls::SlotOf(entry.command) == slot)
            return frame ? entry.frame : entry.quest;
    return g_bindings ? ByKey(g_bindings[slot * 2], frame) : kNone;
}

bool IsLetter(Control control)
{
    return control >= kA && control <= kBHeld;
}

// Ajoute le dessin ou le nom d'un bouton au texte (balises du jeu). false : plus de place.
bool Append(wchar_t* text, Control control, bool french, int mode)
{
    const size_t used = wcslen(text);
    wchar_t* at = text + used;
    const size_t room = kTextSize - used;
    const Label& label = kLabels[control];
    int written = 0;
    if (IsLetter(control)) {
        // Une touche du clavier dessinée, à la lettre du bouton (mode 2 : les dessins de boutons
        // de manette du jeu). Appui long : « tenu » à la suite.
        const wchar_t* image = mode == 2 ? L"btn" : L"key";
        written = swprintf(at, room, L"[format][scale:0.7][image:ui_ctrl_PC_%ls_%ls][/format]", image, label.fr);
        if (written > 0 && (control == kAHeld || control == kBHeld)) {
            const int more = swprintf(at + written, room - written, L"[format][color:green] %ls[/format]", french ? L"tenu" : L"held");
            written = more > 0 ? written + more : -1;
        }
    } else {
        written = swprintf(at, room, L"[format][color:green]%ls[/format]", french ? label.fr : label.en);
    }
    if (written <= 0) {
        *at = 0;
        return false;
    }
    return true;
}

const wchar_t* __cdecl HkKeyText(const int* slots, int count)
{
    ReadConfig();
    const int mode = g_mode.load();
    if (mode == 0 || count < 1 || count > 8)
        return g_keyText(slots, count);
    const bool frame = input::actions::FrameLayout();
    const bool french = g_french.load();

    Control shown[8];
    int found = 0;
    if (!slots) {
        // Sans entrée : le regard ou le choix dans la roue (la souris, dans le jeu d'origine)
        shown[found++] = kStickRight;
    } else {
        bool allMove = true;
        for (int i = 0; i < count; ++i) {
            const Control control = ForSlot(slots[i], frame);
            if (control == kNone) {
                const int slot = slots[i];
                if (slot >= 0 && slot < kSlotCount && !g_loggedKept[slot].exchange(true))
                    LOG_ENGINE("messages : entree %d sans bouton VR (touche 0x%X), la touche du jeu reste affichee", slot,
                               g_bindings ? static_cast<unsigned>(g_bindings[slot * 2]) : 0u);
                return g_keyText(slots, count);
            }
            allMove = allMove && control >= kStickLeftUp && control <= kStickLeftRight;
            shown[found++] = control;
        }
        // Les quatre directions du déplacement : le stick, une fois
        if (count > 1 && allMove) {
            shown[0] = kStickLeft;
            found = 1;
        }
        if (count == 1 && slots[0] >= 0 && slots[0] < kSlotCount && !g_logged[slots[0]].exchange(true))
            LOG_ENGINE("messages : entree %d affichee comme « %s » (disposition %s)", slots[0], kLabels[shown[0]].log,
                       frame ? "Steam Frame" : "Quest 2");
    }

    wchar_t* text = g_ring[g_ringNext.fetch_add(1) % kRingSize];
    text[0] = 0;
    for (int i = 0; i < found; ++i) {
        const size_t used = wcslen(text);
        if (i > 0 && used + 2 < kTextSize) {
            text[used] = L' ';
            text[used + 1] = 0;
        }
        if (!Append(text, shown[i], french, mode))
            return g_keyText(slots, count);
    }
    return text;
}

} // namespace

bool Install()
{
    auto* keyText = static_cast<uint8_t*>(pattern::Find("dessin des touches dans les messages", kKeyTextSig));
    if (!keyText) {
        LOG_ENGINE("messages : fonction du jeu introuvable, les touches du clavier restent affichees");
        return false;
    }
    const uint8_t* insn = keyText + kBindingsInsn;
    if (insn[0] == 0x8B && insn[1] == 0x3C && insn[2] == 0xC5) {
        uintptr_t table = 0;
        memcpy(&table, insn + 3, sizeof(table));
        g_bindings = reinterpret_cast<const int32_t*>(table);
    } else {
        LOG_ENGINE("messages : table des touches introuvable (seules les commandes connues changent d'affichage)");
    }
    const MH_STATUS st = MH_CreateHook(keyText, reinterpret_cast<void*>(&HkKeyText), reinterpret_cast<void**>(&g_keyText));
    if (st != MH_OK || MH_EnableHook(keyText) != MH_OK) {
        LOG_ENGINE("messages : ECHEC du detour du dessin des touches (%s)", MH_StatusToString(st));
        return false;
    }
    g_target = keyText;
    LOG_ENGINE("messages : dessin des touches exe+0x%X detourne, table des touches en 0x%08X : les messages du jeu "
               "montrent les boutons des manettes ([Hud] prompts)",
               static_cast<unsigned>(reinterpret_cast<uintptr_t>(keyText) - reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr))),
               static_cast<unsigned>(reinterpret_cast<uintptr_t>(g_bindings)));
    return true;
}

void Remove()
{
    if (g_target) {
        MH_DisableHook(g_target);
        MH_RemoveHook(g_target);
        g_target = nullptr;
    }
}

} // namespace engine::prompts
