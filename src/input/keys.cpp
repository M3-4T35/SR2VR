#include "input/keys.h"
#include "util/log.h"

#include <atomic>
#include <cstring>

namespace input {
namespace {

HWND g_window = nullptr;
std::atomic<unsigned> g_sent{0};
bool g_warned = false;

BOOL CALLBACK FindWindowOfProcess(HWND window, LPARAM param)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(window) || GetWindow(window, GW_OWNER))
        return TRUE;
    char cls[64]{};
    GetClassNameA(window, cls, sizeof(cls));
    if (strcmp(cls, "ConsoleWindowClass") == 0) // console de debug de Juiced
        return TRUE;
    RECT client{};
    GetClientRect(window, &client);
    if (client.right < 320 || client.bottom < 200)
        return TRUE;
    *reinterpret_cast<HWND*>(param) = window;
    return FALSE;
}

bool IsExtended(WORD vk)
{
    switch (vk) {
    case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT:
    case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT: case VK_INSERT: case VK_DELETE:
        return true;
    default:
        return false;
    }
}

} // namespace

HWND GameWindow()
{
    if (!g_window || !IsWindow(g_window)) {
        g_window = nullptr;
        EnumWindows(FindWindowOfProcess, reinterpret_cast<LPARAM>(&g_window));
    }
    return g_window;
}

namespace {

// Le jeu est-il au premier plan ? Sinon on le signale une fois et on tente de l'y remettre.
bool GameInFront()
{
    HWND game = GameWindow();
    if (game && GetForegroundWindow() == game) {
        g_warned = false;
        return true;
    }
    if (!g_warned) {
        LOG_CORE("touches : la fenetre du jeu n'est pas au premier plan, entrees ignorees (cliquer dans le jeu)");
        g_warned = true;
    }
    if (game)
        SetForegroundWindow(game);
    return false;
}

bool Send(INPUT& in, bool down)
{
    // Un relâchement part toujours : la touche a été enfoncée par nous
    if (down && !GameInFront())
        return false;
    if (SendInput(1, &in, sizeof(in)) != 1)
        return false;
    if (down)
        ++g_sent;
    return true;
}

} // namespace

bool SendKey(WORD vk, bool down)
{
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC)); // gère aussi VK_LSHIFT / VK_LCONTROL
    in.ki.dwFlags = (IsExtended(vk) ? KEYEVENTF_EXTENDEDKEY : 0u) | (down ? 0u : KEYEVENTF_KEYUP);
    return Send(in, down);
}

bool SendScan(WORD scanCode, bool down)
{
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = scanCode;
    in.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0u : KEYEVENTF_KEYUP);
    return Send(in, down);
}

bool MoveMouse(int dx, int dy)
{
    if (!dx && !dy)
        return true;
    HWND game = GameWindow();
    if (!game || GetForegroundWindow() != game)
        return false;
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dy = dy;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    return SendInput(1, &in, sizeof(in)) == 1;
}

bool SendMouseButton(int button, bool down)
{
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = button == 0 ? (down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP)
                                : (down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP);
    return Send(in, down);
}

unsigned TakeSentCount()
{
    return g_sent.exchange(0);
}

} // namespace input
