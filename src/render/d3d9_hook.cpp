// Hook du renderer Direct3D 9 de Saints Row 2.
//
// Aucune adresse codée en dur : on crée un device D3D9 factice (et un device D3D9Ex) sur
// une fenêtre cachée, on lit leurs vtables pour obtenir les adresses de Present/Reset dans
// d3d9.dll, puis on les détourne avec MinHook. Les index de vtable sont fixés par l'ABI COM
// de d3d9.h. Le device du jeu existe déjà à l'injection : ses appels passent par ces mêmes
// fonctions de d3d9.dll.
//
// On compte les images, on décrit le rendu (thread, device, cibles de rendu, scènes par
// image) et on copie le back buffer pour le casque (render/frame_capture), sans rien
// modifier au rendu du jeu.
#include "render/d3d9_hook.h"
#include "render/frame_capture.h"
#include "util/hook.h"
#include "util/log.h"

#include <d3d9.h>
#include <intrin.h>

#include <atomic>
#include <cstdint>
#include <vector>

namespace render {
namespace {

// Index dans les vtables (ordre des méthodes dans d3d9.h)
constexpr int kDeviceReset = 16;
constexpr int kDevicePresent = 17;
constexpr int kDeviceSetRenderTarget = 37;
constexpr int kDeviceBeginScene = 41;
constexpr int kDeviceEndScene = 42;
constexpr int kDeviceClear = 43;
constexpr int kDeviceExPresentEx = 121;
constexpr int kDeviceExResetEx = 132;
constexpr int kSwapChainPresent = 3;

// Deux variantes possibles : 0 = device D3D9, 1 = device D3D9Ex. Leurs méthodes peuvent
// pointer vers des fonctions différentes, chacune a donc sa trampoline.
constexpr int kVariants = 2;
const char* const kVariantName[kVariants] = {"D3D9", "D3D9Ex"};

using DevicePresentFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND,
                                                     const RGNDATA*);
using DeviceResetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using SetRenderTargetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
using SceneFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*);
using ClearFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
using SwapChainPresentFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND,
                                                        const RGNDATA*, DWORD);
using PresentExFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, const RECT*, const RECT*, HWND,
                                                 const RGNDATA*, DWORD);
using ResetExFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*);

DevicePresentFn g_devicePresent[kVariants]{};
DeviceResetFn g_deviceReset[kVariants]{};
SetRenderTargetFn g_setRenderTarget[kVariants]{};
SceneFn g_beginScene[kVariants]{};
SceneFn g_endScene[kVariants]{};
ClearFn g_clear[kVariants]{};
SwapChainPresentFn g_swapChainPresent[kVariants]{};
PresentExFn g_presentEx = nullptr;
ResetExFn g_resetEx = nullptr;

std::vector<void*> g_targets; // fonctions détournées par MinHook

// Indice tiré des sources de Juiced (Render3D.cpp, bitmap.cpp) : variable globale de l'exe
// qui contient le device du jeu. Sert seulement à recouper ce que le hook observe, et
// uniquement si l'exe est chargé à sa base d'origine.
constexpr uintptr_t kExeBase = 0x00400000;
constexpr uintptr_t kJuicedDeviceGlobal = 0x0252A2D0;

// --- Compteurs ----------------------------------------------------------------------

constexpr ULONGLONG kStatsIntervalMs = 10000;

std::atomic<unsigned long long> g_frames{0};
std::atomic<IDirect3DDevice9*> g_lastDevice{nullptr};
std::atomic<DWORD> g_presentThread{0};
// Appels du jeu depuis le dernier relevé (tous threads)
std::atomic<unsigned> g_beginScenes{0}, g_endScenes{0}, g_clears{0}, g_setRenderTargets{0};
unsigned long long g_framesAtLastStats = 0;
ULONGLONG g_lastStatsTick = 0;
int g_statsLogged = 0;

// Present du device peut appeler en interne le Present de la swap chain : on ne compte
// que l'appel le plus externe.
thread_local int t_presentDepth = 0;

struct PresentScope {
    bool outermost;
    PresentScope() : outermost(t_presentDepth++ == 0) {}
    ~PresentScope() { --t_presentDepth; }
};

const char* SwapEffectName(D3DSWAPEFFECT effect)
{
    switch (effect) {
    case D3DSWAPEFFECT_DISCARD: return "DISCARD";
    case D3DSWAPEFFECT_FLIP: return "FLIP";
    case D3DSWAPEFFECT_COPY: return "COPY";
    case D3DSWAPEFFECT_OVERLAY: return "OVERLAY";
    case D3DSWAPEFFECT_FLIPEX: return "FLIPEX";
    default: return "?";
    }
}

void LogPresentParameters(const char* title, const D3DPRESENT_PARAMETERS& pp)
{
    LOG_RENDER("  %s : %ux%u, format %d, %s, %u buffer(s), multisample %d (qualite %lu)", title, pp.BackBufferWidth,
               pp.BackBufferHeight, pp.BackBufferFormat, pp.Windowed ? "fenetre" : "plein ecran", pp.BackBufferCount,
               pp.MultiSampleType, pp.MultiSampleQuality);
    LOG_RENDER("    swap %s, intervalle 0x%X, drapeaux 0x%lX, profondeur auto %d (format %d), fenetre %p",
               SwapEffectName(pp.SwapEffect), pp.PresentationInterval, pp.Flags, pp.EnableAutoDepthStencil,
               pp.AutoDepthStencilFormat, static_cast<void*>(pp.hDeviceWindow));
}

void LogDeviceInfo(IDirect3DDevice9* device)
{
    IDirect3DDevice9Ex* ex = nullptr;
    const bool isEx = SUCCEEDED(device->QueryInterface(IID_IDirect3DDevice9Ex, reinterpret_cast<void**>(&ex)));
    if (ex)
        ex->Release();
    LOG_RENDER("device du jeu : %p (%s), table virtuelle %s", static_cast<void*>(device), isEx ? "D3D9Ex" : "D3D9",
               hook::Describe(*reinterpret_cast<void**>(device)).c_str());

    D3DDEVICE_CREATION_PARAMETERS creation{};
    if (SUCCEEDED(device->GetCreationParameters(&creation))) {
        const DWORD f = creation.BehaviorFlags;
        LOG_RENDER("  creation : adaptateur %u, type %d, drapeaux 0x%lX (%s%s%s%s), fenetre de focus %p",
                   creation.AdapterOrdinal, creation.DeviceType, f,
                   f & D3DCREATE_HARDWARE_VERTEXPROCESSING ? "sommets materiel " : "",
                   f & D3DCREATE_SOFTWARE_VERTEXPROCESSING ? "sommets logiciel " : "",
                   f & D3DCREATE_PUREDEVICE ? "PUREDEVICE " : "",
                   f & D3DCREATE_MULTITHREADED ? "MULTITHREADED" : "non MULTITHREADED",
                   static_cast<void*>(creation.hFocusWindow));
    }

    IDirect3DSwapChain9* sc = nullptr;
    D3DPRESENT_PARAMETERS pp{};
    if (SUCCEEDED(device->GetSwapChain(0, &sc)) && sc) {
        if (SUCCEEDED(sc->GetPresentParameters(&pp))) {
            LogPresentParameters("back buffer", pp);
            const DWORD windowThread = GetWindowThreadProcessId(pp.hDeviceWindow, nullptr);
            LOG_RENDER("  thread du Present %lu, thread de la fenetre %lu (%s)", GetCurrentThreadId(), windowThread,
                       windowThread == GetCurrentThreadId() ? "le meme" : "thread de rendu distinct");
        }
        sc->Release();
    }
    LOG_RENDER("  swap chains : %u", device->GetNumberOfSwapChains());

    // Recoupement avec l'adresse connue de Juiced
    if (reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) == kExeBase) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<void*>(kJuicedDeviceGlobal), &info, sizeof(info)) &&
            info.State == MEM_COMMIT && !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
            auto* fromGlobal = *reinterpret_cast<IDirect3DDevice9**>(kJuicedDeviceGlobal);
            LOG_RENDER("  indice Juiced : *0x%08lX = %p (%s)", static_cast<unsigned long>(kJuicedDeviceGlobal),
                       static_cast<void*>(fromGlobal),
                       fromGlobal == device ? "c'est bien le device du Present" : "DIFFERENT du device du Present");
        }
    }
}

void OnPresent(IDirect3DDevice9* device, const char* via, void* caller)
{
    // Le back buffer contient encore l'image finie : copie pour le casque si demandée
    CaptureFrame(device);

    const unsigned long long n = ++g_frames;
    const ULONGLONG now = GetTickCount64();

    if (device && g_lastDevice.exchange(device) != device) {
        LOG_RENDER("premier Present sur ce device (via %s, image %llu, thread %lu)", via, n, GetCurrentThreadId());
        LOG_RENDER("  appele depuis %s", hook::Describe(caller).c_str());
        LogDeviceInfo(device);
    }
    const DWORD thread = GetCurrentThreadId();
    const DWORD previous = g_presentThread.exchange(thread);
    if (previous && previous != thread)
        LOG_RENDER("le Present change de thread : %lu -> %lu (image %llu)", previous, thread, n);

    if (n == 1) {
        g_lastStatsTick = now;
        g_framesAtLastStats = 1;
        g_beginScenes = g_endScenes = g_clears = g_setRenderTargets = 0;
        return;
    }
    if (now - g_lastStatsTick >= kStatsIntervalMs) {
        const double frames = static_cast<double>(n - g_framesAtLastStats);
        const double fps = frames * 1000.0 / static_cast<double>(now - g_lastStatsTick);
        const unsigned begins = g_beginScenes.exchange(0), ends = g_endScenes.exchange(0);
        const unsigned clears = g_clears.exchange(0), targets = g_setRenderTargets.exchange(0);
        // Relevé toutes les 10 s pendant 5 min, puis toutes les minutes
        if (g_statsLogged < 30 || g_statsLogged % 6 == 0)
            LOG_RENDER("images : %llu au total, %.1f par seconde ; par image : %.1f BeginScene, %.1f EndScene, "
                       "%.1f Clear, %.1f SetRenderTarget",
                       n, fps, begins / frames, ends / frames, clears / frames, targets / frames);
        ++g_statsLogged;
        g_lastStatsTick = now;
        g_framesAtLastStats = n;
    }
}

// --- Détours ------------------------------------------------------------------------

template <int V>
HRESULT STDMETHODCALLTYPE HkDevicePresent(IDirect3DDevice9* self, const RECT* src, const RECT* dst, HWND wnd,
                                          const RGNDATA* dirty)
{
    PresentScope scope;
    if (scope.outermost)
        OnPresent(self, "IDirect3DDevice9::Present", _ReturnAddress());
    return g_devicePresent[V](self, src, dst, wnd, dirty);
}

template <int V>
HRESULT STDMETHODCALLTYPE HkSwapChainPresent(IDirect3DSwapChain9* self, const RECT* src, const RECT* dst, HWND wnd,
                                             const RGNDATA* dirty, DWORD flags)
{
    PresentScope scope;
    if (scope.outermost) {
        IDirect3DDevice9* device = nullptr;
        self->GetDevice(&device);
        OnPresent(device, "IDirect3DSwapChain9::Present", _ReturnAddress());
        if (device)
            device->Release();
    }
    return g_swapChainPresent[V](self, src, dst, wnd, dirty, flags);
}

HRESULT STDMETHODCALLTYPE HkPresentEx(IDirect3DDevice9Ex* self, const RECT* src, const RECT* dst, HWND wnd,
                                      const RGNDATA* dirty, DWORD flags)
{
    PresentScope scope;
    if (scope.outermost)
        OnPresent(self, "IDirect3DDevice9Ex::PresentEx", _ReturnAddress());
    return g_presentEx(self, src, dst, wnd, dirty, flags);
}

template <int V>
HRESULT STDMETHODCALLTYPE HkSetRenderTarget(IDirect3DDevice9* self, DWORD index, IDirect3DSurface9* surface)
{
    ++g_setRenderTargets;
    return g_setRenderTarget[V](self, index, surface);
}

template <int V>
HRESULT STDMETHODCALLTYPE HkBeginScene(IDirect3DDevice9* self)
{
    ++g_beginScenes;
    return g_beginScene[V](self);
}

template <int V>
HRESULT STDMETHODCALLTYPE HkEndScene(IDirect3DDevice9* self)
{
    ++g_endScenes;
    return g_endScene[V](self);
}

template <int V>
HRESULT STDMETHODCALLTYPE HkClear(IDirect3DDevice9* self, DWORD count, const D3DRECT* rects, DWORD flags,
                                  D3DCOLOR color, float z, DWORD stencil)
{
    ++g_clears;
    return g_clear[V](self, count, rects, flags, color, z, stencil);
}

template <int V>
HRESULT STDMETHODCALLTYPE HkDeviceReset(IDirect3DDevice9* self, D3DPRESENT_PARAMETERS* pp)
{
    LOG_RENDER("Reset (%s) demande par %s, thread %lu", kVariantName[V], hook::Describe(_ReturnAddress()).c_str(),
               GetCurrentThreadId());
    if (pp)
        LogPresentParameters("demande", *pp);
    const HRESULT hr = g_deviceReset[V](self, pp);
    LOG_RENDER("Reset -> 0x%08lX", static_cast<unsigned long>(hr));
    return hr;
}

HRESULT STDMETHODCALLTYPE HkResetEx(IDirect3DDevice9Ex* self, D3DPRESENT_PARAMETERS* pp, D3DDISPLAYMODEEX* mode)
{
    LOG_RENDER("ResetEx demande par %s, thread %lu", hook::Describe(_ReturnAddress()).c_str(), GetCurrentThreadId());
    if (pp)
        LogPresentParameters("demande", *pp);
    const HRESULT hr = g_resetEx(self, pp, mode);
    LOG_RENDER("ResetEx -> 0x%08lX", static_cast<unsigned long>(hr));
    return hr;
}

// --- Installation -------------------------------------------------------------------

// Une méthode partagée entre D3D9 et D3D9Ex n'est détournée qu'une fois
template <typename Fn>
bool Hook(void* object, int index, Fn detour, Fn* original, const char* name)
{
    return hook::HookMethod(object, index, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original), name,
                            "[render]", g_targets);
}

HWND CreateDummyWindow()
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SR2VR_DummyD3D";
    RegisterClassExW(&wc);
    return CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 16, 16, nullptr, nullptr, wc.hInstance,
                           nullptr);
}

D3DPRESENT_PARAMETERS DummyParams(HWND wnd)
{
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.BackBufferWidth = 16;
    pp.BackBufferHeight = 16;
    pp.hDeviceWindow = wnd;
    return pp;
}

constexpr DWORD kDummyFlags = D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_DISABLE_DRIVER_MANAGEMENT;

// Hooke les méthodes d'un device factice. V = variante (D3D9 / D3D9Ex)
template <int V>
int HookDevice(IDirect3DDevice9* device)
{
    int ok = 0;
    ok += Hook(device, kDevicePresent, &HkDevicePresent<V>, &g_devicePresent[V], "IDirect3DDevice9::Present");
    ok += Hook(device, kDeviceReset, &HkDeviceReset<V>, &g_deviceReset[V], "IDirect3DDevice9::Reset");
    ok += Hook(device, kDeviceBeginScene, &HkBeginScene<V>, &g_beginScene[V], "IDirect3DDevice9::BeginScene");
    ok += Hook(device, kDeviceEndScene, &HkEndScene<V>, &g_endScene[V], "IDirect3DDevice9::EndScene");
    ok += Hook(device, kDeviceClear, &HkClear<V>, &g_clear[V], "IDirect3DDevice9::Clear");
    ok += Hook(device, kDeviceSetRenderTarget, &HkSetRenderTarget<V>, &g_setRenderTarget[V],
               "IDirect3DDevice9::SetRenderTarget");

    IDirect3DSwapChain9* sc = nullptr;
    if (SUCCEEDED(device->GetSwapChain(0, &sc)) && sc) {
        ok += Hook(sc, kSwapChainPresent, &HkSwapChainPresent<V>, &g_swapChainPresent[V],
                   "IDirect3DSwapChain9::Present");
        sc->Release();
    }
    return ok;
}

} // namespace

bool InstallD3D9Hooks()
{
    HMODULE d3d9 = GetModuleHandleW(L"d3d9.dll");
    if (!d3d9) {
        LOG_RENDER("ERREUR : d3d9.dll n'est pas chargee, hooks desactives");
        return false;
    }
    using Create9Fn = IDirect3D9*(WINAPI*)(UINT);
    using Create9ExFn = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
    auto create9 = reinterpret_cast<Create9Fn>(reinterpret_cast<void*>(GetProcAddress(d3d9, "Direct3DCreate9")));
    auto create9Ex = reinterpret_cast<Create9ExFn>(reinterpret_cast<void*>(GetProcAddress(d3d9, "Direct3DCreate9Ex")));
    if (!create9) {
        LOG_RENDER("ERREUR : Direct3DCreate9 introuvable dans d3d9.dll, hooks desactives");
        return false;
    }

    HWND wnd = CreateDummyWindow();
    if (!wnd) {
        LOG_RENDER("ERREUR : fenetre factice non creee (%lu), hooks desactives", GetLastError());
        return false;
    }

    // Juiced détourne IDirect3D9::CreateDevice (il réessaie en fenêtré si la création
    // échoue) : nos créations de devices factices passent par lui et apparaissent dans son
    // debug.txt (« CreateDevice succeeded »). Sans conséquence.

    // Device D3D9 classique. HAL d'abord, NULLREF si le GPU est déjà pris en exclusif.
    if (IDirect3D9* d3d = create9(D3D_SDK_VERSION)) {
        D3DPRESENT_PARAMETERS pp = DummyParams(wnd);
        IDirect3DDevice9* dev = nullptr;
        HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, kDummyFlags, &pp, &dev);
        if (FAILED(hr)) {
            LOG_RENDER("device factice HAL : echec 0x%08lX, essai NULLREF", static_cast<unsigned long>(hr));
            pp = DummyParams(wnd);
            hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_NULLREF, wnd, kDummyFlags, &pp, &dev);
        }
        if (SUCCEEDED(hr) && dev) {
            LOG_RENDER("device factice D3D9 cree, installation des hooks :");
            HookDevice<0>(dev);
            dev->Release();
        } else {
            LOG_RENDER("ERREUR : device factice D3D9 non cree (0x%08lX)", static_cast<unsigned long>(hr));
        }
        d3d->Release();
    }

    // Device D3D9Ex, au cas où le jeu (ou un remplaçant de d3d9.dll) l'utilise
    IDirect3D9Ex* d3dEx = nullptr;
    if (create9Ex && SUCCEEDED(create9Ex(D3D_SDK_VERSION, &d3dEx)) && d3dEx) {
        D3DPRESENT_PARAMETERS pp = DummyParams(wnd);
        IDirect3DDevice9Ex* dev = nullptr;
        const HRESULT hr =
            d3dEx->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, kDummyFlags, &pp, nullptr, &dev);
        if (SUCCEEDED(hr) && dev) {
            LOG_RENDER("device factice D3D9Ex cree, installation des hooks :");
            HookDevice<1>(dev);
            Hook(dev, kDeviceExPresentEx, &HkPresentEx, &g_presentEx, "IDirect3DDevice9Ex::PresentEx");
            Hook(dev, kDeviceExResetEx, &HkResetEx, &g_resetEx, "IDirect3DDevice9Ex::ResetEx");
            dev->Release();
        } else {
            LOG_RENDER("device factice D3D9Ex non cree (0x%08lX), hooks Ex ignores", static_cast<unsigned long>(hr));
        }
        d3dEx->Release();
    } else {
        LOG_RENDER("Direct3DCreate9Ex indisponible, hooks Ex ignores");
    }

    DestroyWindow(wnd);

    if (g_targets.empty()) {
        LOG_RENDER("ERREUR : aucun hook installe, rendu non intercepte");
        return false;
    }
    if (!hook::ApplyQueued("[render]"))
        return false;
    LOG_RENDER("%zu fonction(s) detournee(s) et activee(s), en attente du premier Present du jeu", g_targets.size());
    return true;
}

unsigned long long PresentCount()
{
    return g_frames;
}

void RemoveD3D9Hooks()
{
    if (g_targets.empty())
        return;
    hook::RemoveAll(g_targets);
    LOG_RENDER("hooks retires (%llu images vues)", g_frames.load());
}

} // namespace render
