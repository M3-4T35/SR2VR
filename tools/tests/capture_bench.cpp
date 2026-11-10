// SR2VR - mesure, sur la carte graphique de la machine, de ce que coûte chaque façon de relire
// une image Direct3D 9 (ce que fait render/frame_capture pour envoyer l'image au casque).
// Device comme celui du jeu (fenêtré, 1920x1080, matériel, PUREDEVICE, MULTITHREADED), images
// faites de beaucoup de petits appels de dessin avec du calcul entre eux (un jeu limité par son
// processeur). Pour chaque mode : temps pris au thread du jeu par image et cadence obtenue.
//
//     capture_bench [largeur hauteur [ms de calcul par image [appels de dessin par image]]]
//
// Sorties : le coût d'une relecture selon la taille et le format, puis les trois modes de
// [Display] async_capture (le vrai code de render/frame_capture) dans une boucle de paires
// d'images. La carte est peu sollicitée : quelques secondes en tout.
#include <windows.h>
#include <d3d9.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "render/frame_capture.h"
#include "util/config.h"
#include "util/log.h"

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
IDirect3DDevice9* g_device = nullptr;

unsigned long long g_checksum = 0;
int g_calls = 0; // petits appels de dessin par image (un triangle minuscule chacun)

// Une image comme celle d'un jeu limité par son processeur : beaucoup de petits appels de
// dessin, du calcul entre eux, presque rien pour la carte
void DrawGameFrame(unsigned id)
{
    const D3DCOLOR color = D3DCOLOR_XRGB(id & 0xFF, (id >> 8) & 0xFF, 0x80);
    const Vertex tiny[3] = {{8, 8, 0.5f, 1, color}, {12, 8, 0.5f, 1, color}, {8, 12, 0.5f, 1, color}};
    g_device->Clear(0, nullptr, D3DCLEAR_TARGET, color, 1.0f, 0);
    g_device->BeginScene();
    g_device->SetFVF(kFvf);
    const int batches = 48;
    for (int b = 0; b < batches; ++b) {
        for (int i = 0; i < g_calls / batches; ++i)
            g_device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tiny, sizeof(Vertex));
        Spin(g_cpuMs / batches);
    }
    g_device->EndScene();
}

struct Result {
    double pairMs = 0, captureMs = 0, presentMs = 0, workerMs = 0;
    unsigned pairs = 0;
};

// Le vrai code de capture, dans une boucle comme celle du jeu en vrai relief : deux images par
// paire, puis `waitMs` où le thread du rendu n'appelle pas Direct3D (il attend la simulation)
Result Run(int mode, int pairs, double waitMs)
{
    char value[4];
    snprintf(value, sizeof(value), "%d", mode);
    WritePrivateProfileStringA("Display", "async_capture", value, config::Path());
    Sleep(1100); // la capture relit ses réglages une fois par seconde
    render::CpuFrame left, right;
    bool fresh[2]{};
    render::CaptureStats stats;
    Result r;
    double start = 0;
    unsigned id = 0, pair = 0;
    for (int i = -10; i < pairs; ++i) {
        if (i == 0) {
            render::TakeCaptureStats(&stats);
            start = NowMs();
        }
        ++pair;
        for (int eye = 0; eye < 2; ++eye) {
            DrawGameFrame(++id);
            render::FrameView view;
            view.valid = true;
            view.eye = eye;
            view.pair = pair;
            view.headQuat[0] = static_cast<float>(id);
            if (eye == 0)
                render::RequestFrame();
            const double t0 = NowMs();
            render::CaptureFrame(g_device, view);
            const double t1 = NowMs();
            g_device->Present(nullptr, nullptr, nullptr, nullptr);
            if (i >= 0) {
                r.captureMs += t1 - t0;
                r.presentMs += NowMs() - t1;
            }
            if (eye == 1 && waitMs > 0)
                Spin(waitMs); // la simulation calcule, le rendu attend
            else if (eye == 0)
                Spin(1.0); // préparation de l'œil droit
        }
        render::TakeLatestFrames(left, right, fresh);
        if (fresh[0] && fresh[1] && i >= 0)
            ++r.pairs;
    }
    r.pairMs = (NowMs() - start) / pairs;
    r.captureMs /= pairs * 2;
    r.presentMs /= pairs * 2;
    render::TakeCaptureStats(&stats);
    r.workerMs = stats.workerMs;
    return r;
}

// Relecture seule (carte au repos) d'une cible de rendu de taille et de format donnés : dit si
// le coût est fixe par appel ou proportionnel au nombre d'octets
double ReadbackMs(UINT w, UINT h, D3DFORMAT format)
{
    IDirect3DSurface9* target = nullptr;
    IDirect3DSurface9* sysmem = nullptr;
    double result = -1.0;
    if (SUCCEEDED(g_device->CreateRenderTarget(w, h, format, D3DMULTISAMPLE_NONE, 0, FALSE, &target, nullptr)) &&
        SUCCEEDED(g_device->CreateOffscreenPlainSurface(w, h, format, D3DPOOL_SYSTEMMEM, &sysmem, nullptr))) {
        double sum = 0;
        int counted = 0;
        for (int i = 0; i < 24; ++i) {
            g_device->ColorFill(target, nullptr, D3DCOLOR_XRGB(i * 10, 40, 80));
            Spin(4.0); // la carte a fini depuis longtemps
            const double t0 = NowMs();
            const HRESULT hr = g_device->GetRenderTargetData(target, sysmem);
            D3DLOCKED_RECT locked{};
            if (SUCCEEDED(hr) && SUCCEEDED(sysmem->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
                g_checksum += static_cast<const uint8_t*>(locked.pBits)[0];
                sysmem->UnlockRect();
            }
            if (i >= 4 && SUCCEEDED(hr)) {
                sum += NowMs() - t0;
                ++counted;
            }
        }
        if (counted)
            result = sum / counted;
    }
    if (target)
        target->Release();
    if (sysmem)
        sysmem->Release();
    return result;
}

} // namespace

int main(int argc, char** argv)
{
    QueryPerformanceFrequency(&g_frequency);
    vrlog::Init(nullptr, "capture_bench.log");
    config::Init(nullptr);
    if (argc > 2) {
        g_width = static_cast<UINT>(atoi(argv[1]));
        g_height = static_cast<UINT>(atoi(argv[2]));
    }
    if (argc > 3)
        g_cpuMs = atof(argv[3]);

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

    // Copie en mémoire seule (ce que coûte la recopie de l'image hors de la surface)
    {
        const size_t bytes = static_cast<size_t>(g_width) * g_height * 4;
        std::vector<uint8_t> source(bytes, 0x55), copy(bytes);
        const double t0 = NowMs();
        for (int i = 0; i < 20; ++i)
            memcpy(copy.data(), source.data(), bytes);
        g_checksum += copy[bytes / 2];
        printf("copie de l'image en memoire : %.2f ms\n", (NowMs() - t0) / 20);
    }

    // Coût d'une relecture selon la taille et le format (carte au repos)
    printf("\nrelecture seule (GetRenderTargetData + LockRect), carte au repos :\n");
    struct Case {
        UINT w, h;
        D3DFORMAT format;
        const char* name;
    };
    const Case cases[] = {
        {g_width / 2, g_height / 2, D3DFMT_A8R8G8B8, "quart d'image, 32 bits"},
        {g_width * 3 / 4, g_height, D3DFMT_A8R8G8B8, "3/4 de la largeur, 32 bits"},
        {g_width, g_height, D3DFMT_A8R8G8B8, "image entiere, 32 bits"},
        {g_width, g_height, D3DFMT_X8R8G8B8, "image entiere, 32 bits sans alpha"},
        {g_width, g_height, D3DFMT_R5G6B5, "image entiere, 16 bits"},
        {g_width * 2, g_height, D3DFMT_A8R8G8B8, "deux images cote a cote, 32 bits"},
        {g_width * 2, g_height, D3DFMT_R5G6B5, "deux images cote a cote, 16 bits"},
    };
    for (const Case& c : cases) {
        const double ms = ReadbackMs(c.w, c.h, c.format);
        const double megabytes = static_cast<double>(c.w) * c.h * (c.format == D3DFMT_R5G6B5 ? 2 : 4) / 1048576.0;
        if (ms < 0)
            printf("  %-36s %4ux%-4u : non gere\n", c.name, c.w, c.h);
        else
            printf("  %-36s %4ux%-4u %5.1f Mo : %6.3f ms (%.2f ms par Mo)\n", c.name, c.w, c.h, megabytes, ms, ms / megabytes);
        fflush(stdout);
    }

    // Les trois modes de [Display] async_capture, par le vrai code de capture. Deux cas : le
    // thread du rendu n'arrête jamais d'appeler Direct3D (le rendu limite la cadence), ou il
    // attend la simulation 8 ms après chaque paire (la simulation limite la cadence).
    g_calls = argc > 4 ? atoi(argv[4]) : 3000;
    if (g_calls > 20000)
        g_calls = 20000;
    static const char* const kModes[3] = {"0 synchrone", "1 asynchrone", "2 autre thread"};
    for (int scenario = 0; scenario < 2; ++scenario) {
        const double waitMs = scenario ? 8.0 : 0.0;
        printf("\n--- paires d'images, %d appels de dessin par image, %s ---\n", g_calls,
               scenario ? "le rendu attend la simulation 8 ms par paire" : "le rendu ne s'arrete jamais");
        printf("%-15s %9s %10s %11s %9s %11s %7s\n", "mode", "paire ms", "paires/s", "capture ms", "Present", "lecture ms",
               "recues");
        for (int mode = 0; mode < 3; ++mode) {
            const Result r = Run(mode, 100, waitMs);
            printf("%-15s %9.2f %10.1f %11.3f %9.3f %11.3f %7u\n", kModes[mode], r.pairMs, 1000.0 / r.pairMs, r.captureMs,
                   r.presentMs, r.workerMs, r.pairs);
            fflush(stdout);
        }
    }
    render::StopCaptureWorker();
    render::ReleaseCaptureResources();
    DeleteFileA(config::Path());
    printf("\n(somme de controle %llu)\n", g_checksum);
    return 0;
}
