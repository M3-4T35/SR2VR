#include "render/frame_capture.h"
#include "util/log.h"

#include <d3d9.h>

#include <atomic>
#include <cstring>
#include <mutex>

namespace render {
namespace {

std::atomic<bool> g_requested{false};

// Thread du jeu uniquement
IDirect3DDevice9* g_owner = nullptr;
IDirect3DSurface9* g_readback = nullptr; // surface en mémoire système, taille du back buffer
UINT g_width = 0;
UINT g_height = 0;
D3DFORMAT g_format = D3DFMT_UNKNOWN;
bool g_unsupported = false; // format ou multisampling non géré : capture coupée pour ce device
int g_failures = 0;
CpuFrame g_work;
CpuFrame g_staged;        // œil gauche d'une paire, en attente de son œil droit
unsigned g_stagedPair = 0; // numéro de cette paire, 0 si rien n'attend

// Partagé avec le thread XR
std::mutex g_mutex;
CpuFrame g_ready[2]; // dernière capture de chaque œil
uint64_t g_seq = 0;
unsigned g_statCount = 0;
double g_statMs = 0.0;
unsigned g_statPairs = 0;
std::atomic<unsigned> g_statBrokenPairs{0};
unsigned g_statEyeCount[2]{};
double g_statEyeLuma[2]{};

// Luminosité moyenne (0 à 255) d'un échantillon de pixels BGRA : une ligne sur 16, un pixel sur 16
double SampleLuma(const CpuFrame& frame)
{
    unsigned long long sum = 0;
    unsigned samples = 0;
    for (UINT y = 8; y < frame.height; y += 16) {
        const uint8_t* row = frame.pixels.data() + static_cast<size_t>(y) * frame.width * 4;
        for (UINT x = 8; x < frame.width; x += 16) {
            const uint8_t* p = row + static_cast<size_t>(x) * 4;
            sum += (p[0] + 2u * p[1] + p[2]) / 4u;
            ++samples;
        }
    }
    return samples ? static_cast<double>(sum) / samples : 0.0;
}

double NowMs()
{
    static LARGE_INTEGER frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
}

// Une fois par device : le device du jeu accepte-t-il quand même une texture partagée ?
// (attendu : non sur un device classique ; oui s'il devient D3D9Ex ou sous DXVK)
void ProbeSharedTexture(IDirect3DDevice9* device, UINT w, UINT h)
{
    IDirect3DTexture9* texture = nullptr;
    HANDLE shared = nullptr;
    const HRESULT hr = device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                             &texture, &shared);
    LOG_RENDER("capture : texture partagee D3D9 -> 0x%08lX, handle %p (%s)", static_cast<unsigned long>(hr),
               static_cast<void*>(shared),
               SUCCEEDED(hr) && shared ? "partage GPU possible" : "partage GPU impossible, lecture par la memoire");
    if (texture)
        texture->Release();
}

bool PrepareReadback(IDirect3DDevice9* device, const D3DSURFACE_DESC& desc)
{
    if (device == g_owner && desc.Width == g_width && desc.Height == g_height && desc.Format == g_format)
        return !g_unsupported;

    if (g_readback) {
        g_readback->Release();
        g_readback = nullptr;
    }
    g_owner = device;
    g_width = desc.Width;
    g_height = desc.Height;
    g_format = desc.Format;
    g_unsupported = false;
    g_failures = 0;

    if (desc.Format != D3DFMT_A8R8G8B8 && desc.Format != D3DFMT_X8R8G8B8) {
        LOG_RENDER("capture : format de back buffer %d non gere, capture coupee", desc.Format);
        g_unsupported = true;
        return false;
    }
    if (desc.MultiSampleType != D3DMULTISAMPLE_NONE) {
        // GetRenderTargetData refuse une cible multisamplée : il faudra la résoudre d'abord
        LOG_RENDER("capture : back buffer multisample (%d) non gere, capture coupee (couper l'anticrenelage)",
                   desc.MultiSampleType);
        g_unsupported = true;
        return false;
    }
    const HRESULT hr = device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM,
                                                            &g_readback, nullptr);
    if (FAILED(hr) || !g_readback) {
        LOG_RENDER("ERREUR : surface de lecture %ux%u non creee : 0x%08lX, capture coupee", desc.Width, desc.Height,
                   static_cast<unsigned long>(hr));
        g_unsupported = true;
        return false;
    }
    LOG_RENDER("capture : lecture du back buffer %ux%u (format %d) par la memoire", desc.Width, desc.Height,
               desc.Format);
    ProbeSharedTexture(device, desc.Width, desc.Height);
    return true;
}

} // namespace

void RequestFrame()
{
    g_requested = true;
}

void CaptureFrame(IDirect3DDevice9* device, const FrameView& view)
{
    if (!device)
        return;
    const bool paired = view.valid && view.pair != 0;
    const bool second = paired && view.eye == 1;
    if (second) {
        // Œil droit : capturé si et seulement si son œil gauche l'a été. Les deux images d'une
        // paire se suivent de plus près que deux demandes du thread XR.
        if (g_stagedPair != view.pair)
            return;
    } else {
        if (g_stagedPair) {
            // L'œil gauche précédent n'a jamais eu son œil droit
            g_stagedPair = 0;
            ++g_statBrokenPairs;
        }
        if (!g_requested.exchange(false))
            return;
    }
    const double start = NowMs();

    IDirect3DSurface9* backBuffer = nullptr;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) || !backBuffer)
        return;
    D3DSURFACE_DESC desc{};
    backBuffer->GetDesc(&desc);
    if (!PrepareReadback(device, desc)) {
        backBuffer->Release();
        return;
    }

    HRESULT hr = device->GetRenderTargetData(backBuffer, g_readback);
    backBuffer->Release();
    D3DLOCKED_RECT locked{};
    if (SUCCEEDED(hr))
        hr = g_readback->LockRect(&locked, nullptr, D3DLOCK_READONLY);
    if (FAILED(hr)) {
        // Device perdu (changement de mode...) : on réessaiera à la prochaine demande
        if (++g_failures == 1 || g_failures == 100)
            LOG_RENDER("capture : lecture du back buffer en echec 0x%08lX (%d fois)", static_cast<unsigned long>(hr),
                       g_failures);
        return;
    }

    const size_t rowBytes = static_cast<size_t>(desc.Width) * 4;
    g_work.pixels.resize(rowBytes * desc.Height);
    g_work.width = desc.Width;
    g_work.height = desc.Height;
    g_work.view = view;
    const auto* src = static_cast<const uint8_t*>(locked.pBits);
    if (static_cast<size_t>(locked.Pitch) == rowBytes) {
        memcpy(g_work.pixels.data(), src, rowBytes * desc.Height);
    } else {
        for (UINT y = 0; y < desc.Height; ++y)
            memcpy(g_work.pixels.data() + y * rowBytes, src + static_cast<size_t>(y) * locked.Pitch, rowBytes);
    }
    g_readback->UnlockRect();

    const double luma = view.valid ? SampleLuma(g_work) : 0.0;
    const double elapsed = NowMs() - start;
    std::lock_guard lock(g_mutex);
    if (view.valid) {
        const int eye = view.eye == 1 ? 1 : 0;
        ++g_statEyeCount[eye];
        g_statEyeLuma[eye] += luma;
    }
    g_work.seq = ++g_seq;
    ++g_statCount;
    g_statMs += elapsed;
    if (paired && !second) {
        // Œil gauche d'une paire : gardé de côté jusqu'à son œil droit
        std::swap(g_work, g_staged);
        g_stagedPair = view.pair;
    } else if (second) {
        // La paire est complète : les deux yeux sont publiés ensemble
        std::swap(g_staged, g_ready[0]);
        std::swap(g_work, g_ready[1]);
        g_stagedPair = 0;
        ++g_statPairs;
    } else {
        std::swap(g_work, g_ready[view.valid && view.eye == 1 ? 1 : 0]);
    }
}

void TakeLatestFrames(CpuFrame& left, CpuFrame& right, bool fresh[2])
{
    std::lock_guard lock(g_mutex);
    CpuFrame* const frames[2] = {&left, &right};
    for (int eye = 0; eye < 2; ++eye) {
        CpuFrame& ready = g_ready[eye];
        fresh[eye] = ready.seq != 0 && ready.seq > frames[eye]->seq;
        // L'ancienne image du thread XR revient au jeu : son numéro, plus petit, la désigne
        // comme périmée et son tampon sera réutilisé
        if (fresh[eye])
            std::swap(*frames[eye], ready);
    }
}

void TakeCaptureStats(CaptureStats* stats)
{
    std::lock_guard lock(g_mutex);
    stats->count = g_statCount;
    stats->averageMs = g_statCount ? g_statMs / g_statCount : 0.0;
    stats->pairs = g_statPairs;
    stats->brokenPairs = g_statBrokenPairs.exchange(0);
    for (int eye = 0; eye < 2; ++eye) {
        stats->eyeCount[eye] = g_statEyeCount[eye];
        stats->eyeLuma[eye] = g_statEyeCount[eye] ? g_statEyeLuma[eye] / g_statEyeCount[eye] : 0.0;
        g_statEyeCount[eye] = 0;
        g_statEyeLuma[eye] = 0.0;
    }
    g_statCount = 0;
    g_statMs = 0.0;
    g_statPairs = 0;
}

} // namespace render
