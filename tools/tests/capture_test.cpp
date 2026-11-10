// SR2VR - test de render/frame_capture sur un vrai device Direct3D 9 (fenêtre cachée).
// Chaque image est remplie d'une couleur qui encode son numéro, porté aussi par sa vue
// (headQuat[0]). Les images publiées doivent avoir la couleur de leur vue (pas de mélange
// entre une image et la vue d'une autre), les paires d'yeux doivent arriver complètes et
// dans l'ordre. Lancé dans les trois modes de [Display] async_capture (synchrone, asynchrone,
// lecture par un autre thread), avec un Reset du device au milieu de chacun. Code de sortie :
// nombre d'erreurs.
#include <windows.h>
#include <d3d9.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "render/frame_capture.h"
#include "util/config.h"
#include "util/log.h"

namespace {

int g_errors = 0;
render::CaptureStats g_ignored;
int g_workMs = 0; // travail simulé du jeu par image (argument), le GPU a le temps de finir

D3DCOLOR ColorOf(unsigned id)
{
    return D3DCOLOR_XRGB(id & 0xFF, (id >> 8) & 0xFF, 0x80);
}

unsigned IdOfPixels(const render::CpuFrame& f)
{
    const uint8_t* p = f.pixels.data() + (static_cast<size_t>(f.height / 2) * f.width + f.width / 2) * 4; // BGRA
    return p[2] | (p[1] << 8);
}

int RunMode(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS pp, int async)
{
    char value[4];
    snprintf(value, sizeof(value), "%d", async);
    WritePrivateProfileStringA("Display", "async_capture", value, config::Path());
    Sleep(1100); // la capture relit ses réglages une fois par seconde

    // Quelques images sans capture : les copies encore en vol du passage précédent arrivent
    for (int i = 0; i < 5; ++i) {
        device->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
        render::CaptureFrame(device, render::FrameView{});
        device->Present(nullptr, nullptr, nullptr, nullptr);
    }
    render::CpuFrame left, right;
    bool drained[2]{};
    render::TakeLatestFrames(left, right, drained); // images des passages précédents
    render::TakeCaptureStats(&g_ignored);
    unsigned published = 0, pairsOk = 0, lastLeft = 0;
    int errors = 0;
    unsigned pair = 0;
    for (unsigned id = 1; id <= 240; ++id) {
        if (id == 120) {
            // Reset au milieu : les surfaces de la capture doivent être libérées avant
            render::ReleaseCaptureResources();
            const HRESULT hr = device->Reset(&pp);
            if (FAILED(hr)) {
                printf("  ERREUR : Reset en echec 0x%08lX (surfaces non liberees ?)\n", static_cast<unsigned long>(hr));
                ++errors;
            }
        }
        // Images 1 à 239 : paires (œil gauche impair, œil droit pair) ; une image seule sur 10
        render::FrameView view;
        view.valid = true;
        view.headQuat[0] = static_cast<float>(id);
        const bool single = id % 20 == 0 || id % 20 == 19;
        if (!single) {
            if (id % 2 == 1) {
                pair = id;
                view.eye = 0;
            } else {
                view.eye = 1;
            }
            view.pair = pair;
        }
        device->Clear(0, nullptr, D3DCLEAR_TARGET, ColorOf(id), 1.0f, 0);
        device->BeginScene();
        device->EndScene();
        if (view.pair == 0 || view.eye == 0)
            render::RequestFrame();
        render::CaptureFrame(device, view);
        device->Present(nullptr, nullptr, nullptr, nullptr);
        if (g_workMs)
            Sleep(static_cast<DWORD>(g_workMs));

        bool fresh[2]{};
        render::TakeLatestFrames(left, right, fresh);
        for (int eye = 0; eye < 2; ++eye) {
            if (!fresh[eye])
                continue;
            const render::CpuFrame& f = eye ? right : left;
            const unsigned viewId = static_cast<unsigned>(f.view.headQuat[0]);
            const unsigned pixelId = IdOfPixels(f);
            ++published;
            if (pixelId != (viewId & 0xFFFF)) {
                if (errors < 10)
                    printf("  ERREUR : oeil %d, vue de l'image %u sur les pixels de l'image %u\n", eye, viewId, pixelId);
                ++errors;
            }
        }
        if (fresh[0] && fresh[1] && left.view.pair) {
            if (left.view.pair != right.view.pair || left.view.eye != 0 || right.view.eye != 1) {
                printf("  ERREUR : paire melangee (%u / %u)\n", left.view.pair, right.view.pair);
                ++errors;
            } else {
                ++pairsOk;
            }
        } else if (fresh[1] != fresh[0] && (fresh[1] ? right.view.pair : left.view.pair)) {
            printf("  ERREUR : demi-paire publiee\n");
            ++errors;
        }
        if (fresh[0]) {
            const unsigned id0 = static_cast<unsigned>(left.view.headQuat[0]);
            if (id0 < lastLeft) {
                printf("  ERREUR : image %u publiee apres l'image %u\n", id0, lastLeft);
                ++errors;
            }
            lastLeft = id0;
        }
    }
    render::CaptureStats stats;
    render::TakeCaptureStats(&stats);
    static const char* const kModes[3] = {"synchrone", "asynchrone", "autre thread"};
    printf("  mode %s : %u images publiees, %u paires completes, %u incompletes, %.3f ms par capture, "
           "%u asynchrones (%.2f Present de retard en moyenne, %u attendues)\n",
           kModes[async], published, pairsOk, stats.brokenPairs, stats.averageMs, stats.async,
           stats.averageLatency, stats.waits);
    if (published < 100 || pairsOk < 50) {
        printf("  ERREUR : trop peu d'images publiees\n");
        ++errors;
    }
    if (async && stats.async == 0) {
        printf("  ERREUR : la capture asynchrone n'a pas servi\n");
        ++errors;
    }
    return errors;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc > 1)
        g_workMs = atoi(argv[1]);
    vrlog::Init(nullptr, "capture_test.log");
    config::Init(nullptr);

    WNDCLASSA wc{};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "sr2vr_capture_test";
    RegisterClassA(&wc);
    HWND window = CreateWindowA(wc.lpszClassName, "capture", WS_OVERLAPPEDWINDOW, 0, 0, 640, 360, nullptr, nullptr,
                                wc.hInstance, nullptr);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!window || !d3d) {
        printf("pas de Direct3D 9 ici : test ignore\n");
        return 77;
    }
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = 640;
    pp.BackBufferHeight = 360;
    pp.hDeviceWindow = window;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9* device = nullptr;
    // MULTITHREADED comme le device du jeu : la lecture par un autre thread l'exige
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                                   D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED, &pp, &device);
    if (FAILED(hr))
        hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                               D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED, &pp, &device);
    if (FAILED(hr) || !device) {
        printf("device D3D9 non cree : 0x%08lX, test ignore\n", static_cast<unsigned long>(hr));
        return 77;
    }

    g_errors += RunMode(device, pp, 0);
    g_errors += RunMode(device, pp, 1);
    g_errors += RunMode(device, pp, 2);
    g_errors += RunMode(device, pp, 1); // d'un mode à l'autre en cours de route
    g_errors += RunMode(device, pp, 2);
    g_errors += RunMode(device, pp, 0);

    render::StopCaptureWorker();
    render::ReleaseCaptureResources();
    device->Release();
    d3d->Release();
    DeleteFileA(config::Path());
    printf(g_errors ? "%d ERREUR(S)\n" : "OK\n", g_errors);
    return g_errors;
}
