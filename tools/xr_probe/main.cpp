// SR2VR - banc d'essai OpenXR 32 bits, hors de Saints Row 2.
//
// Faux jeu : une fenêtre et un device Direct3D 9 classique (mêmes drapeaux que celui du jeu),
// une image de couleur changeante, la même capture (render/frame_capture) et la même session
// OpenXR (xr/xr_session) que la DLL. Sert à savoir si un problème vient du runtime OpenXR
// 32 bits ou du processus du jeu (Juiced, mémoire, hooks).
//
// Sans argument : lance deux essais à la suite, chacun dans son propre processus (un
// plantage de l'un n'empêche pas l'autre), et résume dans xr_probe.log :
//   1. runtime OpenXR actif de Windows        -> xr_probe_defaut.log
//   2. runtime 32 bits de SteamVR, s'il existe -> xr_probe_steamvr.log
// Usage interne : xr_probe --child <nom> [--seconds <n>]
#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "render/frame_capture.h"
#include "util/config.h"
#include "util/log.h"
#include "xr/xr_session.h"

namespace {

bool FileExists(const std::string& p)
{
    const DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string ModuleOf(const void* address)
{
    HMODULE module = nullptr;
    char text[MAX_PATH + 32] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(address), &module) &&
        module) {
        char path[MAX_PATH]{};
        GetModuleFileNameA(module, path, MAX_PATH);
        const char* name = strrchr(path, '\\');
        snprintf(text, sizeof(text), "%s+0x%lX", name ? name + 1 : path,
                 static_cast<unsigned long>(reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(module)));
    }
    return text;
}

// Plantage (du runtime, typiquement) : noté dans le log avant de quitter
LONG WINAPI OnCrash(EXCEPTION_POINTERS* info)
{
    const EXCEPTION_RECORD* r = info->ExceptionRecord;
    LOG_CORE("PLANTAGE : exception 0x%08lX a %p (%s), thread %lu", r->ExceptionCode, r->ExceptionAddress,
             ModuleOf(r->ExceptionAddress).c_str(), GetCurrentThreadId());
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2)
        LOG_CORE("  violation d'acces en %s a l'adresse 0x%08lX", r->ExceptionInformation[0] ? "ecriture" : "lecture",
                 static_cast<unsigned long>(r->ExceptionInformation[1]));
    LOG_CORE("RESULTAT : PLANTAGE");
    ExitProcess(3);
}

// --- Essai (processus enfant) ---------------------------------------------------------

int RunChild(const std::string& name, int seconds)
{
    vrlog::Init(GetModuleHandleA(nullptr), ("xr_probe_" + name + ".log").c_str());
    config::Init(GetModuleHandleA(nullptr));
    SetUnhandledExceptionFilter(OnCrash);
    LOG_CORE("xr_probe %s, essai \"%s\", %u bits, %d s", SR2VR_VERSION, name.c_str(),
             static_cast<unsigned>(sizeof(void*) * 8), seconds);

    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "SR2VR_xr_probe";
    RegisterClassExA(&wc);
    HWND window = CreateWindowExA(0, wc.lpszClassName, "SR2VR xr_probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100,
                                  656, 399, nullptr, nullptr, wc.hInstance, nullptr);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!window || !d3d) {
        LOG_CORE("ERREUR : fenetre ou Direct3D 9 indisponible");
        return 1;
    }
    // Comme le jeu : device classique, sommets matériels, PUREDEVICE, MULTITHREADED
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    pp.BackBufferWidth = 1920;
    pp.BackBufferHeight = 1080;
    pp.hDeviceWindow = window;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9* device = nullptr;
    const HRESULT hr =
        d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                          D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_PUREDEVICE | D3DCREATE_MULTITHREADED, &pp,
                          &device);
    if (FAILED(hr) || !device) {
        LOG_CORE("ERREUR : device D3D9 non cree (0x%08lX)", static_cast<unsigned long>(hr));
        return 1;
    }
    LOG_CORE("faux jeu : device D3D9 1920x1080 cree, demarrage de la session OpenXR");

    xr::Start();

    const ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000;
    unsigned frames = 0;
    while (GetTickCount64() < end) {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        // Bandes de couleur qui défilent : on voit au casque que l'image est vivante
        const int phase = static_cast<int>(frames % 240);
        device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(30, 60, 120), 1.0f, 0);
        const D3DRECT band{phase * 8, 0, phase * 8 + 160, 1080};
        device->Clear(1, &band, D3DCLEAR_TARGET, D3DCOLOR_XRGB(240, 200, 40), 1.0f, 0);
        const D3DRECT square{860, 440, 1060, 640};
        device->Clear(1, &square, D3DCLEAR_TARGET, D3DCOLOR_XRGB(200, 40, 40), 1.0f, 0);
        device->BeginScene();
        device->EndScene();
        render::CaptureFrame(device, render::FrameView{});
        device->Present(nullptr, nullptr, nullptr, nullptr);
        ++frames;
        Sleep(8);
    }
    LOG_CORE("faux jeu : %u images presentees en %d s", frames, seconds);
    xr::RequestStop();
    Sleep(500);
    LOG_CORE("RESULTAT : TERMINE SANS PLANTAGE (lire les lignes [xr] pour l'etat de la session)");
    // Pas de destruction ordonnée : le thread XR est détaché, on quitte comme le jeu
    ExitProcess(0);
}

// --- Lanceur (processus parent) -------------------------------------------------------

// Manifeste OpenXR 32 bits de SteamVR dans une bibliothèque Steam, "" si absent
std::string FindSteamVrRuntime()
{
    char steam[MAX_PATH]{};
    DWORD size = sizeof(steam);
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath", RRF_RT_REG_SZ, nullptr, steam, &size) !=
        ERROR_SUCCESS)
        return {};
    std::string root = steam;
    std::replace(root.begin(), root.end(), '/', '\\');
    std::vector<std::string> libraries{root};
    FILE* f = nullptr;
    if (fopen_s(&f, (root + "\\steamapps\\libraryfolders.vdf").c_str(), "rb") == 0 && f) {
        char line[1024];
        while (fgets(line, sizeof(line), f)) {
            const std::string text = line;
            const size_t key = text.find("\"path\"");
            if (key == std::string::npos)
                continue;
            const size_t open = text.find('"', key + 6);
            const size_t close = open == std::string::npos ? open : text.find('"', open + 1);
            if (close == std::string::npos)
                continue;
            std::string value;
            for (size_t i = open + 1; i < close; ++i) {
                if (text[i] == '\\' && i + 1 < close && text[i + 1] == '\\')
                    ++i;
                value += text[i];
            }
            libraries.push_back(value);
        }
        fclose(f);
    }
    for (const std::string& library : libraries) {
        const std::string json = library + "\\steamapps\\common\\SteamVR\\steamxr_win32.json";
        if (FileExists(json))
            return json;
    }
    return {};
}

// Lance un essai et attend sa fin. Renvoie son code de sortie (0xFFFFFFFF : pas lancé,
// 0xFFFFFFFE : tué après le délai).
DWORD RunTrial(const char* name, int seconds)
{
    char self[MAX_PATH]{};
    GetModuleFileNameA(nullptr, self, MAX_PATH);
    char cmd[MAX_PATH + 128];
    snprintf(cmd, sizeof(cmd), "\"%s\" --child %s --seconds %d", self, name, seconds);
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(self, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
        return 0xFFFFFFFF;
    DWORD code = 0xFFFFFFFE;
    if (WaitForSingleObject(pi.hProcess, (seconds + 30) * 1000) == WAIT_OBJECT_0)
        GetExitCodeProcess(pi.hProcess, &code);
    else
        TerminateProcess(pi.hProcess, 0xFFFFFFFE);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

const char* Verdict(DWORD code)
{
    switch (code) {
    case 0: return "termine sans plantage";
    case 1: return "faux jeu non demarre";
    case 3: return "PLANTAGE (voir son log)";
    case 0xFFFFFFFE: return "bloque, tue apres le delai";
    case 0xFFFFFFFF: return "non lance";
    default: return "termine anormalement";
    }
}

} // namespace

int main(int argc, char** argv)
{
    std::string child;
    int seconds = 30;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--child") && i + 1 < argc)
            child = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc)
            seconds = atoi(argv[++i]);
    }
    if (!child.empty())
        return RunChild(child, seconds);

    vrlog::Init(GetModuleHandleA(nullptr), "xr_probe.log");
    LOG_CORE("xr_probe %s : essais OpenXR 32 bits hors du jeu, %d s chacun", SR2VR_VERSION, seconds);

    printf("Essai 1/2 : runtime OpenXR actif de Windows (%d s)...\n", seconds);
    SetEnvironmentVariableA("XR_RUNTIME_JSON", nullptr);
    const DWORD first = RunTrial("defaut", seconds);
    LOG_CORE("essai \"defaut\" (runtime actif de Windows) : code 0x%08lX, %s -> xr_probe_defaut.log", first,
             Verdict(first));

    const std::string steamvr = FindSteamVrRuntime();
    if (steamvr.empty()) {
        LOG_CORE("essai \"steamvr\" : steamxr_win32.json introuvable dans les bibliotheques Steam, essai saute");
    } else {
        printf("Essai 2/2 : runtime 32 bits de SteamVR (%d s, SteamVR va demarrer)...\n", seconds + 15);
        SetEnvironmentVariableA("XR_RUNTIME_JSON", steamvr.c_str());
        const DWORD second = RunTrial("steamvr", seconds + 15);
        LOG_CORE("essai \"steamvr\" (%s) : code 0x%08lX, %s -> xr_probe_steamvr.log", steamvr.c_str(), second,
                 Verdict(second));
    }
    LOG_CORE("FIN DES ESSAIS");
    return 0;
}
