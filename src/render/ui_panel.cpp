#include "render/ui_panel.h"
#include "engine/camera_hook.h"
#include "render/panel_image.h"
#include "util/config.h"
#include "util/log.h"

#include <MinHook.h>
#include <d3d9.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace render::ui_panel {
namespace {

// Méthodes du device du jeu. C'est un PUREDEVICE : sa table de méthodes n'est pas celle d'un
// device ordinaire, on la lit sur l'objet lui-même, à la première image.
constexpr int kSetRenderTarget = 37, kSetRenderState = 57, kSetTexture = 65, kDrawPrimitive = 81, kDrawIndexedPrimitive = 82,
              kDrawPrimitiveUP = 83, kDrawIndexedPrimitiveUP = 84;
using SetRenderTargetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
using SetTextureFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
using DrawPrimitiveFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
using DrawIndexedFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using DrawUpFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
using DrawIndexedUpFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*,
                                                    D3DFORMAT, const void*, UINT);
using SetRenderStateFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
SetRenderStateFn g_setRenderState = nullptr;
SetRenderTargetFn g_setRenderTarget = nullptr;
SetTextureFn g_setTexture = nullptr;
DrawPrimitiveFn g_drawPrimitive = nullptr;
DrawIndexedFn g_drawIndexed = nullptr;
DrawUpFn g_drawUp = nullptr;
DrawIndexedUpFn g_drawIndexedUp = nullptr;
void* g_hooked[7]{};
bool g_hooks = false, g_hooksFailed = false;


std::atomic<int> g_mode{1};
std::atomic<bool> g_active{false};
ULONGLONG g_settingsTick = 0, g_activeTick = 0, g_readTick = 0;

// Tout ce qui suit : thread qui appelle Direct3D
DWORD g_thread = 0;
IDirect3DSurface9* g_panel = nullptr;  // cible de rendu, taille du back buffer
IDirect3DSurface9* g_sysmem = nullptr; // sa copie lisible
UINT g_width = 0, g_height = 0;
bool g_failed = false;

IDirect3DBaseTexture9* g_texture0 = nullptr; // texture de l'étage 0, suivie ici (PUREDEVICE : pas de GetTexture)
// Image en cours
bool g_frameKnown = false;   // l'œil de l'image en cours a été regardé
bool g_frameLeft = false;    // image de l'œil gauche d'une paire
IDirect3DSurface9* g_back = nullptr; // back buffer (comparaison de pointeurs seulement, sans référence gardée)
bool g_onBack = false;       // la cible courante est le back buffer
int g_segmentDraws = 0;      // dessins depuis que le back buffer est la cible
bool g_composite = false;    // ce passage dans le back buffer a commencé par la recopie de la scène
bool g_redirecting = false;  // la cible est notre calque jusqu'au Present
bool g_gameWantsBack = true; // pendant la redirection : le jeu croit dessiner dans le back buffer
int g_panelDraws = 0;
// États de rendu posés par le jeu, suivis ici (PUREDEVICE : pas de GetRenderState)
DWORD g_states[256]{};
bool g_stateKnown[256]{};
// Mise au point ([Hud] panel_trace=1) : états des dessins redirigés d'une image
int g_traceWanted = 0;
bool g_tracing = false;
char g_traceKey[96]{};
int g_traceRun = 0, g_traceLines = 0;
unsigned g_skipped = 0;
ULONGLONG g_readInterval = 66;

DWORD State(D3DRENDERSTATETYPE state, DWORD fallback)
{
    return g_stateKnown[state] ? g_states[state] : fallback;
}

// Le mélange en place multiplie-t-il ce qui est déjà dans la cible (source x destination) ?
bool Multiplies()
{
    if (!State(D3DRS_ALPHABLENDENABLE, FALSE))
        return false;
    const DWORD src = State(D3DRS_SRCBLEND, D3DBLEND_ONE), dst = State(D3DRS_DESTBLEND, D3DBLEND_ZERO);
    return (src == D3DBLEND_DESTCOLOR && dst == D3DBLEND_ZERO) || (src == D3DBLEND_ZERO && dst == D3DBLEND_SRCCOLOR) ||
           (src == D3DBLEND_DESTCOLOR && dst == D3DBLEND_SRCCOLOR);
}

// L'alpha du calque doit dire ce que l'interface couvre, quel que soit le mélange des couleurs
// choisi par le jeu pour ce dessin :
// - mélange ordinaire : « par-dessus » (alpha = source + destination x (1 - source)) ;
// - lumière ajoutée (destination gardée entière) ou dessin découpé par l'alpha déjà en place
//   (carte ronde) : rien n'est couvert de plus, l'alpha ne change pas.
int g_alphaRule = -1;
void SetAlphaRule(IDirect3DDevice9* device)
{
    const DWORD src = State(D3DRS_SRCBLEND, D3DBLEND_ONE), dst = State(D3DRS_DESTBLEND, D3DBLEND_ZERO);
    const bool keep = State(D3DRS_ALPHABLENDENABLE, FALSE) &&
                      (dst == D3DBLEND_ONE || src == D3DBLEND_DESTALPHA || src == D3DBLEND_INVDESTALPHA);
    if (g_alphaRule == static_cast<int>(keep))
        return;
    g_alphaRule = keep;
    g_setRenderState(device, D3DRS_SRCBLENDALPHA, keep ? D3DBLEND_ZERO : D3DBLEND_ONE);
    g_setRenderState(device, D3DRS_DESTBLENDALPHA, keep ? D3DBLEND_ONE : D3DBLEND_INVSRCALPHA);
}

void TraceFlush()
{
    if (g_traceRun && g_traceLines < 80) {
        LOG_RENDER("  calque, trace : %d dessin(s) %s", g_traceRun, g_traceKey);
        ++g_traceLines;
    }
    g_traceRun = 0;
}

void TraceDraw(UINT primitives)
{
    D3DSURFACE_DESC desc{};
    if (g_texture0 && g_texture0->GetType() == D3DRTYPE_TEXTURE)
        static_cast<IDirect3DTexture9*>(g_texture0)->GetLevelDesc(0, &desc);
    char key[96];
    snprintf(key, sizeof(key), "ecriture %lX, melange %lu (%lu/%lu), test alpha %lu, z %lu, texture %ux%u, %s", 
             State(D3DRS_COLORWRITEENABLE, 0xF), State(D3DRS_ALPHABLENDENABLE, 0), State(D3DRS_SRCBLEND, 0),
             State(D3DRS_DESTBLEND, 0), State(D3DRS_ALPHATESTENABLE, 0), State(D3DRS_ZENABLE, 0), desc.Width, desc.Height,
             primitives <= 2 ? "2 triangles au plus" : "plus de 2 triangles");
    if (g_traceRun && strcmp(key, g_traceKey))
        TraceFlush();
    memcpy(g_traceKey, key, sizeof(key));
    ++g_traceRun;
}

unsigned g_logged = 0;

double NowMs()
{
    static LARGE_INTEGER frequency{};
    if (!frequency.QuadPart)
        QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
}

void ReleaseSurfaces()
{
    if (g_panel)
        g_panel->Release();
    if (g_sysmem)
        g_sysmem->Release();
    g_panel = g_sysmem = nullptr;
    g_width = g_height = 0;
}

bool EnsureSurfaces(IDirect3DDevice9* device)
{
    D3DSURFACE_DESC like{};
    if (!g_back || FAILED(g_back->GetDesc(&like)))
        return false;
    if (g_panel && g_width == like.Width && g_height == like.Height)
        return true;
    if (g_failed)
        return false;
    ReleaseSurfaces();
    HRESULT hr = device->CreateRenderTarget(like.Width, like.Height, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE,
                                            &g_panel, nullptr);
    if (SUCCEEDED(hr))
        hr = device->CreateOffscreenPlainSurface(like.Width, like.Height, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &g_sysmem,
                                                 nullptr);
    if (FAILED(hr) || like.MultiSampleType != D3DMULTISAMPLE_NONE) {
        LOG_RENDER("calque de l'interface : surfaces %ux%u impossibles (0x%08lX, multisample %d), l'interface reste "
                   "dans l'image de l'oeil gauche",
                   like.Width, like.Height, static_cast<unsigned long>(hr), static_cast<int>(like.MultiSampleType));
        ReleaseSurfaces();
        g_failed = true;
        return false;
    }
    g_width = like.Width;
    g_height = like.Height;
    LOG_RENDER("calque de l'interface : cible %ux%u creee", g_width, g_height);
    return true;
}

// La texture de l'étage 0 est-elle une cible de rendu de la taille de l'écran (la scène finie) ?
bool SceneTexture()
{
    D3DSURFACE_DESC desc{}, back{};
    if (!g_texture0 || g_texture0->GetType() != D3DRTYPE_TEXTURE ||
        FAILED(static_cast<IDirect3DTexture9*>(g_texture0)->GetLevelDesc(0, &desc)))
        return false;
    return (desc.Usage & D3DUSAGE_RENDERTARGET) && g_back && SUCCEEDED(g_back->GetDesc(&back)) &&
           desc.Width == back.Width && desc.Height == back.Height;
}

void LearnFrame(IDirect3DDevice9* device)
{
    g_frameKnown = true;
    int eye = 0;
    unsigned pair = 0;
    g_frameLeft = g_mode.load() == 1 && engine::PeekFrameView(&eye, &pair) && pair && eye == 0;
    IDirect3DSurface9* back = nullptr;
    if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) && back) {
        g_back = back;
        back->Release(); // le device garde le back buffer en vie : seul le pointeur sert
    }
    if (g_frameLeft)
        CountPanel(PanelEvent::LeftFrame);
}

// Avant chaque dessin. false : ne pas dessiner.
bool BeforeDraw(IDirect3DDevice9* device, UINT primitives)
{
    if (GetCurrentThreadId() != g_thread)
        return true;
    if (g_redirecting) {
        if (Multiplies()) {
            ++g_skipped;
            return false;
        }
        SetAlphaRule(device);
        ++g_panelDraws;
        if (g_tracing)
            TraceDraw(primitives);
        return true;
    }
    if (!g_frameKnown)
        LearnFrame(device);
    if (!g_frameLeft || !g_onBack)
        return true;
    const bool scene = (g_segmentDraws == 0 || g_composite) && SceneTexture();
    if (g_segmentDraws++ == 0) {
        g_composite = scene;
        return true;
    }
    if (!g_composite || scene)
        return true;
    // Un voile qui multiplie l'image (assombrissement des bords, teinte) vient encore avant
    // l'interface. Il n'a pas de sens sur un calque transparent, et l'œil droit ne l'a jamais
    // eu : il n'est dessiné nulle part.
    if (Multiplies()) {
        ++g_skipped;
        return false;
    }
    // Premier dessin après la recopie de la scène : l'interface commence
    if (!EnsureSurfaces(device) || FAILED(g_setRenderTarget(device, 0, g_panel)))
        return true;
    device->Clear(0, nullptr, D3DCLEAR_TARGET, 0x00000000, 1.0f, 0);
    g_setRenderState(device, D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
    g_alphaRule = -1;
    SetAlphaRule(device);
    g_redirecting = true;
    g_gameWantsBack = true;
    g_panelDraws = 1;
    g_tracing = g_traceWanted > 0 && --g_traceWanted == 0;
    if (g_tracing) {
        g_traceLines = g_traceRun = 0;
        TraceDraw(primitives);
    }
    return true;
}

HRESULT STDMETHODCALLTYPE HkSetRenderTarget(IDirect3DDevice9* self, DWORD index, IDirect3DSurface9* surface)
{
    if (index == 0 && GetCurrentThreadId() == g_thread) {
        if (!g_frameKnown)
            LearnFrame(self);
        g_onBack = surface == g_back;
        g_segmentDraws = 0;
        g_composite = false;
        if (g_redirecting) {
            // Le jeu repasse par une autre cible (rare) : on le laisse faire, et son retour au
            // back buffer redevient un retour au calque
            g_gameWantsBack = g_onBack;
            if (g_onBack && g_panel)
                return g_setRenderTarget(self, 0, g_panel);
        }
    }
    return g_setRenderTarget(self, index, surface);
}

HRESULT STDMETHODCALLTYPE HkSetRenderState(IDirect3DDevice9* self, D3DRENDERSTATETYPE state, DWORD value)
{
    if (static_cast<unsigned>(state) < 256 && GetCurrentThreadId() == g_thread) {
        g_states[state] = value;
        g_stateKnown[state] = true;
    }
    return g_setRenderState(self, state, value);
}

HRESULT STDMETHODCALLTYPE HkSetTexture(IDirect3DDevice9* self, DWORD stage, IDirect3DBaseTexture9* texture)
{
    if (stage == 0 && GetCurrentThreadId() == g_thread)
        g_texture0 = texture;
    return g_setTexture(self, stage, texture);
}

HRESULT STDMETHODCALLTYPE HkDrawPrimitive(IDirect3DDevice9* self, D3DPRIMITIVETYPE type, UINT start, UINT count)
{
    if (!BeforeDraw(self, count))
        return D3D_OK;
    return g_drawPrimitive(self, type, start, count);
}

HRESULT STDMETHODCALLTYPE HkDrawIndexed(IDirect3DDevice9* self, D3DPRIMITIVETYPE type, INT base, UINT minIndex, UINT vertices,
                                        UINT start, UINT count)
{
    if (!BeforeDraw(self, count))
        return D3D_OK;
    return g_drawIndexed(self, type, base, minIndex, vertices, start, count);
}

HRESULT STDMETHODCALLTYPE HkDrawUp(IDirect3DDevice9* self, D3DPRIMITIVETYPE type, UINT count, const void* data, UINT stride)
{
    if (!BeforeDraw(self, count))
        return D3D_OK;
    return g_drawUp(self, type, count, data, stride);
}

HRESULT STDMETHODCALLTYPE HkDrawIndexedUp(IDirect3DDevice9* self, D3DPRIMITIVETYPE type, UINT minIndex, UINT vertices, UINT count,
                                          const void* indices, D3DFORMAT format, const void* data, UINT stride)
{
    if (!BeforeDraw(self, count))
        return D3D_OK;
    return g_drawIndexedUp(self, type, minIndex, vertices, count, indices, format, data, stride);
}

bool EnsureHooks(IDirect3DDevice9* device)
{
    if (g_hooks || g_hooksFailed)
        return g_hooks;
    void** methods = *reinterpret_cast<void***>(device);
    const struct {
        int index;
        void* detour;
        void** original;
    } hooks[] = {
        {kSetRenderTarget, reinterpret_cast<void*>(&HkSetRenderTarget), reinterpret_cast<void**>(&g_setRenderTarget)},
        {kSetRenderState, reinterpret_cast<void*>(&HkSetRenderState), reinterpret_cast<void**>(&g_setRenderState)},
        {kSetTexture, reinterpret_cast<void*>(&HkSetTexture), reinterpret_cast<void**>(&g_setTexture)},
        {kDrawPrimitive, reinterpret_cast<void*>(&HkDrawPrimitive), reinterpret_cast<void**>(&g_drawPrimitive)},
        {kDrawIndexedPrimitive, reinterpret_cast<void*>(&HkDrawIndexed), reinterpret_cast<void**>(&g_drawIndexed)},
        {kDrawPrimitiveUP, reinterpret_cast<void*>(&HkDrawUp), reinterpret_cast<void**>(&g_drawUp)},
        {kDrawIndexedPrimitiveUP, reinterpret_cast<void*>(&HkDrawIndexedUp), reinterpret_cast<void**>(&g_drawIndexedUp)},
    };
    g_thread = GetCurrentThreadId();
    int n = 0;
    for (const auto& h : hooks) {
        const MH_STATUS st = MH_CreateHook(methods[h.index], h.detour, h.original);
        if (st != MH_OK || MH_EnableHook(methods[h.index]) != MH_OK) {
            LOG_RENDER("calque de l'interface : ECHEC du detour de la methode %d du device (%s), l'interface reste "
                       "dans l'image de l'oeil gauche",
                       h.index, MH_StatusToString(st));
            g_hooksFailed = true;
            return false;
        }
        g_hooked[n++] = methods[h.index];
    }
    LOG_RENDER("calque de l'interface : SetRenderTarget, SetTexture et les dessins du device du jeu detournes "
               "([Hud] panel=1)");
    g_hooks = true;
    return true;
}

void Read(IDirect3DDevice9* device, const FrameView& view)
{
    if (FAILED(device->GetRenderTargetData(g_panel, g_sysmem)))
        return;
    D3DLOCKED_RECT locked{};
    if (FAILED(g_sysmem->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
        return;
    PublishPanel(static_cast<const uint8_t*>(locked.pBits), locked.Pitch, g_width, g_height, view);
    g_sysmem->UnlockRect();
    CountPanel(PanelEvent::Read);
}

} // namespace

void OnPresent(IDirect3DDevice9* device, const FrameView& view)
{
    const ULONGLONG now = GetTickCount64();
    if (now - g_settingsTick >= 1000) {
        g_settingsTick = now;
        g_mode = config::Int("Hud", "panel", 1);
        // Lecture du calque : [Hud] panel_rate fois par seconde au plus (chaque lecture coûte 2 à 3 ms au jeu)
        const int rate = config::Int("Hud", "panel_rate", 15);
        g_readInterval = 1000 / static_cast<ULONGLONG>(rate < 1 ? 1 : rate > 120 ? 120 : rate);
    }
    if (g_mode.load() == 1 && !g_hooks)
        EnsureHooks(device);
    if (now == g_settingsTick && config::Int("Hud", "panel_trace", 0) == 1 && !g_traceWanted && !g_traceLines)
        g_traceWanted = 400; // la 400e image redirigée
    if (g_redirecting) {
        const double start = NowMs();
        if (g_tracing) {
            TraceFlush();
            g_tracing = false;
        }
        // Le jeu retrouve son back buffer et son mélange habituel
        g_setRenderState(device, D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
        if (g_gameWantsBack && g_back)
            g_setRenderTarget(device, 0, g_back);
        if (now - g_readTick + 2 >= g_readInterval) {
            g_readTick = now;
            Read(device, view);
        }
        if (g_logged < 3) {
            ++g_logged;
            LOG_RENDER("calque de l'interface : %d dessins rediriges dans l'image de l'oeil gauche de la paire %u, %u "
                       "voile(s) non dessine(s) depuis le debut",
                       g_panelDraws, view.pair, g_skipped);
        }
        g_redirecting = false;
        g_active = true;
        g_activeTick = now;
        CountPanel(PanelEvent::Redirected, static_cast<long long>((NowMs() - start) * 1000.0));
    } else if (g_active.load() && now - g_activeTick > 500) {
        g_active = false;
    }
    // Image suivante
    g_frameKnown = false;
    g_frameLeft = false;
    g_onBack = false;
    g_segmentDraws = 0;
    g_composite = false;
}

void ReleaseResources()
{
    ReleaseSurfaces();
    g_failed = false;
    g_back = nullptr;
    g_texture0 = nullptr;
    g_redirecting = false;
    g_frameKnown = false;
}

void Remove()
{
    for (void*& target : g_hooked) {
        if (target) {
            MH_DisableHook(target);
            MH_RemoveHook(target);
            target = nullptr;
        }
    }
    g_hooks = false;
}

bool Active()
{
    return g_active.load();
}

} // namespace render::ui_panel
