// Session OpenXR sur un device D3D11 dédié, dans un processus 32 bits.
//
// Jalon 3 : l'image du jeu est affichée sur un quad (écran virtuel) fixe dans l'espace
// LOCAL, à 2 m devant la position de la tête au démarrage. Même image pour les deux yeux.
// Tout tourne dans un thread à part : xrWaitFrame cadence ce thread au rythme du casque
// sans ralentir le jeu. Repris du jalon 4 de WarbandVR ; l'image arrive par la mémoire
// (render/frame_capture) au lieu d'une texture partagée.
#include "xr/xr_session.h"
#include "input/keys.h"
#include "render/frame_capture.h"
#include "util/log.h"
#include "xr/xr_input.h"

#include <dxgi1_2.h>
#include <openxr/openxr_platform.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace xr {
namespace {

// Écran virtuel : largeur en mètres et distance devant l'utilisateur
constexpr float kScreenWidth = 2.4f;
constexpr float kScreenDistance = 2.0f;
constexpr ULONGLONG kStatsIntervalMs = 10000;

std::atomic<bool> g_stop{false};

// Dernière orientation du casque (quaternion x, y, z, w), écrite par le thread XR
std::mutex g_headMutex;
float g_headQuat[4]{0.0f, 0.0f, 0.0f, 1.0f};
ULONGLONG g_headTick = 0;

// --- Chien de garde : signale un appel bloquant qui ne rend pas la main -------------

constexpr ULONGLONG kStuckMs = 3000;
std::atomic<const char*> g_phase{nullptr}; // appel en cours dans le thread XR
std::atomic<ULONGLONG> g_phaseSince{0};

struct Phase {
    explicit Phase(const char* name)
    {
        g_phaseSince = GetTickCount64();
        g_phase = name;
    }
    ~Phase() { g_phase = nullptr; }
};

void Watchdog()
{
    const char* stuckIn = nullptr;
    while (!g_stop) {
        Sleep(1000);
        const char* phase = g_phase;
        const ULONGLONG since = g_phaseSince;
        if (phase && GetTickCount64() - since > kStuckMs) {
            if (stuckIn != phase)
                LOG_XR("ALERTE : thread XR bloque dans %s depuis %llu ms", phase, GetTickCount64() - since);
            stuckIn = phase;
        } else if (stuckIn) {
            LOG_XR("thread XR debloque (etait dans %s)", stuckIn);
            stuckIn = nullptr;
        }
    }
}

const char* StateName(XrSessionState s)
{
    switch (s) {
    case XR_SESSION_STATE_IDLE: return "IDLE";
    case XR_SESSION_STATE_READY: return "READY";
    case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
    case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
    case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
    case XR_SESSION_STATE_STOPPING: return "STOPPING";
    case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
    case XR_SESSION_STATE_EXITING: return "EXITING";
    default: return "UNKNOWN";
    }
}

// --- Runtime OpenXR 32 bits ---------------------------------------------------------

// Runtime actif inscrit dans le registre, vue 32 bits (celle que lit le chargeur d'un
// processus 32 bits) ou vue 64 bits. "" si absent.
std::string ActiveRuntime(HKEY hive, REGSAM view)
{
    HKEY key = nullptr;
    if (RegOpenKeyExA(hive, "SOFTWARE\\Khronos\\OpenXR\\1", 0, KEY_QUERY_VALUE | view, &key) != ERROR_SUCCESS)
        return {};
    char value[MAX_PATH * 2]{};
    DWORD size = sizeof(value) - 1, type = 0;
    const LSTATUS status =
        RegQueryValueExA(key, "ActiveRuntime", nullptr, &type, reinterpret_cast<BYTE*>(value), &size);
    RegCloseKey(key);
    return status == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) ? std::string(value) : std::string();
}

// Le chargeur OpenXR lit HKEY_CURRENT_USER avant HKEY_LOCAL_MACHINE : on fait de même
std::string ActiveRuntime(REGSAM view, const char** where)
{
    std::string value = ActiveRuntime(HKEY_CURRENT_USER, view);
    *where = "HKCU";
    if (value.empty()) {
        value = ActiveRuntime(HKEY_LOCAL_MACHINE, view);
        *where = "HKLM";
    }
    return value;
}

bool FileExists(const std::string& path)
{
    const DWORD attributes = GetFileAttributesA(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// Le jeu est en 32 bits : il lui faut le manifeste 32 bits du runtime. S'il n'est pas
// inscrit alors que le runtime 64 bits l'est, on cherche son jumeau à côté
// (steamxr_win64.json -> win32) et on le désigne au chargeur par XR_RUNTIME_JSON, pour ce
// processus seulement.
//
// Le runtime 32 bits de Meta (oculus_openxr_32.json, constaté avec Oculus 1.208) plante dans
// RuntimeIPCServiceClient_32.dll à la création de la session, même dans un programme minimal
// (tools/xr_probe) : on refuse de le charger, sauf s'il est imposé par XR_RUNTIME_JSON
// (injecteur : --default-runtime). L'injecteur choisit alors SteamVR, qui pilote aussi les
// casques Meta. false : ne pas démarrer OpenXR.
bool SelectRuntime()
{
    char fromEnv[MAX_PATH * 2]{};
    const bool hasEnv = GetEnvironmentVariableA("XR_RUNTIME_JSON", fromEnv, sizeof(fromEnv)) > 0;
    const char* where32 = "";
    const char* where64 = "";
    const std::string view32 = ActiveRuntime(KEY_WOW64_32KEY, &where32);
    const std::string view64 = ActiveRuntime(KEY_WOW64_64KEY, &where64);
    LOG_XR("runtime actif 32 bits : %s (%s)", view32.empty() ? "aucun" : view32.c_str(), view32.empty() ? "-" : where32);
    LOG_XR("runtime actif 64 bits : %s (%s)", view64.empty() ? "aucun" : view64.c_str(), view64.empty() ? "-" : where64);
    if (hasEnv) {
        LOG_XR("XR_RUNTIME_JSON impose : %s%s", fromEnv, FileExists(fromEnv) ? "" : " (FICHIER ABSENT)");
        return true;
    }
    std::string chosen = view32;
    if (view32.empty() && !view64.empty()) {
        const size_t slash = view64.find_last_of("\\/");
        const size_t pos = view64.rfind("64");
        if (pos == std::string::npos || (slash != std::string::npos && pos < slash)) {
            LOG_XR("pas de runtime 32 bits inscrit, et pas de jumeau 32 bits devinable");
            return true; // le chargeur dira lui-même qu'il n'y a pas de runtime
        }
        std::string twin = view64;
        twin.replace(pos, 2, "32");
        if (!FileExists(twin)) {
            LOG_XR("pas de runtime 32 bits inscrit ; jumeau absent : %s", twin.c_str());
            return true;
        }
        SetEnvironmentVariableA("XR_RUNTIME_JSON", twin.c_str());
        LOG_XR("pas de runtime 32 bits inscrit : repli sur %s (XR_RUNTIME_JSON)", twin.c_str());
        chosen = twin;
    }
    std::string lower = chosen;
    for (char& c : lower)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (lower.find("oculus_openxr_32") != std::string::npos) {
        LOG_XR("ERREUR : le runtime actif est celui de Meta en 32 bits, qui plante a la creation de la session. "
               "Lancer avec SteamVR (injecteur : --steamvr, automatique si SteamVR est installe) ; "
               "--default-runtime pour l'essayer quand meme");
        return false;
    }
    return true;
}

class XrApp {
public:
    ~XrApp() { Shutdown(); }
    void Run();

private:
    bool Check(XrResult r, const char* what);
    bool CreateInstance();
    bool WaitForSystem();
    bool CreateDevice();
    bool CreateSession();
    bool EnsureSwapchain(UINT w, UINT h);
    void DestroySwapchain();
    void PollEvents();
    void RenderFrame();
    bool CopyGameFrame();
    void Shutdown();

    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId system_ = XR_NULL_SYSTEM_ID;
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace localSpace_ = XR_NULL_HANDLE;
    XrSpace viewSpace_ = XR_NULL_HANDLE;
    XrSessionState state_ = XR_SESSION_STATE_UNKNOWN;
    bool running_ = false;
    bool exitRequested_ = false;
    bool quietErrors_ = false; // nouveaux essais de création de l'instance : sans log

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;

    XrSwapchain swapchain_ = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> images_;
    int64_t format_ = 0;
    UINT scWidth_ = 0;
    UINT scHeight_ = 0;
    bool hasContent_ = false; // au moins une image du jeu a été copiée dans la swapchain

    render::CpuFrame frame_; // dernière image du jeu reçue
    MenuInput input_;        // manettes -> touches des menus

    unsigned long long framesSubmitted_ = 0;
    unsigned long long framesCopied_ = 0;
    unsigned long long submittedAtStats_ = 0;
    unsigned long long copiedAtStats_ = 0;
    ULONGLONG lastStats_ = 0;
    int statsLogged_ = 0;
};

bool XrApp::Check(XrResult r, const char* what)
{
    if (XR_SUCCEEDED(r))
        return true;
    if (quietErrors_)
        return false;
    char name[XR_MAX_RESULT_STRING_SIZE]{};
    if (instance_ == XR_NULL_HANDLE || XR_FAILED(xrResultToString(instance_, r, name)))
        snprintf(name, sizeof(name), "%d", static_cast<int>(r));
    LOG_XR("ERREUR : %s -> %s", what, name);
    return false;
}

bool XrApp::CreateInstance()
{
    uint32_t count = 0;
    {
        Phase p("xrEnumerateInstanceExtensionProperties");
        if (!Check(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr), "enumeration des extensions"))
            return false;
    }
    std::vector<XrExtensionProperties> exts(count, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, count, &count, exts.data());
    bool hasD3D11 = false;
    for (const auto& e : exts)
        hasD3D11 |= strcmp(e.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0;
    if (!hasD3D11) {
        if (!quietErrors_)
            LOG_XR("ERREUR : le runtime ne propose pas %s (%u extensions)", XR_KHR_D3D11_ENABLE_EXTENSION_NAME, count);
        return false;
    }

    const char* enabled[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(ci.applicationInfo.applicationName, "SR2VR");
    strcpy_s(ci.applicationInfo.engineName, "Saints Row 2");
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = 1;
    ci.enabledExtensionNames = enabled;
    {
        Phase p("xrCreateInstance");
        if (!Check(xrCreateInstance(&ci, &instance_), "xrCreateInstance"))
            return false;
    }

    XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(instance_, &props);
    LOG_XR("runtime : %s %u.%u.%u", props.runtimeName, XR_VERSION_MAJOR(props.runtimeVersion),
           XR_VERSION_MINOR(props.runtimeVersion), XR_VERSION_PATCH(props.runtimeVersion));
    return true;
}

// Le casque peut ne pas être encore branché / Link inactif : on réessaie
bool XrApp::WaitForSystem()
{
    XrSystemGetInfo gi{XR_TYPE_SYSTEM_GET_INFO};
    gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    bool warned = false;
    while (!g_stop) {
        XrResult r = xrGetSystem(instance_, &gi, &system_);
        if (XR_SUCCEEDED(r))
            break;
        if (r != XR_ERROR_FORM_FACTOR_UNAVAILABLE)
            return Check(r, "xrGetSystem");
        if (!warned) {
            LOG_XR("casque non disponible (Link / SteamVR actif ?), nouvel essai toutes les 3 s");
            warned = true;
        }
        Sleep(3000);
    }
    if (g_stop)
        return false;

    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(instance_, system_, &sp);
    LOG_XR("casque : %s (vendor 0x%X), %u calques max, image max %ux%u", sp.systemName, sp.vendorId,
           sp.graphicsProperties.maxLayerCount, sp.graphicsProperties.maxSwapchainImageWidth,
           sp.graphicsProperties.maxSwapchainImageHeight);

    // Résolution recommandée par œil : servira au rendu stéréo (jalon 5)
    uint32_t viewCount = 0;
    xrEnumerateViewConfigurationViews(instance_, system_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount,
                                      nullptr);
    std::vector<XrViewConfigurationView> views(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    if (viewCount && XR_SUCCEEDED(xrEnumerateViewConfigurationViews(instance_, system_,
                                                                    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                                    viewCount, &viewCount, views.data())))
        LOG_XR("  %u vues, resolution recommandee par oeil %ux%u (max %ux%u)", viewCount,
               views[0].recommendedImageRectWidth, views[0].recommendedImageRectHeight,
               views[0].maxImageRectWidth, views[0].maxImageRectHeight);
    return true;
}

bool XrApp::CreateDevice()
{
    auto getReqs = PFN_xrGetD3D11GraphicsRequirementsKHR{};
    if (!Check(xrGetInstanceProcAddr(instance_, "xrGetD3D11GraphicsRequirementsKHR",
                                     reinterpret_cast<PFN_xrVoidFunction*>(&getReqs)),
               "xrGetInstanceProcAddr(xrGetD3D11GraphicsRequirementsKHR)"))
        return false;
    XrGraphicsRequirementsD3D11KHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    if (!Check(getReqs(instance_, system_, &reqs), "xrGetD3D11GraphicsRequirementsKHR"))
        return false;

    // Le device D3D11 doit être sur l'adaptateur imposé par le runtime
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        LOG_XR("ERREUR : CreateDXGIFactory1");
        return false;
    }
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{};
        adapter->GetDesc1(&d);
        if (memcmp(&d.AdapterLuid, &reqs.adapterLuid, sizeof(LUID)) == 0) {
            char name[128]{};
            WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof(name), nullptr, nullptr);
            LOG_XR("adaptateur du casque : %s", name);
            break;
        }
        adapter->Release();
        adapter = nullptr;
    }
    factory->Release();
    if (!adapter) {
        LOG_XR("ERREUR : adaptateur demande par le runtime introuvable");
        return false;
    }

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                   static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &device_, &got, &context_);
    adapter->Release();
    if (FAILED(hr) || got < reqs.minFeatureLevel) {
        LOG_XR("ERREUR : D3D11CreateDevice 0x%08lX (niveau 0x%X, minimum 0x%X)", static_cast<unsigned long>(hr), got,
               reqs.minFeatureLevel);
        return false;
    }
    LOG_XR("device D3D11 cree (niveau 0x%X)", got);
    return true;
}

bool XrApp::CreateSession()
{
    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = device_;
    XrSessionCreateInfo ci{XR_TYPE_SESSION_CREATE_INFO};
    ci.next = &binding;
    ci.systemId = system_;
    {
        Phase p("xrCreateSession");
        if (!Check(xrCreateSession(instance_, &ci, &session_), "xrCreateSession"))
            return false;
    }

    XrReferenceSpaceCreateInfo si{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    si.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    si.poseInReferenceSpace.orientation.w = 1.0f;
    if (!Check(xrCreateReferenceSpace(session_, &si, &localSpace_), "xrCreateReferenceSpace(LOCAL)"))
        return false;
    si.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (!Check(xrCreateReferenceSpace(session_, &si, &viewSpace_), "xrCreateReferenceSpace(VIEW)"))
        return false;
    input_.Init(instance_, session_);

    // Format : sRGB en priorité, car le jeu écrit des valeurs déjà encodées en sRGB
    uint32_t count = 0;
    xrEnumerateSwapchainFormats(session_, 0, &count, nullptr);
    std::vector<int64_t> formats(count);
    xrEnumerateSwapchainFormats(session_, count, &count, formats.data());
    for (int64_t wanted : {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM}) {
        for (int64_t f : formats)
            if (f == wanted && !format_)
                format_ = f;
    }
    if (!format_) {
        LOG_XR("ERREUR : aucun format BGRA propose par le runtime (%u formats)", count);
        return false;
    }
    LOG_XR("session creee, format de swapchain %lld%s", static_cast<long long>(format_),
           format_ == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ? " (BGRA sRGB)" : " (BGRA, couleurs possiblement fausses)");
    return true;
}

bool XrApp::EnsureSwapchain(UINT w, UINT h)
{
    if (swapchain_ != XR_NULL_HANDLE && w == scWidth_ && h == scHeight_)
        return true;
    DestroySwapchain();

    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = format_;
    ci.sampleCount = 1;
    ci.width = w;
    ci.height = h;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    if (!Check(xrCreateSwapchain(session_, &ci, &swapchain_), "xrCreateSwapchain"))
        return false;

    uint32_t count = 0;
    xrEnumerateSwapchainImages(swapchain_, 0, &count, nullptr);
    images_.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    xrEnumerateSwapchainImages(swapchain_, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images_.data()));
    scWidth_ = w;
    scHeight_ = h;
    LOG_XR("swapchain %ux%u creee (%u images)", w, h, count);
    return true;
}

void XrApp::DestroySwapchain()
{
    if (swapchain_ != XR_NULL_HANDLE)
        xrDestroySwapchain(swapchain_);
    swapchain_ = XR_NULL_HANDLE;
    images_.clear();
    scWidth_ = scHeight_ = 0;
    hasContent_ = false;
}

void XrApp::PollEvents()
{
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(instance_, &ev) == XR_SUCCESS) {
        switch (ev.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            const auto& e = reinterpret_cast<const XrEventDataSessionStateChanged&>(ev);
            state_ = e.state;
            LOG_XR("etat de session : %s", StateName(state_));
            if (state_ == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                running_ = Check(xrBeginSession(session_, &bi), "xrBeginSession");
                if (running_)
                    LOG_XR("session demarree");
            } else if (state_ == XR_SESSION_STATE_STOPPING) {
                xrEndSession(session_);
                running_ = false;
                LOG_XR("session arretee");
            } else if (state_ == XR_SESSION_STATE_EXITING || state_ == XR_SESSION_STATE_LOSS_PENDING) {
                exitRequested_ = true;
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            LOG_XR("perte de l'instance OpenXR annoncee");
            exitRequested_ = true;
            break;
        default:
            break;
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

// Envoie la dernière image du jeu dans la swapchain. true si une nouvelle image a été copiée.
bool XrApp::CopyGameFrame()
{
    if (!render::TakeLatestFrame(frame_) || !EnsureSwapchain(frame_.width, frame_.height))
        return false;

    bool copied = false;
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    bool acquired = false;
    {
        Phase p("xrAcquireSwapchainImage");
        acquired = Check(xrAcquireSwapchainImage(swapchain_, &ai, &index), "xrAcquireSwapchainImage");
    }
    if (acquired) {
        {
            Phase p("xrWaitSwapchainImage");
            acquired = Check(xrWaitSwapchainImage(swapchain_, &wi), "xrWaitSwapchainImage");
        }
        if (acquired) {
            context_->UpdateSubresource(images_[index].texture, 0, nullptr, frame_.pixels.data(), frame_.width * 4, 0);
            copied = true;
        }
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        Phase p("xrReleaseSwapchainImage");
        xrReleaseSwapchainImage(swapchain_, &ri);
    }
    return copied;
}

void XrApp::RenderFrame()
{
    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    {
        Phase p("xrWaitFrame");
        if (!Check(xrWaitFrame(session_, &wi, &fs), "xrWaitFrame"))
            return;
    }
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    {
        Phase p("xrBeginFrame");
        if (!Check(xrBeginFrame(session_, &bi), "xrBeginFrame"))
            return;
    }

    // Orientation de la tête à l'instant d'affichage prévu, pour la caméra du jeu. Seulement
    // casque sur la tête (shouldRender) : posé, il reste suivi et ferait regarder le sol.
    XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
    const bool located = fs.shouldRender &&
                         XR_SUCCEEDED(xrLocateSpace(viewSpace_, localSpace_, fs.predictedDisplayTime, &head)) &&
                         (head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
    {
        std::lock_guard lock(g_headMutex);
        if (located) {
            g_headQuat[0] = head.pose.orientation.x;
            g_headQuat[1] = head.pose.orientation.y;
            g_headQuat[2] = head.pose.orientation.z;
            g_headQuat[3] = head.pose.orientation.w;
            g_headTick = GetTickCount64();
        } else {
            g_headTick = 0;
        }
    }

    input_.Update(session_, state_ == XR_SESSION_STATE_FOCUSED);

    if (fs.shouldRender) {
        if (CopyGameFrame()) {
            hasContent_ = true;
            ++framesCopied_;
        }
        // Image suivante : capturée par le jeu à son prochain Present
        render::RequestFrame();
    }

    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    const XrCompositionLayerBaseHeader* layers[1]{};
    uint32_t layerCount = 0;
    if (fs.shouldRender && hasContent_) {
        quad.space = localSpace_;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = swapchain_;
        quad.subImage.imageRect = {{0, 0}, {static_cast<int32_t>(scWidth_), static_cast<int32_t>(scHeight_)}};
        quad.pose.orientation.w = 1.0f;
        quad.pose.position = {0.0f, 0.0f, -kScreenDistance};
        quad.size = {kScreenWidth, kScreenWidth * static_cast<float>(scHeight_) / static_cast<float>(scWidth_)};
        layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
    }

    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = layerCount;
    ei.layers = layers;
    {
        Phase p("xrEndFrame");
        if (!Check(xrEndFrame(session_, &ei), "xrEndFrame"))
            return;
    }
    // Hors affichage (SYNCHRONIZED), certains runtimes ne cadencent pas xrWaitFrame :
    // on évite de tourner à vide
    if (!fs.shouldRender)
        Sleep(5);

    ++framesSubmitted_;
    const ULONGLONG now = GetTickCount64();
    if (framesSubmitted_ == 1) {
        LOG_XR("premiere image soumise au casque (periode d'affichage %.2f ms)",
               static_cast<double>(fs.predictedDisplayPeriod) / 1e6);
        lastStats_ = now;
    } else if (now - lastStats_ >= kStatsIntervalMs) {
        const double seconds = static_cast<double>(now - lastStats_) / 1000.0;
        unsigned captures = 0;
        double captureMs = 0.0;
        render::TakeCaptureStats(&captures, &captureMs);
        // Relevé toutes les 10 s pendant 5 min, puis toutes les minutes
        if (statsLogged_ < 30 || statsLogged_ % 6 == 0)
            LOG_XR("casque : %.1f images par seconde soumises, %.1f images du jeu par seconde (capture %.2f ms en "
                   "moyenne), etat %s, %u touche(s) envoyee(s)",
                   static_cast<double>(framesSubmitted_ - submittedAtStats_) / seconds,
                   static_cast<double>(framesCopied_ - copiedAtStats_) / seconds, captureMs, StateName(state_),
                   input::TakeSentCount());
        ++statsLogged_;
        submittedAtStats_ = framesSubmitted_;
        copiedAtStats_ = framesCopied_;
        lastStats_ = now;
    }
}

void XrApp::Shutdown()
{
    input_.ReleaseAll();
    DestroySwapchain();
    if (viewSpace_ != XR_NULL_HANDLE)
        xrDestroySpace(viewSpace_);
    viewSpace_ = XR_NULL_HANDLE;
    if (localSpace_ != XR_NULL_HANDLE)
        xrDestroySpace(localSpace_);
    if (session_ != XR_NULL_HANDLE)
        xrDestroySession(session_);
    if (instance_ != XR_NULL_HANDLE)
        xrDestroyInstance(instance_);
    localSpace_ = XR_NULL_HANDLE;
    session_ = XR_NULL_HANDLE;
    instance_ = XR_NULL_HANDLE;
    if (context_)
        context_->Release();
    if (device_)
        device_->Release();
    context_ = nullptr;
    device_ = nullptr;
}

void XrApp::Run()
{
    if (!SelectRuntime()) {
        LOG_XR("OpenXR indisponible : le jeu continue sans VR");
        return;
    }
    // Runtime pas encore prêt (SteamVR sans casque connecté : xrCreateInstance échoue avec
    // XR_ERROR_RUNTIME_FAILURE après quelques secondes) : on réessaie au lieu d'abandonner la
    // VR pour toute la partie
    int attempts = 0;
    while (!g_stop && !CreateInstance()) {
        if (attempts++ == 0)
            LOG_XR("runtime OpenXR pas pret (casque connecte ? Link actif ?), nouvel essai toutes les 3 s");
        quietErrors_ = true;
        Sleep(3000);
    }
    quietErrors_ = false;
    if (attempts && !g_stop)
        LOG_XR("runtime OpenXR pret apres %d essai(s) infructueux", attempts);
    if (g_stop || !WaitForSystem() || !CreateDevice() || !CreateSession()) {
        LOG_XR("OpenXR indisponible : le jeu continue sans VR");
        return;
    }
    LOG_XR("en attente de l'etat READY (mettre le casque)");
    while (!g_stop && !exitRequested_) {
        {
            Phase p("xrPollEvent");
            PollEvents();
        }
        if (running_)
            RenderFrame();
        else
            Sleep(50);
    }
    if (running_)
        xrEndSession(session_);
    LOG_XR("fin de la session OpenXR");
}

} // namespace

void Start()
{
    std::thread(Watchdog).detach();
    std::thread([] {
        XrApp app;
        app.Run();
    }).detach();
}

void RequestStop()
{
    g_stop = true;
}

bool GetHeadRotation(float out[9])
{
    float x, y, z, w;
    {
        std::lock_guard lock(g_headMutex);
        if (!g_headTick || GetTickCount64() - g_headTick > 500)
            return false;
        x = g_headQuat[0];
        y = g_headQuat[1];
        z = g_headQuat[2];
        w = g_headQuat[3];
    }
    out[0] = 1.0f - 2.0f * (y * y + z * z);
    out[1] = 2.0f * (x * y - z * w);
    out[2] = 2.0f * (x * z + y * w);
    out[3] = 2.0f * (x * y + z * w);
    out[4] = 1.0f - 2.0f * (x * x + z * z);
    out[5] = 2.0f * (y * z - x * w);
    out[6] = 2.0f * (x * z - y * w);
    out[7] = 2.0f * (y * z + x * w);
    out[8] = 1.0f - 2.0f * (x * x + y * y);
    return true;
}

} // namespace xr
