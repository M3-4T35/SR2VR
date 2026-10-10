// SR2VR - injecteur CLI 32 bits
// Lance SR2_pc.exe depuis le dossier du jeu puis y injecte sr2_vr.dll
// (CreateRemoteThread + LoadLibraryW). Ne modifie aucun fichier du jeu.
// 32 bits comme le jeu : l'adresse de LoadLibraryW doit être celle du kernel32 32 bits.
//
// Usage : sr2_vr_injector [--game-dir <dossier>] [--dll <chemin>] [--early]
//                         [--steamvr | --runtime <json>]
//                         [--wait] [--timeout <s>] [-- <arguments du jeu>]
//   --early   : injecte avant que le jeu ne démarre (processus suspendu).
//               Par défaut on attend la fenêtre du jeu.
//   --steamvr : utilise le runtime OpenXR 32 bits de SteamVR pour ce lancement seulement
//               (Steam Frame, Index...), sans changer le runtime par défaut de Windows.
//   --runtime : idem avec le manifeste OpenXR 32 bits (.json) d'un autre runtime.
//   --default-runtime : runtime OpenXR actif de Windows, quel qu'il soit.
//               Sans ces options : runtime actif de Windows, sauf si c'est celui de Meta,
//               dont la version 32 bits plante à la création de la session (constaté avec
//               Oculus 1.208, même hors du jeu : tools/xr_probe). SteamVR est alors choisi
//               s'il est installé ; il pilote aussi les casques Meta (Link).
//   --wait    : reste ouvert jusqu'à la fermeture du jeu (automatique si lancé par Steam).
//   Dossier du jeu : --game-dir, sinon le dossier de l'injecteur s'il contient le jeu, sinon
//   game_dir dans la section [Launch] de sr2_vr.ini (à côté de l'injecteur), sinon les
//   bibliothèques Steam, sinon le dossier Steam par défaut.
#include <windows.h>
#include <psapi.h>
#include <fcntl.h>
#include <io.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cwctype>
#include <iterator>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kDefaultGameDir[] = L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Saints Row 2";
constexpr wchar_t kSteamSubDir[] = L"\\steamapps\\common\\Saints Row 2";
// Manifeste OpenXR 32 bits de SteamVR, dans la bibliothèque Steam où il est installé
constexpr wchar_t kSteamVrSubDir[] = L"\\steamapps\\common\\SteamVR";
constexpr wchar_t kSteamVrRuntime[] = L"steamxr_win32.json";
constexpr wchar_t kExeName[] = L"SR2_pc.exe";
constexpr wchar_t kDllName[] = L"sr2_vr.dll";
constexpr wchar_t kIniName[] = L"sr2_vr.ini";

// Sorties de l'injecteur : console (si elle existe) et sr2_vr_injector.log à côté de lui
FILE* g_log = nullptr;

void Out(FILE* stream, const wchar_t* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vfwprintf(stream, fmt, args);
    va_end(args);
    if (g_log) {
        va_start(args, fmt);
        vfwprintf(g_log, fmt, args);
        va_end(args);
        fflush(g_log);
    }
}

void PrintLastError(const wchar_t* what)
{
    DWORD err = GetLastError();
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, err, 0, reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
    Out(stderr, L"[injector] ERREUR %s : %lu %s", what, err, msg ? msg : L"\n");
    if (msg)
        LocalFree(msg);
}

std::wstring DirOfSelf()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s = path;
    return s.substr(0, s.find_last_of(L'\\'));
}

std::wstring FullPath(const std::wstring& p)
{
    wchar_t buf[MAX_PATH]{};
    DWORD n = GetFullPathNameW(p.c_str(), MAX_PATH, buf, nullptr);
    return (n && n < MAX_PATH) ? std::wstring(buf) : p;
}

bool FileExists(const std::wstring& p)
{
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring FromUtf8(const std::string& s)
{
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1)
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

// Dossier d'installation de Steam (registre), "" si Steam n'est pas installé
std::wstring SteamDir()
{
    wchar_t path[MAX_PATH]{};
    DWORD size = sizeof(path);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, path,
                     &size) != ERROR_SUCCESS)
        return {};
    std::wstring dir = path;
    std::replace(dir.begin(), dir.end(), L'/', L'\\');
    return dir;
}

// Dossier `subDir` contenant `file` dans une des bibliothèques Steam : lignes
// « "path" "D:\\SteamLibrary" » de steamapps\libraryfolders.vdf. "" si absent.
std::wstring FindInSteamLibraries(const wchar_t* subDir, const wchar_t* file)
{
    const std::wstring steam = SteamDir();
    if (steam.empty())
        return {};
    std::vector<std::wstring> libraries{steam};
    FILE* f = nullptr;
    if (_wfopen_s(&f, (steam + L"\\steamapps\\libraryfolders.vdf").c_str(), L"rb") == 0 && f) {
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
                    ++i; // antislash doublé du format VDF
                value += text[i];
            }
            libraries.push_back(FromUtf8(value));
        }
        fclose(f);
    }
    for (const std::wstring& library : libraries) {
        const std::wstring dir = library + subDir;
        if (FileExists(dir + L"\\" + file))
            return dir;
    }
    return {};
}

// L'exécutable est-il bien un PE 32 bits (x86) ? Sinon l'injection ne peut pas marcher.
bool IsX86Executable(const std::wstring& path)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f)
        return false;
    IMAGE_DOS_HEADER dos{};
    DWORD signature = 0;
    IMAGE_FILE_HEADER header{};
    const bool ok = fread(&dos, sizeof(dos), 1, f) == 1 && dos.e_magic == IMAGE_DOS_SIGNATURE &&
                    fseek(f, dos.e_lfanew, SEEK_SET) == 0 && fread(&signature, sizeof(signature), 1, f) == 1 &&
                    signature == IMAGE_NT_SIGNATURE && fread(&header, sizeof(header), 1, f) == 1 &&
                    header.Machine == IMAGE_FILE_MACHINE_I386;
    fclose(f);
    return ok;
}

// Manifeste du runtime OpenXR actif pour un processus 32 bits (l'injecteur en est un : il
// lit la même vue du registre que le jeu). Le chargeur OpenXR lit HKCU avant HKLM.
std::wstring ActiveRuntime32()
{
    for (HKEY hive : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        wchar_t path[MAX_PATH * 2]{};
        DWORD size = sizeof(path);
        if (RegGetValueW(hive, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime",
                         RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, path, &size) == ERROR_SUCCESS &&
            path[0])
            return path;
    }
    return {};
}

bool IsMeta32Runtime(std::wstring path)
{
    std::transform(path.begin(), path.end(), path.begin(), towlower);
    return path.find(L"oculus_openxr_32") != std::wstring::npos;
}

// Fenêtre principale visible du processus (hors console de debug de Juiced)
struct WindowSearch {
    DWORD pid;
    bool found;
};

BOOL CALLBACK FindGameWindow(HWND window, LPARAM param)
{
    auto* search = reinterpret_cast<WindowSearch*>(param);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid != search->pid || !IsWindowVisible(window) || GetWindow(window, GW_OWNER))
        return TRUE;
    wchar_t cls[64]{};
    GetClassNameW(window, cls, 64);
    if (wcscmp(cls, L"ConsoleWindowClass") == 0)
        return TRUE;
    search->found = true;
    return FALSE;
}

bool HasGameWindow(DWORD pid)
{
    WindowSearch search{pid, false};
    EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

bool IsModuleLoaded(HANDLE process, const std::wstring& fullPath)
{
    HMODULE mods[1024];
    DWORD needed = 0;
    if (!EnumProcessModulesEx(process, mods, sizeof(mods), &needed, LIST_MODULES_ALL))
        return false;
    DWORD count = std::min<DWORD>(needed / sizeof(HMODULE), static_cast<DWORD>(std::size(mods)));
    for (DWORD i = 0; i < count; ++i) {
        wchar_t name[MAX_PATH]{};
        if (GetModuleFileNameExW(process, mods[i], name, MAX_PATH) && _wcsicmp(name, fullPath.c_str()) == 0)
            return true;
    }
    return false;
}

// Charge la DLL dans le processus cible via un thread distant sur LoadLibraryW
bool InjectDll(HANDLE process, const std::wstring& dllPath)
{
    const SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        PrintLastError(L"VirtualAllocEx");
        return false;
    }

    bool ok = false;
    if (!WriteProcessMemory(process, remote, dllPath.c_str(), bytes, nullptr)) {
        PrintLastError(L"WriteProcessMemory");
    } else {
        // kernel32 (32 bits) est chargé à la même adresse dans tous les processus 32 bits
        // d'une session
        auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW")));
        HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remote, 0, nullptr);
        if (!thread) {
            PrintLastError(L"CreateRemoteThread");
        } else {
            if (WaitForSingleObject(thread, 30000) != WAIT_OBJECT_0) {
                Out(stderr, L"[injector] ERREUR : LoadLibraryW distant n'a pas rendu la main en 30 s\n");
            } else {
                // En 32 bits le code de sortie est le HMODULE entier ; l'énumération confirme
                DWORD exitCode = 0;
                GetExitCodeThread(thread, &exitCode);
                ok = exitCode != 0 || IsModuleLoaded(process, dllPath);
                if (!ok)
                    Out(stderr, L"[injector] ERREUR : LoadLibraryW a échoué dans le jeu "
                                L"(DLL introuvable, mauvaise architecture ou dépendance manquante)\n");
            }
            CloseHandle(thread);
        }
    }
    // En cas de timeout le thread distant peut encore lire la chaîne : on ne libère pas
    if (ok)
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    return ok;
}

void Usage()
{
    fwprintf(stderr, L"Usage : sr2_vr_injector [--game-dir <dossier>] [--dll <chemin>] [--early]\n"
                     L"                        [--steamvr | --runtime <json> | --default-runtime]\n"
                     L"                        [--wait] [--timeout <s>] [-- <arguments du jeu>]\n");
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    // Accents : UTF-16 vers la console, UTF-8 quand la sortie est redirigée
    _setmode(_fileno(stdout), _isatty(_fileno(stdout)) ? _O_U16TEXT : _O_U8TEXT);
    _setmode(_fileno(stderr), _isatty(_fileno(stderr)) ? _O_U16TEXT : _O_U8TEXT);

    {
        const std::wstring logPath = DirOfSelf() + L"\\sr2_vr_injector.log";
        if (_wfopen_s(&g_log, logPath.c_str(), L"w, ccs=UTF-8") != 0)
            g_log = nullptr;
    }
    std::wstring gameDir;
    std::wstring dllPath = DirOfSelf() + L"\\" + kDllName;
    std::wstring gameArgs;
    bool early = false;
    bool wait = false;
    bool steamvr = false;
    bool defaultRuntime = false;
    std::wstring runtimeJson;
    DWORD timeoutSec = 120;

    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--game-dir" && i + 1 < argc) {
            gameDir = argv[++i];
        } else if (a == L"--dll" && i + 1 < argc) {
            dllPath = argv[++i];
        } else if (a == L"--timeout" && i + 1 < argc) {
            timeoutSec = static_cast<DWORD>(_wtoi(argv[++i]));
        } else if (a == L"--early") {
            early = true;
        } else if (a == L"--wait") {
            wait = true;
        } else if (a == L"--steamvr") {
            steamvr = true;
        } else if (a == L"--runtime" && i + 1 < argc) {
            runtimeJson = argv[++i];
        } else if (a == L"--default-runtime") {
            defaultRuntime = true;
        } else if (a == L"--") {
            for (++i; i < argc; ++i) {
                if (!gameArgs.empty())
                    gameArgs += L' ';
                gameArgs += argv[i];
            }
        } else if (a == L"-h" || a == L"--help") {
            Usage();
            return 0;
        } else {
            Out(stderr, L"[injector] Argument inconnu : %s\n", a.c_str());
            Usage();
            return 2;
        }
    }

    const wchar_t* source = L"--game-dir";
    if (gameDir.empty()) {
        wchar_t fromIni[MAX_PATH]{};
        GetPrivateProfileStringW(L"Launch", L"game_dir", L"", fromIni, MAX_PATH,
                                 (DirOfSelf() + L"\\" + kIniName).c_str());
        if (FileExists(DirOfSelf() + L"\\" + kExeName)) {
            gameDir = DirOfSelf();
            source = L"dossier de l'injecteur";
        } else if (fromIni[0]) {
            gameDir = fromIni;
            source = L"sr2_vr.ini";
        } else if (gameDir = FindInSteamLibraries(kSteamSubDir, kExeName); !gameDir.empty()) {
            source = L"bibliothèque Steam";
        } else {
            gameDir = kDefaultGameDir;
            source = L"dossier Steam par défaut";
        }
    }

    dllPath = FullPath(dllPath);
    gameDir = FullPath(gameDir);
    const std::wstring exePath = gameDir + L"\\" + kExeName;

    Out(stdout, L"[injector] SR2VR %hs\n", SR2VR_VERSION);
    Out(stdout, L"[injector] Jeu       : %s (%s)\n", exePath.c_str(), source);
    Out(stdout, L"[injector] Arguments : %s\n", gameArgs.empty() ? L"(aucun)" : gameArgs.c_str());
    Out(stdout, L"[injector] DLL       : %s\n", dllPath.c_str());
    Out(stdout, L"[injector] Mode      : %s\n", early ? L"précoce (processus suspendu)" : L"après création de la fenêtre");

    if (!FileExists(exePath)) {
        Out(stderr, L"[injector] ERREUR : jeu introuvable. Indiquer son dossier avec --game-dir ou dans "
                    L"sr2_vr.ini :\n[Launch]\ngame_dir=<dossier de SR2_pc.exe>\n");
        return 1;
    }
    if (!IsX86Executable(exePath)) {
        Out(stderr, L"[injector] ERREUR : %s n'est pas un exécutable 32 bits\n", exePath.c_str());
        return 1;
    }
    if (!FileExists(dllPath)) {
        Out(stderr, L"[injector] ERREUR : DLL introuvable : %s\n", dllPath.c_str());
        return 1;
    }
    if (!FileExists(gameDir + L"\\juiced.ini"))
        Out(stdout, L"[injector] ATTENTION : juiced.ini absent, Juiced Patch ne semble pas installé\n");

    // Runtime OpenXR de ce lancement : variable XR_RUNTIME_JSON héritée par le jeu (le
    // chargeur OpenXR la préfère au runtime actif de Windows)
    const std::wstring active = ActiveRuntime32();
    if (defaultRuntime && runtimeJson.empty()) {
        // Rendu explicite : la DLL refuse sinon le runtime 32 bits de Meta
        runtimeJson = active;
        if (active.empty())
            Out(stdout, L"[injector] ATTENTION : aucun runtime OpenXR 32 bits actif dans Windows\n");
    } else if (!steamvr && runtimeJson.empty() && IsMeta32Runtime(active)) {
        if (FindInSteamLibraries(kSteamVrSubDir, kSteamVrRuntime).empty()) {
            Out(stdout, L"[injector] ATTENTION : le runtime actif est celui de Meta, dont la version 32 bits plante ;\n"
                        L"[injector]             SteamVR est introuvable : le jeu sera lance sans VR\n");
        } else {
            steamvr = true;
            Out(stdout, L"[injector] Runtime actif : Meta, dont la version 32 bits plante -> SteamVR a la place\n"
                        L"[injector]             (--default-runtime pour garder celui de Windows)\n");
        }
    }
    if (steamvr && runtimeJson.empty()) {
        const std::wstring dir = FindInSteamLibraries(kSteamVrSubDir, kSteamVrRuntime);
        if (dir.empty()) {
            Out(stderr, L"[injector] ERREUR : SteamVR (%s) introuvable dans les bibliothèques Steam\n", kSteamVrRuntime);
            return 1;
        }
        runtimeJson = dir + L"\\" + kSteamVrRuntime;
    }
    if (!runtimeJson.empty()) {
        runtimeJson = FullPath(runtimeJson);
        if (!FileExists(runtimeJson)) {
            Out(stderr, L"[injector] ERREUR : runtime OpenXR introuvable : %s\n", runtimeJson.c_str());
            return 1;
        }
        SetEnvironmentVariableW(L"XR_RUNTIME_JSON", runtimeJson.c_str());
        Out(stdout, L"[injector] OpenXR    : %s\n", runtimeJson.c_str());
    } else {
        Out(stdout, L"[injector] OpenXR    : runtime actif de Windows\n");
    }

    // CreateProcessW peut modifier la ligne de commande : il lui faut un tampon modifiable
    std::wstring cmdLine = L"\"" + exePath + L"\"";
    if (!gameArgs.empty())
        cmdLine += L" " + gameArgs;
    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exePath.c_str(), cmdBuf.data(), nullptr, nullptr, FALSE, early ? CREATE_SUSPENDED : 0,
                        nullptr, gameDir.c_str(), &si, &pi)) {
        PrintLastError(L"CreateProcessW");
        return 1;
    }
    Out(stdout, L"[injector] Processus lancé, PID %lu\n", pi.dwProcessId);

    bool ok = false;
    if (early) {
        ok = InjectDll(pi.hProcess, dllPath);
        ResumeThread(pi.hThread);
    } else {
        // d3d9.dll est un import de l'exe (présent dès le départ) : on attend plutôt la
        // fenêtre du jeu, ou la fermeture du processus
        const ULONGLONG deadline = GetTickCount64() + timeoutSec * 1000ULL;
        bool window = false, closed = false;
        while (!window && !closed && GetTickCount64() < deadline) {
            if (WaitForSingleObject(pi.hProcess, 250) == WAIT_OBJECT_0) {
                DWORD code = 0;
                GetExitCodeProcess(pi.hProcess, &code);
                Out(stderr, L"[injector] ERREUR : le jeu s'est fermé avant l'injection (code %lu)\n", code);
                closed = true;
            } else {
                window = HasGameWindow(pi.dwProcessId);
            }
        }
        if (window) {
            Out(stdout, L"[injector] Fenêtre du jeu détectée\n");
            ok = InjectDll(pi.hProcess, dllPath);
        } else if (!closed) {
            Out(stderr, L"[injector] Aucune fenêtre après %lu s, injection quand même\n", timeoutSec);
            ok = InjectDll(pi.hProcess, dllPath);
        }
    }

    Out(stdout, ok ? L"[injector] Injection réussie. Log : %s\\sr2_vr.log\n" : L"[injector] Injection échouée.\n",
        dllPath.substr(0, dllPath.find_last_of(L'\\')).c_str());

    // Lancé par Steam (options de lancement) : Steam suit la partie tant que l'injecteur
    // tourne, il attend donc la fin du jeu sans qu'il faille --wait
    wchar_t steamApp[32]{};
    if (GetEnvironmentVariableW(L"SteamAppId", steamApp, 32) || GetEnvironmentVariableW(L"SteamGameId", steamApp, 32))
        wait = true;
    if (wait && WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT) {
        Out(stdout, L"[injector] En attente de la fermeture du jeu (--wait)\n");
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        Out(stdout, L"[injector] Jeu fermé (code %lu)\n", code);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (g_log)
        fclose(g_log);
    return ok ? 0 : 1;
}
