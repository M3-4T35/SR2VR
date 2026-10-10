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
#include "util/config.h"
#include "util/log.h"
#include "xr/xr_input.h"

#include <dxgi1_2.h>
#include <openxr/openxr_platform.h>

#include <atomic>
#include <cctype>
#include <cmath>
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
std::atomic<float> g_ipd{0.063f}; // écart entre les yeux mesuré par le casque (mètres)

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
    bool EnsureSwapchains(UINT w, UINT h);
    void DestroySwapchains();
    void PollEvents();
    void RenderFrame();
    bool CopyGameFrame(int eye);
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

    // Une cible par œil. Hors relief, tout passe par la première (écran virtuel, ou même
    // image pour les deux yeux).
    struct EyeTarget {
        XrSwapchain swapchain = XR_NULL_HANDLE;
        std::vector<XrSwapchainImageD3D11KHR> images;
        bool hasContent = false; // au moins une image du jeu a été copiée dans la swapchain
        ULONGLONG copiedTick = 0;
        render::CpuFrame frame;  // dernière image du jeu reçue pour cet œil, avec sa vue
    };
    EyeTarget eyes_[2];
    int64_t format_ = 0;
    UINT scWidth_ = 0;
    UINT scHeight_ = 0;

    MenuInput input_;        // manettes -> touches des menus

    unsigned long long framesSubmitted_ = 0;
    unsigned long long framesCopied_ = 0;
    unsigned long long submittedAtStats_ = 0;
    unsigned long long copiedAtStats_ = 0;
    ULONGLONG lastStats_ = 0;
    int statsLogged_ = 0;

    // Réglages [Display] de sr2_vr.ini, relus chaque seconde
    void ReadConfig();
    // 0 écran virtuel, 1 immersif (même image aux deux yeux), 2 relief un œil par image,
    // 3 vrai relief (deux images par image de simulation)
    int displayMode_ = 0;
    float fovScale_ = 1.0f;   // correction du champ de vue annoncé au casque
    ULONGLONG lastConfig_ = 0;
    int shownMode_ = -1;      // dernier affichage réellement utilisé (pour le log)
    ULONGLONG lastModeLog_ = 0;
    unsigned modeChanges_ = 0; // changements d'affichage depuis le dernier log
    unsigned shownCount_[3]{}; // images du casque par affichage (écran, immersif, relief) depuis le relevé

    // Écran virtuel : replacé devant la tête quand il réapparaît (sortie du jeu vers un menu)
    XrPosef headPose_{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
    bool headPoseValid_ = false;
    XrPosef screenPose_{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -kScreenDistance}};
    ULONGLONG screenShownTick_ = 0;
    void PlaceScreen();
};

void XrApp::ReadConfig()
{
    displayMode_ = config::Int("Display", "mode", 0);
    fovScale_ = config::Float("Display", "fov_scale", 1.0f);
    if (!(fovScale_ > 0.2f && fovScale_ < 5.0f))
        fovScale_ = 1.0f;
}

// Écran virtuel à kScreenDistance devant la tête, à sa hauteur, face à elle, vertical
void XrApp::PlaceScreen()
{
    if (!headPoseValid_)
        return;
    const XrQuaternionf& q = headPose_.orientation;
    // Avant de la tête (-z tourné), projeté à l'horizontale
    float fx = -2.0f * (q.x * q.z + q.y * q.w);
    float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    const float length = std::sqrt(fx * fx + fz * fz);
    if (length < 0.1f)
        return; // tête à la verticale : on garde la place précédente
    fx /= length;
    fz /= length;
    const float yaw = std::atan2(-fx, -fz); // rotation autour de +y qui amène -z sur cet avant
    screenPose_.orientation = {0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
    screenPose_.position = {headPose_.position.x + fx * kScreenDistance, headPose_.position.y,
                            headPose_.position.z + fz * kScreenDistance};
}

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

bool XrApp::EnsureSwapchains(UINT w, UINT h)
{
    if (eyes_[0].swapchain != XR_NULL_HANDLE && w == scWidth_ && h == scHeight_)
        return true;
    DestroySwapchains();

    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = format_;
    ci.sampleCount = 1;
    ci.width = w;
    ci.height = h;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    uint32_t count = 0;
    for (EyeTarget& eye : eyes_) {
        if (!Check(xrCreateSwapchain(session_, &ci, &eye.swapchain), "xrCreateSwapchain")) {
            DestroySwapchains();
            return false;
        }
        xrEnumerateSwapchainImages(eye.swapchain, 0, &count, nullptr);
        eye.images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        xrEnumerateSwapchainImages(eye.swapchain, count, &count,
                                   reinterpret_cast<XrSwapchainImageBaseHeader*>(eye.images.data()));
    }
    scWidth_ = w;
    scHeight_ = h;
    LOG_XR("2 swapchains %ux%u creees (%u images chacune)", w, h, count);
    return true;
}

void XrApp::DestroySwapchains()
{
    for (EyeTarget& eye : eyes_) {
        if (eye.swapchain != XR_NULL_HANDLE)
            xrDestroySwapchain(eye.swapchain);
        eye.swapchain = XR_NULL_HANDLE;
        eye.images.clear();
        eye.hasContent = false;
    }
    scWidth_ = scHeight_ = 0;
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

// Envoie l'image du jeu que cet œil vient de recevoir dans sa swapchain. true si elle a été
// copiée.
bool XrApp::CopyGameFrame(int eyeIndex)
{
    EyeTarget& eye = eyes_[eyeIndex];
    if (!EnsureSwapchains(eye.frame.width, eye.frame.height))
        return false;

    bool copied = false;
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    bool acquired = false;
    {
        Phase p("xrAcquireSwapchainImage");
        acquired = Check(xrAcquireSwapchainImage(eye.swapchain, &ai, &index), "xrAcquireSwapchainImage");
    }
    if (acquired) {
        {
            Phase p("xrWaitSwapchainImage");
            acquired = Check(xrWaitSwapchainImage(eye.swapchain, &wi), "xrWaitSwapchainImage");
        }
        if (acquired) {
            context_->UpdateSubresource(eye.images[index].texture, 0, nullptr, eye.frame.pixels.data(),
                                        eye.frame.width * 4, 0);
            copied = true;
        }
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        Phase p("xrReleaseSwapchainImage");
        xrReleaseSwapchainImage(eye.swapchain, &ri);
    }
    if (copied) {
        eye.hasContent = true;
        eye.copiedTick = GetTickCount64();
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
    headPoseValid_ = located && (head.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
    if (headPoseValid_)
        headPose_ = head.pose;
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

    const ULONGLONG tick = GetTickCount64();
    if (tick - lastConfig_ >= 1000) {
        lastConfig_ = tick;
        ReadConfig();
    }

    // Position des yeux à l'instant d'affichage : sert aux calques de projection, et l'écart
    // entre les yeux au décalage de la caméra du jeu en relief
    XrView views[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    bool viewsLocated = false;
    if (fs.shouldRender) {
        XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
        li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        li.displayTime = fs.predictedDisplayTime;
        li.space = localSpace_;
        XrViewState viewState{XR_TYPE_VIEW_STATE};
        uint32_t viewCount = 0;
        viewsLocated = XR_SUCCEEDED(xrLocateViews(session_, &li, &viewState, 2, &viewCount, views)) && viewCount == 2;
        if (viewsLocated) {
            const float dx = views[1].pose.position.x - views[0].pose.position.x;
            const float dy = views[1].pose.position.y - views[0].pose.position.y;
            const float dz = views[1].pose.position.z - views[0].pose.position.z;
            const float ipd = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (ipd > 0.04f && ipd < 0.09f)
                g_ipd = ipd;
        }

        // Les deux yeux d'un seul geste : jamais la moitié d'une paire
        bool fresh[2]{};
        render::TakeLatestFrames(eyes_[0].frame, eyes_[1].frame, fresh);
        for (int eye = 0; eye < 2; ++eye)
            if (fresh[eye] && CopyGameFrame(eye))
                ++framesCopied_;
        // Image suivante : capturée par le jeu à son prochain Present
        render::RequestFrame();
    }

    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView projected[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                  {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    const XrCompositionLayerBaseHeader* layers[1]{};
    uint32_t layerCount = 0;

    // Affichage : 2 = relief (une image par œil, rendue avec la caméra décalée), 1 = immersif
    // (la même image pour les deux yeux), 0 = écran virtuel. Les images de projection portent
    // l'orientation de tête qui a servi à les rendre et leur champ de vue : le compositeur les
    // recale sur la tête à chaque image du casque. Sans caméra de jeu (menu principal), ou tant
    // qu'un œil n'a pas d'image récente, on retombe sur le mode inférieur.
    const ULONGLONG nowTick = GetTickCount64(); // après les copies : jamais avant leur horodatage
    const auto usable = [&](const EyeTarget& eye, int wantedEye) {
        return eye.hasContent && eye.frame.view.valid && eye.frame.view.eye == wantedEye &&
               eye.frame.view.hfovDeg > 1.0f && nowTick - eye.copiedTick < 500;
    };
    int shown = 0;
    if (fs.shouldRender && viewsLocated && displayMode_ >= 2 && usable(eyes_[0], 0) && usable(eyes_[1], 1))
        shown = 2;
    else if (fs.shouldRender && viewsLocated && displayMode_ >= 1 && usable(eyes_[0], 0))
        shown = 1;

    if (shown) {
        float halfH = 0.0f, halfV = 0.0f;
        for (int i = 0; i < 2; ++i) {
            const EyeTarget& source = eyes_[shown == 2 ? i : 0];
            halfH = std::atan(std::tan(source.frame.view.hfovDeg * 0.5f * 0.01745329f) * fovScale_);
            halfV = std::atan(std::tan(halfH) * static_cast<float>(scHeight_) / static_cast<float>(scWidth_));
            projected[i].pose.position = views[i].pose.position;
            projected[i].pose.orientation = {source.frame.view.headQuat[0], source.frame.view.headQuat[1],
                                             source.frame.view.headQuat[2], source.frame.view.headQuat[3]};
            projected[i].fov = {-halfH, halfH, halfV, -halfV};
            projected[i].subImage.swapchain = source.swapchain;
            projected[i].subImage.imageRect = {{0, 0}, {static_cast<int32_t>(scWidth_), static_cast<int32_t>(scHeight_)}};
        }
        projection.space = localSpace_;
        projection.viewCount = 2;
        projection.views = projected;
        layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
        if (shownMode_ != shown && ++modeChanges_ && tick - lastModeLog_ >= 2000) {
            LOG_XR("affichage %s : champ de vue annonce %.1f x %.1f degres (echelle %.2f), ecart des yeux %.1f mm, "
                   "%u changement(s) d'affichage",
                   shown == 2 ? "en relief" : "immersif sans relief", halfH * 114.59156f, halfV * 114.59156f, fovScale_,
                   g_ipd.load() * 1000.0f, modeChanges_);
            lastModeLog_ = tick;
            modeChanges_ = 0;
        }
        shownMode_ = shown;
        ++shownCount_[shown];
    } else if (fs.shouldRender && eyes_[0].hasContent) {
        // L'écran revient après une vue immersive (ou pour la première fois) : devant la tête
        if (!screenShownTick_ || nowTick - screenShownTick_ > 500)
            PlaceScreen();
        screenShownTick_ = nowTick;
        ++shownCount_[0];
        if (shownMode_ != 0 && ++modeChanges_ && tick - lastModeLog_ >= 2000) {
            LOG_XR("affichage : ecran virtuel%s, %u changement(s) d'affichage",
                   displayMode_ >= 1 ? " (pas de camera de jeu pour cette image)" : "", modeChanges_);
            lastModeLog_ = tick;
            modeChanges_ = 0;
        }
        shownMode_ = 0;
        quad.space = localSpace_;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = eyes_[0].swapchain;
        quad.subImage.imageRect = {{0, 0}, {static_cast<int32_t>(scWidth_), static_cast<int32_t>(scHeight_)}};
        quad.pose = screenPose_;
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
        render::CaptureStats capture;
        render::TakeCaptureStats(&capture);
        // Relevé toutes les 10 s pendant 5 min, puis toutes les minutes
        if (statsLogged_ < 30 || statsLogged_ % 6 == 0)
            LOG_XR("casque : %.1f images par seconde soumises, %.1f images du jeu par seconde (capture %.2f ms en "
                   "moyenne), etat %s, %u touche(s) envoyee(s)",
                   static_cast<double>(framesSubmitted_ - submittedAtStats_) / seconds,
                   static_cast<double>(framesCopied_ - copiedAtStats_) / seconds, capture.averageMs, StateName(state_),
                   input::TakeSentCount());
        if (statsLogged_ < 30 || statsLogged_ % 6 == 0)
            LOG_XR("  affichage sur ces images : %u ecran virtuel, %u immersif, %u relief ; paires du jeu : %.1f par "
                   "seconde, %u incomplete(s)",
                   shownCount_[0], shownCount_[1], shownCount_[2], static_cast<double>(capture.pairs) / seconds,
                   capture.brokenPairs);
        if ((statsLogged_ < 30 || statsLogged_ % 6 == 0) && capture.eyeCount[0] + capture.eyeCount[1])
            LOG_XR("  images avec vue : oeil gauche %u (luminosite moyenne %.0f sur 255), oeil droit %u (luminosite "
                   "moyenne %.0f)",
                   capture.eyeCount[0], capture.eyeLuma[0], capture.eyeCount[1], capture.eyeLuma[1]);
        shownCount_[0] = shownCount_[1] = shownCount_[2] = 0;
        ++statsLogged_;
        submittedAtStats_ = framesSubmitted_;
        copiedAtStats_ = framesCopied_;
        lastStats_ = now;
    }
}

void XrApp::Shutdown()
{
    input_.ReleaseAll();
    DestroySwapchains();
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

bool GetHeadPose(float quat[4], float* ipd)
{
    if (ipd)
        *ipd = g_ipd;
    std::lock_guard lock(g_headMutex);
    if (!g_headTick || GetTickCount64() - g_headTick > 500)
        return false;
    memcpy(quat, g_headQuat, sizeof(g_headQuat));
    return true;
}

} // namespace xr
