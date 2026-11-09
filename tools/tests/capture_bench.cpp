// SR2VR - mesure, sur la carte graphique de la machine, de ce que coûte chaque façon de relire
// une image Direct3D 9 (ce que fait render/frame_capture pour envoyer l'image au casque).
// Device comme celui du jeu (fenêtré, 1920x1080, matériel, MULTITHREADED), image chargée de
// rectangles translucides pour occuper la carte, travail du processeur simulé entre les lots
// de dessin. Pour chaque méthode : temps pris au thread du jeu par image et cadence obtenue.
//
//     capture_bench [largeur hauteur [ms de travail du processeur par image [ms visés pour la carte]]]
//
// Méthodes :
//   aucune     pas de relecture (référence)
//   synchrone  GetRenderTargetData(back buffer) + LockRect + copie          ([Display] async_capture=0)
//   immediate  StretchRect + GetRenderTargetData tout de suite, lecture plus tard (async_capture=1)
//   differee   StretchRect + événement ; GetRenderTargetData quand la carte a fini, à un Present suivant
//   sans copie comme « différée », mais la surface verrouillée est lue sur place (pas de copie)
#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

struct Vertex {
    float x, y, z, rhw;
    D3DCOLOR color;
};
constexpr DWORD kFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;

LARGE_INTEGER g_frequency;
double NowMs()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * 1000.0 / static_cast<double>(g_frequency.QuadPart);
}

void Spin(double ms)
{
    const double end = NowMs() + ms;
    while (NowMs() < end) {
    }
}

UINT g_width = 1920, g_height = 1080;
double g_cpuMs = 12.0;
int g_quads = 0;
IDirect3DDevice9* g_device = nullptr;

// Une image : des lots de rectangles plein écran, du travail de processeur entre les lots
void DrawFrame(unsigned id)
{
    const float w = static_cast<float>(g_width), h = static_cast<float>(g_height);
    const D3DCOLOR color = D3DCOLOR_ARGB(8, id & 0xFF, (id >> 8) & 0xFF, 0x80);
    const Vertex quad[4] = {{0, 0, 0.5f, 1, color}, {w, 0, 0.5f, 1, color}, {0, h, 0.5f, 1, color}, {w, h, 0.5f, 1, color}};
    g_device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(id & 0xFF, (id >> 8) & 0xFF, 0x80), 1.0f, 0);
    g_device->BeginScene();
    g_device->SetFVF(kFvf);
    g_device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    g_device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    g_device->SetRenderState(D3DRS_ZENABLE, FALSE);
    const int batches = 8;
    for (int b = 0; b < batches; ++b) {
        for (int i = 0; i < g_quads / batches; ++i)
            g_device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex));
        Spin(g_cpuMs / batches);
    }
    g_device->EndScene();
}

struct Slot {
    IDirect3DSurface9* target = nullptr;
    IDirect3DSurface9* sysmem = nullptr;
    IDirect3DQuery9* query = nullptr;
    int state = 0; // 0 libre, 1 copie lancée, 2 transfert fait
    unsigned id = 0;
};
constexpr int kSlots = 4;
Slot g_slots[kSlots];
IDirect3DSurface9* g_readback = nullptr;
std::vector<uint8_t> g_pixels;
unsigned long long g_checksum = 0;
unsigned g_read = 0, g_waited = 0;

void CopyOut(IDirect3DSurface9* surface, bool copy)
{
    D3DLOCKED_RECT locked{};
    if (FAILED(surface->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
        return;
    if (copy) {
        const size_t row = static_cast<size_t>(g_width) * 4;
        for (UINT y = 0; y < g_height; ++y)
            memcpy(g_pixels.data() + y * row, static_cast<const uint8_t*>(locked.pBits) + static_cast<size_t>(y) * locked.Pitch, row);
        g_checksum += g_pixels[row * (g_height / 2) + (g_width / 2) * 4];
    } else {
        g_checksum += static_cast<const uint8_t*>(locked.pBits)[static_cast<size_t>(locked.Pitch) * (g_height / 2) + (g_width / 2) * 4];
    }
    surface->UnlockRect();
    ++g_read;
}

enum Method { kNone, kSync, kImmediate, kDeferred, kNoCopy, kMethods };
const char* const kNames[kMethods] = {"aucune", "synchrone", "immediate", "differee", "sans copie"};

int g_head = 0, g_pending = 0;

// Ce que la méthode fait au Present de l'image `id` (thread du jeu)
void Capture(Method method, unsigned id, IDirect3DSurface9* backBuffer)
{
    if (method == kNone)
        return;
    if (method == kSync) {
        if (SUCCEEDED(g_device->GetRenderTargetData(backBuffer, g_readback)))
            CopyOut(g_readback, true);
        return;
    }
    // Copies terminées, dans l'ordre
    while (g_pending > 0) {
        Slot& slot = g_slots[g_head];
        const bool done = slot.query->GetData(nullptr, 0, 0) != S_FALSE;
        if (!done && g_pending < kSlots - 1)
            break;
        if (!done)
            ++g_waited;
        if (method != kImmediate)
            g_device->GetRenderTargetData(slot.target, slot.sysmem);
        CopyOut(slot.sysmem, method != kNoCopy);
        g_head = (g_head + 1) % kSlots;
        --g_pending;
    }
    Slot& slot = g_slots[(g_head + g_pending) % kSlots];
    g_device->StretchRect(backBuffer, nullptr, slot.target, nullptr, D3DTEXF_NONE);
    if (method == kImmediate)
        g_device->GetRenderTargetData(slot.target, slot.sysmem);
    slot.query->Issue(D3DISSUE_END);
    slot.id = id;
    ++g_pending;
}

struct Result {
    double frameMs = 0, captureMs = 0, captureMax = 0, presentMs = 0;
    unsigned read = 0, waited = 0;
};

Result Run(Method method, int frames)
{
    g_head = g_pending = 0;
    g_read = g_waited = 0;
    Result r;
    std::vector<double> captures;
    double start = 0;
    for (int i = -20; i < frames; ++i) { // 20 images de mise en route
        if (i == 0)
            start = NowMs();
        DrawFrame(static_cast<unsigned>(i + 100));
        IDirect3DSurface9* backBuffer = nullptr;
        g_device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
        const double t0 = NowMs();
        Capture(method, static_cast<unsigned>(i + 100), backBuffer);
        const double t1 = NowMs();
        backBuffer->Release();
        g_device->Present(nullptr, nullptr, nullptr, nullptr);
        const double t2 = NowMs();
        if (i >= 0) {
            captures.push_back(t1 - t0);
            r.captureMs += t1 - t0;
            r.presentMs += t2 - t1;
        }
    }
    r.frameMs = (NowMs() - start) / frames;
    r.captureMs /= frames;
    r.presentMs /= frames;
    std::sort(captures.begin(), captures.end());
    r.captureMax = captures.empty() ? 0 : captures[captures.size() * 99 / 100];
    r.read = g_read;
    r.waited = g_waited;
    // Les copies encore en vol sont laissées : la méthode suivante repart de zéro
    for (Slot& slot : g_slots)
        slot.query->GetData(nullptr, 0, D3DGETDATA_FLUSH);
    Sleep(50);
    return r;
}

// Temps que met la carte à finir l'image : relecture synchrone juste après le dessin, moins la
// même relecture sur une image vide
double GpuMs(int quads)
{
    const int saved = g_quads;
    const double savedCpu = g_cpuMs;
    g_cpuMs = 0;
    double total = 0;
    for (int pass = 0; pass < 2; ++pass) {
        g_quads = pass ? quads : 0;
        double sum = 0;
        for (int i = 0; i < 12; ++i) {
            DrawFrame(static_cast<unsigned>(i));
            IDirect3DSurface9* backBuffer = nullptr;
            g_device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
            const double t0 = NowMs();
            g_device->GetRenderTargetData(backBuffer, g_readback);
            if (i >= 2)
                sum += NowMs() - t0;
            backBuffer->Release();
            g_device->Present(nullptr, nullptr, nullptr, nullptr);
        }
        total = pass ? sum / 10 - total : sum / 10;
    }
    g_quads = saved;
    g_cpuMs = savedCpu;
    return total;
}

} // namespace

int main(int argc, char** argv)
{
    QueryPerformanceFrequency(&g_frequency);
    double gpuTarget = 8.0;
    if (argc > 2) {
        g_width = static_cast<UINT>(atoi(argv[1]));
        g_height = static_cast<UINT>(atoi(argv[2]));
    }
    if (argc > 3)
        g_cpuMs = atof(argv[3]);
    if (argc > 4)
        gpuTarget = atof(argv[4]);

    WNDCLASSA wc{};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "sr2vr_capture_bench";
    RegisterClassA(&wc);
    HWND window = CreateWindowA(wc.lpszClassName, "bench", WS_POPUP, 0, 0, static_cast<int>(g_width),
                                static_cast<int>(g_height), nullptr, nullptr, wc.hInstance, nullptr);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!window || !d3d) {
        printf("pas de Direct3D 9 ici\n");
        return 77;
    }
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    pp.BackBufferWidth = g_width;
    pp.BackBufferHeight = g_height;
    pp.hDeviceWindow = window;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    // Comme le jeu : sommets par le matériel, PUREDEVICE, MULTITHREADED
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                                   D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_PUREDEVICE | D3DCREATE_MULTITHREADED, &pp,
                                   &g_device);
    if (FAILED(hr) || !g_device) {
        printf("device D3D9 non cree : 0x%08lX\n", static_cast<unsigned long>(hr));
        return 77;
    }
    D3DADAPTER_IDENTIFIER9 adapter{};
    d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &adapter);
    printf("carte : %s\nimage %ux%u, %.1f ms de travail du processeur par image\n", adapter.Description, g_width, g_height,
           g_cpuMs);

    g_pixels.resize(static_cast<size_t>(g_width) * g_height * 4);
    hr = g_device->CreateOffscreenPlainSurface(g_width, g_height, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &g_readback, nullptr);
    for (Slot& slot : g_slots) {
        if (SUCCEEDED(hr))
            hr = g_device->CreateRenderTarget(g_width, g_height, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &slot.target,
                                              nullptr);
        if (SUCCEEDED(hr))
            hr = g_device->CreateOffscreenPlainSurface(g_width, g_height, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &slot.sysmem,
                                                       nullptr);
        if (SUCCEEDED(hr))
            hr = g_device->CreateQuery(D3DQUERYTYPE_EVENT, &slot.query);
    }
    if (FAILED(hr)) {
        printf("surfaces non creees : 0x%08lX\n", static_cast<unsigned long>(hr));
        return 1;
    }

    // Copie en mémoire seule (ce que coûte la recopie de l'image hors de la surface)
    {
        std::vector<uint8_t> source(g_pixels.size(), 0x55);
        const double t0 = NowMs();
        for (int i = 0; i < 20; ++i)
            memcpy(g_pixels.data(), source.data(), source.size());
        printf("copie de l'image en memoire : %.2f ms\n", (NowMs() - t0) / 20);
    }

    // Charges de la carte : presque rien (jeu limité par le processeur), puis la cible demandée
    for (int load = 0; load < 2; ++load) {
        int quads = 0;
        double gpu = 0;
        if (load) {
            for (quads = 64; quads < 200000; quads *= 2) {
                gpu = GpuMs(quads);
                if (gpu >= gpuTarget)
                    break;
            }
        }
        g_quads = quads;
        printf("\n--- %d rectangles plein ecran par image : la carte met environ %.1f ms a finir l'image ---\n", quads,
               gpu);
        printf("%-11s %9s %9s %12s %12s %9s %7s\n", "methode", "image ms", "par s", "capture ms", "capture 99%", "Present",
               "attend.");
        for (int m = 0; m < kMethods; ++m) {
            const Result r = Run(static_cast<Method>(m), 150);
            printf("%-11s %9.2f %9.1f %12.3f %12.3f %9.3f %7u\n", kNames[m], r.frameMs, 1000.0 / r.frameMs, r.captureMs,
                   r.captureMax, r.presentMs, r.waited);
            fflush(stdout);
        }
    }
    printf("\n(somme de controle %llu)\n", g_checksum);
    return 0;
}
