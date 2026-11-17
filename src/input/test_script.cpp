#include "input/test_script.h"
#include "input/keys.h"
#include "input/vpad.h"
#include "util/config.h"
#include "util/log.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define LOG_TEST(...) ::vrlog::Write("[test]", __VA_ARGS__)

namespace input {
namespace {

std::vector<std::string> g_lines;
std::atomic<int> g_shotsAsked{0};
std::atomic<int> g_shotsTaken{0};

int Named(const char* name, const char* const* names, int count)
{
    for (int i = 0; i < count; ++i)
        if (names[i] && !_stricmp(name, names[i]))
            return i;
    return -1;
}

// Dans l'ordre de vpad::Button et vpad::Axis
const char* const kButtons[vpad::kButtonCount] = {"a",  "b",  "x",  "y",  "back", "guide", "start", "ls",
                                                 "rs", "lb", "rb", "up", "down", "left",  "right"};
const char* const kAxes[vpad::kAxisCount] = {"lx", "ly", "rx", "ry", "lt", "rt"};

// La manette virtuelle n'est attachée par le thread du casque que si une session tourne
bool PadReady()
{
    for (int i = 0; i < 20 && !vpad::Ready(); ++i) {
        vpad::Attach();
        if (!vpad::Ready())
            Sleep(500);
    }
    return vpad::Ready();
}

DWORD WINAPI Run(LPVOID)
{
    LOG_TEST("script d'entrees : %zu lignes", g_lines.size());
    for (const std::string& line : g_lines) {
        char cmd[16]{}, name[16]{};
        float value = 0;
        unsigned ms = 0;
        char text[200]{};
        if (sscanf_s(line.c_str(), "%15s", cmd, static_cast<unsigned>(sizeof(cmd))) != 1 || cmd[0] == '#')
            continue;
        const char* rest = line.c_str() + line.find(cmd) + strlen(cmd);
        if (!strcmp(cmd, "wait")) {
            Sleep(static_cast<DWORD>(strtoul(rest, nullptr, 0)));
        } else if (!strcmp(cmd, "pad") && sscanf_s(rest, "%15s %u", name, static_cast<unsigned>(sizeof(name)), &ms) >= 1) {
            const int button = Named(name, kButtons, vpad::kButtonCount);
            if (button < 0 || !PadReady()) {
                LOG_TEST("pad %s : %s", name, button < 0 ? "bouton inconnu" : "manette virtuelle pas prete");
                continue;
            }
            LOG_TEST("pad %s", name);
            vpad::SetScriptButton(button, true);
            Sleep(ms ? ms : 150);
            vpad::SetScriptButton(button, false);
            Sleep(100);
        } else if (!strcmp(cmd, "axis") &&
                   sscanf_s(rest, "%15s %f %u", name, static_cast<unsigned>(sizeof(name)), &value, &ms) >= 2) {
            const int axis = Named(name, kAxes, vpad::kAxisCount);
            if (axis < 0 || !PadReady()) {
                LOG_TEST("axis %s : %s", name, axis < 0 ? "axe inconnu" : "manette virtuelle pas prete");
                continue;
            }
            LOG_TEST("axis %s %.2f", name, value);
            vpad::SetScriptAxis(axis, true, value);
            Sleep(ms ? ms : 500);
            vpad::SetScriptAxis(axis, false, 0.0f);
            Sleep(100);
        } else if (!strcmp(cmd, "key")) {
            char* end = nullptr;
            const WORD vk = static_cast<WORD>(strtoul(rest, &end, 0));
            const DWORD hold = end && *end ? static_cast<DWORD>(strtoul(end, nullptr, 0)) : 0;
            const bool sent = SendKey(vk, true);
            Sleep(hold ? hold : 100);
            SendKey(vk, false);
            LOG_TEST("key 0x%02X%s", vk, sent ? "" : " (non envoyee : le jeu n'est pas au premier plan)");
            Sleep(100);
        } else if (!strcmp(cmd, "shot")) {
            const int asked = ++g_shotsAsked;
            for (int i = 0; i < 30 && g_shotsTaken.load() < asked; ++i)
                Sleep(100);
            if (g_shotsTaken.load() < asked)
                LOG_TEST("shot %d : pas de capture (aucun casque ne recoit d'images)", asked);
        } else if (!strcmp(cmd, "log") &&
                   sscanf_s(rest, " %199[^\r\n]", text, static_cast<unsigned>(sizeof(text))) == 1) {
            LOG_TEST("%s", text);
        } else {
            LOG_TEST("ligne ignoree : %s", line.c_str());
        }
    }
    LOG_TEST("script d'entrees termine");
    return 0;
}

} // namespace

bool TakeShotRequest(int* number)
{
    const int asked = g_shotsAsked.load();
    if (g_shotsTaken.load() >= asked)
        return false;
    g_shotsTaken = asked;
    *number = asked;
    return true;
}

void StartTestScript()
{
    std::string path = config::Path();
    const size_t slash = path.find_last_of("\\/");
    path = (slash == std::string::npos ? std::string() : path.substr(0, slash + 1)) + "sr2_vr_test.txt";
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "r") || !f)
        return;
    char buf[256];
    while (fgets(buf, sizeof(buf), f))
        g_lines.emplace_back(buf);
    fclose(f);
    if (HANDLE t = CreateThread(nullptr, 0, Run, nullptr, 0, nullptr))
        CloseHandle(t);
}

} // namespace input
