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

bool SendKey(WORD vk, bool down)
{
    if (down) {
        HWND game = GameWindow();
        if (!game || GetForegroundWindow() != game) {
            if (!g_warned) {
                LOG_CORE("touches : la fenetre du jeu n'est pas au premier plan, appui ignore (cliquer dans le jeu)");
                g_warned = true;
            }
            if (game)
                SetForegroundWindow(game);
            return false;
        }
        g_warned = false;
    }
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC)); // gère aussi VK_LSHIFT / VK_LCONTROL
    in.ki.dwFlags = (IsExtended(vk) ? KEYEVENTF_EXTENDEDKEY : 0u) | (down ? 0u : KEYEVENTF_KEYUP);
    if (SendInput(1, &in, sizeof(in)) != 1)
        return false;
    if (down)
        ++g_sent;
    return true;
}

unsigned TakeSentCount()
{
    return g_sent.exchange(0);
}

} // namespace input
