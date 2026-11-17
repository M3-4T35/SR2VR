#include "input/vpad.h"
#include "util/log.h"

#include <mutex>
#include <windows.h>

#include <cstdint>
#include <cstring>

namespace input::vpad {
namespace {

// Ce qu'il faut de l'interface de SDL 3.2 (SDL_joystick.h), sans dépendre de ses en-têtes
struct VirtualJoystickDesc {
    uint32_t version; // taille de la structure
    uint16_t type;    // SDL_JoystickType
    uint16_t padding;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t naxes;
    uint16_t nbuttons;
    uint16_t nballs;
    uint16_t nhats;
    uint16_t ntouchpads;
    uint16_t nsensors;
    uint16_t padding2[2];
    uint32_t button_mask;
    uint32_t axis_mask;
    const char* name;
    const void* touchpads;
    const void* sensors;
    void* userdata;
    void* Update;
    void* SetPlayerIndex;
    void* Rumble;
    void* RumbleTriggers;
    void* SetLED;
    void* SendEffect;
    void* SetSensorsEnabled;
    void* Cleanup;
};
constexpr uint32_t kInitGamepad = 0x00002000u; // SDL_INIT_GAMEPAD
constexpr uint16_t kTypeGamepad = 1;           // SDL_JOYSTICK_TYPE_GAMEPAD

using WasInitFn = uint32_t(__cdecl*)(uint32_t);
using AttachFn = uint32_t(__cdecl*)(const VirtualJoystickDesc*);
using OpenFn = void*(__cdecl*)(uint32_t);
using SetAxisFn = bool(__cdecl*)(void*, int, int16_t);
using SetButtonFn = bool(__cdecl*)(void*, int, bool);
using GetErrorFn = const char*(__cdecl*)();

WasInitFn g_wasInit = nullptr;
AttachFn g_attach = nullptr;
OpenFn g_open = nullptr;
SetAxisFn g_setAxis = nullptr;
SetButtonFn g_setButton = nullptr;
GetErrorFn g_getError = nullptr;

void* g_joystick = nullptr;
bool g_failed = false; // échec définitif, déjà loggué
bool g_waitingLogged = false;
bool g_buttons[kButtonCount]{};
int16_t g_axes[kAxisCount]{};
// Deux threads au plus : celui du casque et celui du script d'essai
std::recursive_mutex g_mutex;
bool g_wanted[kButtonCount]{};    // ce que demandent les manettes VR
float g_wantedAxes[kAxisCount]{};
bool g_script[kButtonCount]{};    // ce que tient le script d'essai
bool g_scriptAxisOn[kAxisCount]{};
float g_scriptAxes[kAxisCount]{};

template <typename T>
bool Resolve(HMODULE sdl, const char* name, T* out)
{
    *out = reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(sdl, name)));
    return *out != nullptr;
}

void SendAxis(int axis, int16_t value)
{
    g_axes[axis] = value;
    g_setAxis(g_joystick, axis, value);
}

} // namespace

bool Ready()
{
    return g_joystick != nullptr;
}

bool Attach()
{
    std::lock_guard lock(g_mutex);
    if (g_joystick)
        return true;
    if (g_failed)
        return false;
    const HMODULE sdl = GetModuleHandleA("SDL3.dll");
    if (!sdl) {
        if (!g_waitingLogged) {
            LOG_XR("manette virtuelle : SDL3.dll n'est pas chargee par le jeu (Juiced, SDL=1), pas de roue des armes");
            g_waitingLogged = true;
        }
        return false;
    }
    if (!g_attach) {
        if (!Resolve(sdl, "SDL_WasInit", &g_wasInit) || !Resolve(sdl, "SDL_AttachVirtualJoystick", &g_attach) ||
            !Resolve(sdl, "SDL_OpenJoystick", &g_open) || !Resolve(sdl, "SDL_SetJoystickVirtualAxis", &g_setAxis) ||
            !Resolve(sdl, "SDL_SetJoystickVirtualButton", &g_setButton)) {
            LOG_XR("manette virtuelle : fonctions absentes de SDL3.dll, abandon");
            g_failed = true;
            return false;
        }
        Resolve(sdl, "SDL_GetError", &g_getError);
    }
    // Juiced initialise les manettes de SDL au démarrage du jeu
    if (!(g_wasInit(kInitGamepad) & kInitGamepad))
        return false;

    VirtualJoystickDesc desc{};
    desc.version = sizeof(desc);
    desc.type = kTypeGamepad;
    desc.vendor_id = 0x045E;  // Microsoft
    desc.product_id = 0x028E; // manette Xbox 360
    desc.naxes = kAxisCount;
    desc.nbuttons = kButtonCount;
    desc.button_mask = (1u << kButtonCount) - 1;
    desc.axis_mask = (1u << kAxisCount) - 1;
    desc.name = "SR2VR - manettes VR";
    const uint32_t id = g_attach(&desc);
    void* joystick = id ? g_open(id) : nullptr;
    if (!joystick) {
        LOG_XR("manette virtuelle : ECHEC (%s)", g_getError ? g_getError() : "?");
        g_failed = true;
        return false;
    }
    g_joystick = joystick;
    // Gâchettes relâchées : l'axe complet de -32768 à 32767 devient 0 à 1 côté manette
    memset(g_buttons, 0, sizeof(g_buttons));
    for (int axis = 0; axis < kAxisCount; ++axis)
        SendAxis(axis, axis >= kLeftTrigger ? INT16_MIN : 0);
    LOG_XR("manette virtuelle : attachee a SDL (numero %u), le jeu la voit comme une manette branchee",
           static_cast<unsigned>(id));
    return true;
}

namespace {

void ApplyButton(int button)
{
    const bool down = g_wanted[button] || g_script[button];
    if (!g_joystick || g_buttons[button] == down)
        return;
    g_buttons[button] = down;
    g_setButton(g_joystick, button, down);
}

void ApplyAxis(int axis)
{
    if (!g_joystick)
        return;
    const float value = g_scriptAxisOn[axis] ? g_scriptAxes[axis] : g_wantedAxes[axis];
    int raw = 0;
    if (axis >= kLeftTrigger) {
        const float v = value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
        raw = static_cast<int>(v * 65535.0f) - 32768;
    } else {
        const float v = value < -1.0f ? -1.0f : value > 1.0f ? 1.0f : value;
        raw = static_cast<int>(v * 32767.0f);
    }
    const auto clamped = static_cast<int16_t>(raw < INT16_MIN ? INT16_MIN : raw > INT16_MAX ? INT16_MAX : raw);
    if (g_axes[axis] != clamped)
        SendAxis(axis, clamped);
}

} // namespace

void SetButton(int button, bool down)
{
    if (button < 0 || button >= kButtonCount)
        return;
    std::lock_guard lock(g_mutex);
    g_wanted[button] = down;
    ApplyButton(button);
}

void SetAxis(int axis, float value)
{
    if (axis < 0 || axis >= kAxisCount)
        return;
    std::lock_guard lock(g_mutex);
    g_wantedAxes[axis] = value;
    ApplyAxis(axis);
}

void SetScriptButton(int button, bool down)
{
    if (button < 0 || button >= kButtonCount)
        return;
    std::lock_guard lock(g_mutex);
    g_script[button] = down;
    ApplyButton(button);
}

void SetScriptAxis(int axis, bool active, float value)
{
    if (axis < 0 || axis >= kAxisCount)
        return;
    std::lock_guard lock(g_mutex);
    g_scriptAxisOn[axis] = active;
    g_scriptAxes[axis] = value;
    ApplyAxis(axis);
}

void Reset()
{
    if (!g_joystick)
        return;
    for (int button = 0; button < kButtonCount; ++button)
        SetButton(button, false);
    for (int axis = 0; axis < kAxisCount; ++axis)
        SetAxis(axis, 0.0f);
}

} // namespace input::vpad
