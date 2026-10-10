// SR2VR - point d'entrée de la DLL injectée dans SR2_pc.exe (32 bits)
// Décrit le processus dans sr2_vr.log, puis installe les hooks du rendu.
#include <windows.h>
#include <tlhelp32.h>
#include <winver.h>

#include <MinHook.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include "engine/camera_hook.h"
#include "render/d3d9_hook.h"
#include "util/config.h"
#include "util/log.h"
#include "xr/xr_session.h"

namespace {

bool g_minhookReady = false;

// Date de compilation et taille en mémoire d'un module chargé (en-tête PE)
void DescribeImage(HMODULE module, DWORD* timestamp, DWORD* imageSize)
{
    auto* base = reinterpret_cast<const BYTE*>(module);
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    *timestamp = nt->FileHeader.TimeDateStamp;
    *imageSize = nt->OptionalHeader.SizeOfImage;
}

std::string FormatTimestamp(DWORD timestamp)
{
    const __time64_t t = timestamp;
    tm utc{};
    char text[32] = "?";
    if (_gmtime64_s(&utc, &t) == 0)
        strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &utc);
    return text;
}

// Taille du fichier sur disque : avec la date de compilation, identifie la version de l'exe
unsigned long long FileSize(const char* path)
{
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &info))
        return 0;
    return (static_cast<unsigned long long>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
}

// Version inscrite dans les ressources du fichier ("" s'il n'en a pas)
std::string FileVersion(const char* path)
{
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeA(path, &handle);
    if (!size)
        return {};
    std::string data(size, '\0');
    VS_FIXEDFILEINFO* info = nullptr;
    UINT length = 0;
    if (!GetFileVersionInfoA(path, 0, size, data.data()) ||
        !VerQueryValueA(data.data(), "\\", reinterpret_cast<void**>(&info), &length) || !info)
        return {};
    char text[64];
    snprintf(text, sizeof(text), "%u.%u.%u.%u", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
             HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
    return text;
}

// Le module est-il rangé dans le dossier du jeu (DLL locale) plutôt que dans Windows ?
bool InGameDir(const char* modulePath, const char* gameDir)
{
    return _strnicmp(modulePath, gameDir, strlen(gameDir)) == 0;
}

void LogModule(const char* name, const char* role, const char* gameDir)
{
    HMODULE module = GetModuleHandleA(name);
    if (!module) {
        LOG_CORE("  %-14s absente   (%s)", name, role);
        return;
    }
    char path[MAX_PATH]{};
    GetModuleFileNameA(module, path, MAX_PATH);
    LOG_CORE("  %-14s %p  %s  [%s] (%s)", name, static_cast<void*>(module), path,
             InGameDir(path, gameDir) ? "dossier du jeu" : "systeme", role);
}

// Tous les modules du processus : repère un d3d9.dll de remplacement (DXVK, ReShade...) ou
// une autre DLL injectée qui poserait ses propres hooks
void LogAllModules()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) {
        LOG_CORE("liste des modules indisponible (erreur %lu)", GetLastError());
        return;
    }
    MODULEENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    int count = 0;
    LOG_CORE("modules charges :");
    for (BOOL ok = Module32First(snap, &entry); ok; ok = Module32Next(snap, &entry), ++count)
        LOG_CORE("  %p %8lu Ko  %s", static_cast<void*>(entry.modBaseAddr), entry.modBaseSize / 1024, entry.szExePath);
    LOG_CORE("  (%d modules)", count);
    CloseHandle(snap);
}

struct WindowSearch {
    HWND found = nullptr;
};

BOOL CALLBACK FindGameWindow(HWND window, LPARAM param)
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
    reinterpret_cast<WindowSearch*>(param)->found = window;
    return FALSE;
}

// Fenêtre principale du jeu : attendue jusqu'à 60 s (l'injection peut précéder sa création)
void LogGameWindow()
{
    WindowSearch search;
    for (int i = 0; i < 240 && !search.found; ++i) {
        EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&search));
        if (!search.found)
            Sleep(250);
    }
    if (!search.found) {
        LOG_CORE("fenetre du jeu introuvable apres 60 s");
        return;
    }
    char cls[128]{}, title[128]{};
    GetClassNameA(search.found, cls, sizeof(cls));
    GetWindowTextA(search.found, title, sizeof(title));
    RECT client{}, outer{};
    GetClientRect(search.found, &client);
    GetWindowRect(search.found, &outer);
    const LONG style = GetWindowLongA(search.found, GWL_STYLE);
    const LONG exStyle = GetWindowLongA(search.found, GWL_EXSTYLE);
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    GetMonitorInfoA(MonitorFromWindow(search.found, MONITOR_DEFAULTTONEAREST), &monitor);
    LOG_CORE("fenetre du jeu : %p classe \"%s\" titre \"%s\"", static_cast<void*>(search.found), cls, title);
    LOG_CORE("  zone cliente : %ld x %ld, position ecran (%ld, %ld), ecran %ld x %ld", client.right, client.bottom,
             outer.left, outer.top, monitor.rcMonitor.right - monitor.rcMonitor.left,
             monitor.rcMonitor.bottom - monitor.rcMonitor.top);
    LOG_CORE("  style 0x%08lX (%s), style etendu 0x%08lX, thread %lu", style,
             (style & WS_CAPTION) == WS_CAPTION ? "fenetre avec bordure" : "sans bordure", exStyle,
             GetWindowThreadProcessId(search.found, nullptr));
}

DWORD WINAPI InitThread(LPVOID)
{
    HMODULE exe = GetModuleHandleA(nullptr);
    char exePath[MAX_PATH]{};
    GetModuleFileNameA(exe, exePath, MAX_PATH);
    std::string gameDir = exePath;
    gameDir.erase(gameDir.find_last_of('\\') + 1);

    DWORD stamp = 0, imageSize = 0;
    DescribeImage(exe, &stamp, &imageSize);

    LOG_CORE("DLL injectee (SR2VR %s)", SR2VR_VERSION);
    LOG_CORE("  processus   : %s (PID %lu)", exePath, GetCurrentProcessId());
    LOG_CORE("  arguments   : %s", GetCommandLineA());
    LOG_CORE("  base exe    : %p, image %lu Ko", static_cast<void*>(exe), imageSize / 1024);
    LOG_CORE("  architecture: %u bits", static_cast<unsigned>(sizeof(void*) * 8));
    LOG_CORE("  exe compile : %s UTC (0x%08lX), fichier %llu octets", FormatTimestamp(stamp).c_str(), stamp,
             FileSize(exePath));

    // Juiced Patch : c'est le DFEngine.dll du jeu, remplacé
    if (HMODULE juiced = GetModuleHandleA("DFEngine.dll")) {
        char path[MAX_PATH]{};
        GetModuleFileNameA(juiced, path, MAX_PATH);
        DescribeImage(juiced, &stamp, &imageSize);
        const std::string version = FileVersion(path);
        LOG_CORE("  DFEngine.dll: %p, compile %s UTC, %llu octets, version %s", static_cast<void*>(juiced),
                 FormatTimestamp(stamp).c_str(), FileSize(path), version.empty() ? "non renseignee" : version.c_str());
        // Le DFEngine.dll d'origine date de 2009 : plus récent = Juiced
        LOG_CORE("  Juiced Patch: %s", stamp > 0x4B000000 ? "present (DFEngine.dll remplace)" : "ABSENT (DFEngine.dll d'origine)");
    } else {
        LOG_CORE("  DFEngine.dll: absente (inattendu : l'exe l'importe)");
    }

    LOG_CORE("modules graphiques et d'entrees :");
    LogModule("d3d9.dll", "rendu", gameDir.c_str());
    LogModule("d3dx9_39.dll", "utilitaires D3DX", gameDir.c_str());
    LogModule("d3d11.dll", "rendu, inattendu", gameDir.c_str());
    LogModule("dxgi.dll", "rendu, inattendu", gameDir.c_str());
    LogModule("opengl32.dll", "rendu, inattendu", gameDir.c_str());
    LogModule("vulkan-1.dll", "DXVK", gameDir.c_str());
    LogModule("dinput8.dll", "clavier / souris", gameDir.c_str());
    LogModule("xinput1_3.dll", "manette, import de l'exe", gameDir.c_str());
    LogModule("xinput1_4.dll", "manette, chargee par Juiced", gameDir.c_str());
    LogModule("SDL3.dll", "manette via Juiced", gameDir.c_str());
    LogModule("binkw32.dll", "videos", gameDir.c_str());
    LogModule("dsound.dll", "son", gameDir.c_str());
    LogModule("xaudio2_7.dll", "son", gameDir.c_str());
    LogAllModules();

    const MH_STATUS status = MH_Initialize();
    g_minhookReady = status == MH_OK;
    LOG_CORE("MinHook : %s", MH_StatusToString(status));

    LogGameWindow();

    if (!g_minhookReady || !render::InstallD3D9Hooks()) {
        LOG_CORE("hook du renderer indisponible : le jeu continue sans VR");
        return 0;
    }
    xr::Start();
    engine::InstallCameraHook();

    // Le jeu présente-t-il des images par les fonctions détournées ? Vérifié une fois.
    Sleep(15000);
    if (render::PresentCount() == 0)
        LOG_CORE("ATTENTION : aucun Present vu 15 s apres l'installation des hooks");
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        vrlog::Init(hModule);
        config::Init(hModule);
        // Jamais de travail lourd dans DllMain (loader lock) : on délègue à un thread
        if (HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr))
            CloseHandle(t);
    } else if (reason == DLL_PROCESS_DETACH && reserved == nullptr) {
        // FreeLibrary explicite : on retire les hooks. À la fermeture du processus
        // (reserved != nullptr) les autres threads sont déjà morts, on ne touche à rien.
        xr::RequestStop();
        engine::RemoveCameraHook();
        render::RemoveD3D9Hooks();
        if (g_minhookReady)
            MH_Uninitialize();
    }
    return TRUE;
}
