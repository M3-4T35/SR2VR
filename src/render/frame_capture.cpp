#include "render/frame_capture.h"
#include "util/config.h"
#include "util/log.h"

#include <d3d9.h>

#include <atomic>
#include <cstring>
#include <mutex>

namespace render {
namespace {

// --- Deux façons de relire le back buffer ----------------------------------------------
//
// Synchrone ([Display] async_capture=0, l'ancienne) : GetRenderTargetData du back buffer vers
// une surface en mémoire système, puis LockRect, au Present de l'image. Le thread du jeu
// attend que le GPU ait fini toute l'image avant de pouvoir la lire.
//
// Asynchrone (async_capture=1, par défaut) : au Present, le back buffer est recopié par le
// GPU dans une cible de rendu à nous (StretchRect, qui résout aussi un back buffer
// multisamplé), le transfert vers la mémoire système est demandé tout de suite
// (GetRenderTargetData) et un événement (requête D3DQUERYTYPE_EVENT) est posé derrière. Rien
// de tout cela n'attend le GPU. Aux Present suivants, une copie dont l'événement est passé est
// lue (LockRect sans attente) et publiée. Une copie qui tarde plus de kMaxAgePresents
// images est attendue. Méthode de la capture « mémoire » d'OBS pour Direct3D 9.
// Prix : l'image arrive au casque un Present plus tard (au moins), avec sa vue d'origine.
constexpr int kSlots = 4;                    // copies en vol au plus
constexpr unsigned long long kMaxAgePresents = 2; // au-delà, la copie est attendue (latence bornée)

struct Slot {
    IDirect3DSurface9* target = nullptr;  // cible de rendu (D3DPOOL_DEFAULT), copie GPU du back buffer
    IDirect3DSurface9* sysmem = nullptr;  // surface en mémoire système, reçoit le transfert
    IDirect3DQuery9* query = nullptr;     // événement posé après le transfert (absent : non géré)
    FrameView view;
    unsigned long long issuedAt = 0;      // numéro du Present qui a lancé la copie
};

std::atomic<bool> g_requested{false};

// Thread du jeu uniquement (et Reset, protégé par g_deviceMutex)
std::mutex g_deviceMutex;
IDirect3DDevice9* g_owner = nullptr;
IDirect3DSurface9* g_readback = nullptr; // synchrone : surface en mémoire système
UINT g_width = 0;
UINT g_height = 0;
D3DFORMAT g_format = D3DFMT_UNKNOWN;
D3DMULTISAMPLE_TYPE g_multisample = D3DMULTISAMPLE_NONE;
bool g_preparedAsync = false;
bool g_unsupported = false; // format ou multisampling non géré : capture coupée pour ce device
int g_failures = 0;
CpuFrame g_work;
CpuFrame g_staged;         // œil gauche d'une paire, en attente de son œil droit
unsigned g_stagedPair = 0; // numéro de cette paire, 0 si rien n'attend
unsigned g_issuedPair = 0; // paire dont l'œil gauche a été capturé (ou lancé), 0 sinon

bool g_async = true;                         // réglage
IDirect3DDevice9* g_asyncFailed = nullptr;   // device sur lequel la capture asynchrone a échoué
ULONGLONG g_configTick = 0;
unsigned long long g_presents = 0;
Slot g_slots[kSlots];
int g_head = 0;    // plus ancienne copie en vol
int g_pending = 0; // copies en vol

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
unsigned g_statWaits = 0;
unsigned long long g_statLatency = 0; // somme des Present écoulés entre lancement et publication
unsigned g_statAsyncCount = 0;

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

template <typename T>
void SafeRelease(T*& p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
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
    SafeRelease(texture);
}

// Copies en vol abandonnées (changement de device ou de mode, Reset) : la paire en cours aussi
void DropPending()
{
    g_head = 0;
    g_pending = 0;
    g_issuedPair = 0;
}

void ReleaseResources()
{
    DropPending();
    for (Slot& slot : g_slots) {
        SafeRelease(slot.target);
        SafeRelease(slot.sysmem);
        SafeRelease(slot.query);
    }
    SafeRelease(g_readback);
    g_owner = nullptr; // tout sera recréé à la prochaine capture
}

bool CreateAsyncSlots(IDirect3DDevice9* device, const D3DSURFACE_DESC& desc)
{
    int queries = 0;
    for (Slot& slot : g_slots) {
        HRESULT hr = device->CreateRenderTarget(desc.Width, desc.Height, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE,
                                                &slot.target, nullptr);
        if (SUCCEEDED(hr))
            hr = device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM,
                                                     &slot.sysmem, nullptr);
        if (FAILED(hr) || !slot.target || !slot.sysmem) {
            LOG_RENDER("capture asynchrone : surfaces %ux%u non creees (0x%08lX), retour a la capture synchrone",
                       desc.Width, desc.Height, static_cast<unsigned long>(hr));
            return false;
        }
        if (SUCCEEDED(device->CreateQuery(D3DQUERYTYPE_EVENT, &slot.query)) && slot.query)
            ++queries;
    }
    LOG_RENDER("capture asynchrone : %d copies en vol au plus, %ux%u, %s", kSlots, desc.Width, desc.Height,
               queries == kSlots ? "evenements GPU disponibles"
                                 : "evenements GPU indisponibles (lecture au Present suivant)");
    return true;
}

bool PrepareReadback(IDirect3DDevice9* device, const D3DSURFACE_DESC& desc)
{
    const bool async = g_async && device != g_asyncFailed;
    if (device == g_owner && desc.Width == g_width && desc.Height == g_height && desc.Format == g_format &&
        desc.MultiSampleType == g_multisample && async == g_preparedAsync)
        return !g_unsupported;

    const bool firstForDevice = device != g_owner || desc.Width != g_width || desc.Height != g_height;
    ReleaseResources();
    g_owner = device;
    g_width = desc.Width;
    g_height = desc.Height;
    g_format = desc.Format;
    g_multisample = desc.MultiSampleType;
    g_preparedAsync = async;
    g_unsupported = false;
    g_failures = 0;

    if (desc.Format != D3DFMT_A8R8G8B8 && desc.Format != D3DFMT_X8R8G8B8) {
        LOG_RENDER("capture : format de back buffer %d non gere, capture coupee", desc.Format);
        g_unsupported = true;
        return false;
    }
    if (async) {
        if (CreateAsyncSlots(device, desc)) {
            if (firstForDevice)
                ProbeSharedTexture(device, desc.Width, desc.Height);
            return true;
        }
        // Repli : capture synchrone pour ce device
        for (Slot& slot : g_slots) {
            SafeRelease(slot.target);
            SafeRelease(slot.sysmem);
            SafeRelease(slot.query);
        }
        g_asyncFailed = device; // pas de nouvel essai sur ce device
        g_preparedAsync = false;
    }
    if (desc.MultiSampleType != D3DMULTISAMPLE_NONE) {
        // GetRenderTargetData refuse une cible multisamplée (la capture asynchrone la résout)
        LOG_RENDER("capture : back buffer multisample (%d) non gere en capture synchrone, capture coupee "
                   "(couper l'anticrenelage ou [Display] async_capture=1)",
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
    LOG_RENDER("capture synchrone : lecture du back buffer %ux%u (format %d) par la memoire", desc.Width,
               desc.Height, desc.Format);
    if (firstForDevice)
        ProbeSharedTexture(device, desc.Width, desc.Height);
    return true;
}

void LogFailure(const char* what, HRESULT hr)
{
    // Device perdu (changement de mode...) : on réessaiera à la prochaine demande
    if (++g_failures == 1 || g_failures == 100)
        LOG_RENDER("capture : %s en echec 0x%08lX (%d fois)", what, static_cast<unsigned long>(hr), g_failures);
}

// Copie une surface verrouillée dans g_work, avec le repère de visée de sa vue
void CopyLocked(const D3DLOCKED_RECT& locked, const FrameView& view)
{
    const size_t rowBytes = static_cast<size_t>(g_width) * 4;
    g_work.pixels.resize(rowBytes * g_height);
    g_work.width = g_width;
    g_work.height = g_height;
    g_work.view = view;
    const auto* src = static_cast<const uint8_t*>(locked.pBits);
    if (static_cast<size_t>(locked.Pitch) == rowBytes) {
        memcpy(g_work.pixels.data(), src, rowBytes * g_height);
    } else {
        for (UINT y = 0; y < g_height; ++y)
            memcpy(g_work.pixels.data() + y * rowBytes, src + static_cast<size_t>(y) * locked.Pitch, rowBytes);
    }
    if (view.valid)
        DrawSight(g_work.pixels.data(), g_width, g_height, view.hfovDeg, view.sight);
}

// Publie g_work pour le thread XR : les deux yeux d'une paire ensemble, l'œil gauche gardé
// de côté jusqu'à son œil droit
void Publish(double elapsedMs)
{
    const FrameView view = g_work.view; // copie : g_work change de contenu aux échanges ci-dessous
    const bool paired = view.valid && view.pair != 0;
    const bool second = paired && view.eye == 1;
    const double luma = view.valid ? SampleLuma(g_work) : 0.0;
    std::lock_guard lock(g_mutex);
    if (view.valid) {
        const int eye = view.eye == 1 ? 1 : 0;
        ++g_statEyeCount[eye];
        g_statEyeLuma[eye] += luma;
    }
    ++g_statCount;
    g_statMs += elapsedMs;
    if (second && g_stagedPair != view.pair) {
        // Son œil gauche s'est perdu en route (lecture en échec) : pas de demi-paire
        ++g_statBrokenPairs;
        return;
    }
    g_work.seq = ++g_seq;
    if (paired && !second) {
        std::swap(g_work, g_staged);
        g_stagedPair = view.pair;
    } else if (second) {
        std::swap(g_staged, g_ready[0]);
        std::swap(g_work, g_ready[1]);
        g_stagedPair = 0;
        ++g_statPairs;
    } else {
        std::swap(g_work, g_ready[view.valid && view.eye == 1 ? 1 : 0]);
    }
}

// --- Capture synchrone -----------------------------------------------------------------

void CaptureSync(IDirect3DDevice9* device, IDirect3DSurface9* backBuffer, const FrameView& view, double start)
{
    HRESULT hr = device->GetRenderTargetData(backBuffer, g_readback);
    D3DLOCKED_RECT locked{};
    if (SUCCEEDED(hr))
        hr = g_readback->LockRect(&locked, nullptr, D3DLOCK_READONLY);
    if (FAILED(hr)) {
        LogFailure("lecture du back buffer", hr);
        return;
    }
    CopyLocked(locked, view);
    g_readback->UnlockRect();
    Publish(NowMs() - start);
}

// --- Capture asynchrone ----------------------------------------------------------------

// Lit et publie les copies terminées, dans l'ordre de leur lancement. force : la plus ancienne
// est attendue même si elle n'est pas finie (place nécessaire, ou trop vieille). Rend le temps
// passé.
double Collect(bool forceOldest)
{
    double spent = 0.0;
    while (g_pending > 0) {
        Slot& slot = g_slots[g_head];
        const unsigned long long age = g_presents - slot.issuedAt;
        bool done;
        if (slot.query) {
            // S_FALSE : pas encore passé ; une erreur (device perdu) : LockRect le dira
            done = slot.query->GetData(nullptr, 0, 0) != S_FALSE;
        } else {
            done = age >= 1;
        }
        if (!done && !forceOldest && age < kMaxAgePresents)
            break;
        const double start = NowMs();
        if (!done) {
            std::lock_guard lock(g_mutex);
            ++g_statWaits;
        }
        forceOldest = false;
        D3DLOCKED_RECT locked{};
        const HRESULT hr = slot.sysmem->LockRect(&locked, nullptr, D3DLOCK_READONLY); // attend si pas fini
        g_head = (g_head + 1) % kSlots;
        --g_pending;
        if (FAILED(hr)) {
            LogFailure("lecture d'une copie", hr);
            continue;
        }
        CopyLocked(locked, slot.view);
        slot.sysmem->UnlockRect();
        const double elapsed = NowMs() - start;
        spent += elapsed;
        {
            std::lock_guard lock(g_mutex);
            g_statLatency += age;
            ++g_statAsyncCount;
        }
        Publish(elapsed);
    }
    return spent;
}

// Lance la copie de l'image présentée. false si elle n'a pas pu l'être.
bool Issue(IDirect3DDevice9* device, IDirect3DSurface9* backBuffer, const FrameView& view)
{
    if (g_pending == kSlots)
        Collect(true); // toutes les copies sont en vol : la plus ancienne est attendue
    Slot& slot = g_slots[(g_head + g_pending) % kSlots];
    HRESULT hr = device->StretchRect(backBuffer, nullptr, slot.target, nullptr, D3DTEXF_NONE);
    if (SUCCEEDED(hr))
        hr = device->GetRenderTargetData(slot.target, slot.sysmem);
    if (FAILED(hr)) {
        LogFailure("copie du back buffer", hr);
        return false;
    }
    if (slot.query)
        slot.query->Issue(D3DISSUE_END);
    slot.view = view;
    slot.issuedAt = g_presents;
    ++g_pending;
    return true;
}

void ReadConfig()
{
    const ULONGLONG now = GetTickCount64();
    if (g_configTick && now - g_configTick < 1000)
        return;
    g_configTick = now;
    const bool async = config::Int("Display", "async_capture", 1) != 0;
    if (async != g_async) {
        LOG_RENDER("capture : passage en mode %s", async ? "asynchrone" : "synchrone");
        g_async = async; // PrepareReadback recrée les surfaces du nouveau mode
    }
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
    std::lock_guard deviceLock(g_deviceMutex);
    ++g_presents;
    ReadConfig();
    const double start = NowMs();

    // Copies terminées depuis les Present précédents (même pour une image qu'on ne capture pas)
    double collected = 0.0;
    if (g_preparedAsync && device == g_owner)
        collected = Collect(false);

    const bool paired = view.valid && view.pair != 0;
    const bool second = paired && view.eye == 1;
    if (second) {
        // Œil droit : capturé si et seulement si son œil gauche l'a été. Les deux images d'une
        // paire se suivent de plus près que deux demandes du thread XR.
        if (g_issuedPair != view.pair)
            return;
    } else {
        if (g_issuedPair) {
            // L'œil gauche précédent n'a jamais eu son œil droit
            g_issuedPair = 0;
            ++g_statBrokenPairs;
        }
        if (!g_requested.exchange(false))
            return;
    }

    IDirect3DSurface9* backBuffer = nullptr;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) || !backBuffer)
        return;
    D3DSURFACE_DESC desc{};
    backBuffer->GetDesc(&desc);
    if (!PrepareReadback(device, desc)) {
        backBuffer->Release();
        return;
    }
    if (g_preparedAsync) {
        const bool issued = Issue(device, backBuffer, view);
        backBuffer->Release();
        if (issued && paired)
            g_issuedPair = second ? 0 : view.pair;
        else if (second)
            g_issuedPair = 0;
        // Temps du lancement compté avec les lectures (une seule moyenne : temps pris au jeu)
        std::lock_guard lock(g_mutex);
        g_statMs += NowMs() - start - collected;
        return;
    }
    if (paired)
        g_issuedPair = second ? 0 : view.pair;
    CaptureSync(device, backBuffer, view, start);
    backBuffer->Release();
}

void ReleaseCaptureResources()
{
    std::lock_guard deviceLock(g_deviceMutex);
    if (g_owner)
        LOG_RENDER("capture : surfaces liberees avant le Reset du device");
    ReleaseResources();
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
    stats->async = g_statAsyncCount;
    stats->averageLatency = g_statAsyncCount ? static_cast<double>(g_statLatency) / g_statAsyncCount : 0.0;
    stats->waits = g_statWaits;
    for (int eye = 0; eye < 2; ++eye) {
        stats->eyeCount[eye] = g_statEyeCount[eye];
        stats->eyeLuma[eye] = g_statEyeCount[eye] ? g_statEyeLuma[eye] / g_statEyeCount[eye] : 0.0;
        g_statEyeCount[eye] = 0;
        g_statEyeLuma[eye] = 0.0;
    }
    g_statCount = 0;
    g_statMs = 0.0;
    g_statPairs = 0;
    g_statAsyncCount = 0;
    g_statLatency = 0;
    g_statWaits = 0;
}

} // namespace render
