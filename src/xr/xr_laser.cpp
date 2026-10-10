#include "xr/xr_laser.h"
#include "util/config.h"
#include "util/log.h"

#include <openxr/openxr_platform.h>

#include <cmath>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <vector>

namespace xr {
namespace {

// Point visé publié par le moteur
struct SightState {
    bool visible = false;
    bool hit = false;
    float distance = 0.0f;
    ULONGLONG tick = 0;
};
std::mutex g_sightMutex;
SightState g_sight[2];
// Le jeu peut ne rendre que 15 à 25 images par seconde : au-delà de ce délai sans nouvelle, il
// ne vise plus
constexpr ULONGLONG kSightMaxAgeMs = 300;

constexpr int32_t kBeamWidth = 64, kBeamHeight = 8;
constexpr int32_t kDotSize = 32;
// Le trait part un peu devant la manette, pas du poignet
constexpr float kBeamStart = 0.08f;
constexpr float kBeamThickness = 0.006f; // mètres
// Point d'impact : taille apparente constante (fraction de sa distance à la tête), avec un
// minimum de près
constexpr float kDotAngular = 0.011f;
constexpr float kDotMin = 0.012f;

struct Vec {
    float x, y, z;
};
Vec operator+(Vec a, Vec b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec operator-(Vec a, Vec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec operator*(Vec a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec Cross(Vec a, Vec b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Length(Vec a) { return std::sqrt(Dot(a, a)); }
Vec ToVec(const XrVector3f& v) { return {v.x, v.y, v.z}; }

// Avant d'une pose : -z tourné par son orientation
Vec Forward(const XrQuaternionf& q)
{
    return {-2.0f * (q.x * q.z + q.w * q.y), -2.0f * (q.y * q.z - q.w * q.x), -(1.0f - 2.0f * (q.x * q.x + q.y * q.y))};
}

// Orientation dont les axes x, y, z sont ces trois vecteurs (repère direct, vecteurs unitaires)
XrQuaternionf FromAxes(Vec x, Vec y, Vec z)
{
    const float trace = x.x + y.y + z.z;
    XrQuaternionf q{};
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (y.z - z.y) / s;
        q.y = (z.x - x.z) / s;
        q.z = (x.y - y.x) / s;
    } else if (x.x > y.y && x.x > z.z) {
        const float s = std::sqrt(1.0f + x.x - y.y - z.z) * 2.0f;
        q.w = (y.z - z.y) / s;
        q.x = 0.25f * s;
        q.y = (y.x + x.y) / s;
        q.z = (z.x + x.z) / s;
    } else if (y.y > z.z) {
        const float s = std::sqrt(1.0f + y.y - x.x - z.z) * 2.0f;
        q.w = (z.x - x.z) / s;
        q.x = (y.x + x.y) / s;
        q.y = 0.25f * s;
        q.z = (z.y + y.z) / s;
    } else {
        const float s = std::sqrt(1.0f + z.z - x.x - y.y) * 2.0f;
        q.w = (x.y - y.x) / s;
        q.x = (z.x + x.z) / s;
        q.y = (z.y + y.z) / s;
        q.z = 0.25f * s;
    }
    return q;
}

// Couleur (linéaire, 0 à 1) et opacité -> pixel BGRA sRGB à alpha prémultiplié
void PutPixel(uint8_t* pixel, float r, float g, float b, float alpha)
{
    const auto encode = [alpha](float linear) {
        float v = linear * alpha;
        v = v <= 0.0031308f ? 12.92f * v : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
        return static_cast<uint8_t>(v <= 0.0f ? 0.0f : v >= 1.0f ? 255.0f : v * 255.0f + 0.5f);
    };
    pixel[0] = encode(b);
    pixel[1] = encode(g);
    pixel[2] = encode(r);
    pixel[3] = static_cast<uint8_t>(alpha <= 0.0f ? 0.0f : alpha >= 1.0f ? 255.0f : alpha * 255.0f + 0.5f);
}

float Smooth(float edge0, float edge1, float x)
{
    float t = (x - edge0) / (edge1 - edge0);
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

void SetSight(int hand, bool visible, float distance, bool hit)
{
    if (hand < 0 || hand > 1)
        return;
    std::lock_guard lock(g_sightMutex);
    SightState& sight = g_sight[hand];
    sight.visible = visible && distance > 0.0f && distance < 100000.0f;
    sight.hit = hit;
    sight.distance = distance;
    sight.tick = GetTickCount64();
}

bool Laser::CreateTexture(XrSession session, ID3D11DeviceContext* context, int64_t format, int32_t width,
                          int32_t height, const uint8_t* bgra, Texture* texture)
{
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = format;
    ci.sampleCount = 1;
    ci.width = static_cast<uint32_t>(width);
    ci.height = static_cast<uint32_t>(height);
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(session, &ci, &texture->swapchain))) {
        texture->swapchain = XR_NULL_HANDLE;
        return false;
    }
    uint32_t count = 0;
    xrEnumerateSwapchainImages(texture->swapchain, 0, &count, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    if (!count || XR_FAILED(xrEnumerateSwapchainImages(texture->swapchain, count, &count,
                                                       reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()))))
        return false;
    // L'image n'est écrite qu'une fois : le compositeur garde la dernière image rendue
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrAcquireSwapchainImage(texture->swapchain, &ai, &index)) || index >= count)
        return false;
    const bool waited = XR_SUCCEEDED(xrWaitSwapchainImage(texture->swapchain, &wi));
    if (waited)
        context->UpdateSubresource(images[index].texture, 0, nullptr, bgra, static_cast<UINT>(width) * 4, 0);
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    const bool released = XR_SUCCEEDED(xrReleaseSwapchainImage(texture->swapchain, &ri));
    texture->width = width;
    texture->height = height;
    return waited && released;
}

bool Laser::Init(XrSession session, ID3D11DeviceContext* context, int64_t format)
{
    Destroy();
    // Trait : rouge, plein près de la manette, qui s'efface vers son bout ; bords adoucis
    std::vector<uint8_t> beam(static_cast<size_t>(kBeamWidth) * kBeamHeight * 4);
    for (int32_t y = 0; y < kBeamHeight; ++y) {
        const float v = (static_cast<float>(y) + 0.5f) / kBeamHeight * 2.0f - 1.0f; // -1 à 1 en travers
        const float across = 1.0f - Smooth(0.35f, 1.0f, std::fabs(v));
        for (int32_t x = 0; x < kBeamWidth; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / kBeamWidth; // 0 côté manette, 1 au bout
            const float along = Smooth(0.0f, 0.04f, u) * (1.0f - Smooth(0.25f, 1.0f, u));
            PutPixel(&beam[(static_cast<size_t>(y) * kBeamWidth + x) * 4], 1.0f, 0.03f, 0.02f, 0.9f * across * along);
        }
    }
    // Point : disque rouge à bord adouci, cœur plus clair
    std::vector<uint8_t> dot(static_cast<size_t>(kDotSize) * kDotSize * 4);
    for (int32_t y = 0; y < kDotSize; ++y) {
        for (int32_t x = 0; x < kDotSize; ++x) {
            const float dx = (static_cast<float>(x) + 0.5f) / kDotSize * 2.0f - 1.0f;
            const float dy = (static_cast<float>(y) + 0.5f) / kDotSize * 2.0f - 1.0f;
            const float radius = std::sqrt(dx * dx + dy * dy);
            const float core = 1.0f - Smooth(0.15f, 0.55f, radius);
            PutPixel(&dot[(static_cast<size_t>(y) * kDotSize + x) * 4], 1.0f, 0.03f + 0.55f * core, 0.02f + 0.45f * core,
                     1.0f - Smooth(0.7f, 0.98f, radius));
        }
    }
    ready_ = CreateTexture(session, context, format, kBeamWidth, kBeamHeight, beam.data(), &beam_) &&
             CreateTexture(session, context, format, kDotSize, kDotSize, dot.data(), &dot_);
    if (!ready_) {
        LOG_XR("repere de visee : textures refusees par le runtime, pas de trait ni de point dans le casque");
        Destroy();
        return false;
    }
    LOG_XR("repere de visee : trait et point prets (calques du casque, replaces a chaque image du casque)");
    return true;
}

void Laser::Destroy()
{
    for (Texture* texture : {&beam_, &dot_}) {
        if (texture->swapchain != XR_NULL_HANDLE)
            xrDestroySwapchain(texture->swapchain);
        texture->swapchain = XR_NULL_HANDLE;
    }
    ready_ = false;
}

void Laser::ReadConfig()
{
    mode_ = config::Int("Hands", "laser", 1);
    length_ = config::Float("Hands", "laser_length", 3.0f);
    if (!(length_ >= 0.2f && length_ <= 200.0f))
        length_ = 3.0f;
}

uint32_t Laser::Build(XrSpace space, const XrPosef& head, const XrPosef* const aim[2],
                      XrCompositionLayerQuad out[kMaxLayers])
{
    if (!ready_)
        return 0;
    const ULONGLONG now = GetTickCount64();
    if (now - lastConfig_ >= 1000) {
        lastConfig_ = now;
        ReadConfig();
    }
    if (mode_ != 1 && mode_ != 2)
        return 0;

    uint32_t count = 0;
    const auto add = [&](const Texture& texture, Vec center, Vec x, Vec y, Vec z, float width, float height) {
        XrCompositionLayerQuad& quad = out[count++];
        quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        quad.space = space;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = texture.swapchain;
        quad.subImage.imageRect = {{0, 0}, {texture.width, texture.height}};
        quad.pose.orientation = FromAxes(x, y, z);
        quad.pose.position = {center.x, center.y, center.z};
        quad.size = {width, height};
    };

    const Vec eye = ToVec(head.position);
    for (int hand = 0; hand < 2; ++hand) {
        SightState sight;
        {
            std::lock_guard lock(g_sightMutex);
            sight = g_sight[hand];
        }
        if (!aim[hand] || !sight.visible || now - sight.tick > kSightMaxAgeMs)
            continue;
        const Vec origin = ToVec(aim[hand]->position);
        const Vec forward = Forward(aim[hand]->orientation);
        bool drawn = false;

        // Trait : un rectangle étiré le long du rayon, tourné autour de lui pour faire face à
        // la tête
        const float length = (sight.distance < length_ ? sight.distance : length_) - kBeamStart;
        if (mode_ == 1 && length > 0.05f) {
            const Vec center = origin + forward * (kBeamStart + 0.5f * length);
            Vec normal = eye - center;
            normal = normal - forward * Dot(normal, forward);
            const float size = Length(normal);
            if (size > 0.001f) {
                normal = normal * (1.0f / size);
                add(beam_, center, forward, Cross(normal, forward), normal, length, kBeamThickness);
                drawn = true;
            }
        }
        // Point d'impact, face à la tête
        if (sight.hit) {
            const Vec point = origin + forward * sight.distance;
            Vec normal = eye - point;
            const float distance = Length(normal);
            if (distance > 0.05f) {
                normal = normal * (1.0f / distance);
                Vec right = Cross({0.0f, 1.0f, 0.0f}, normal);
                float size = Length(right);
                if (size < 0.01f) {
                    right = Cross({1.0f, 0.0f, 0.0f}, normal);
                    size = Length(right);
                }
                right = right * (1.0f / size);
                const float diameter = distance * kDotAngular > kDotMin ? distance * kDotAngular : kDotMin;
                add(dot_, point, right, Cross(normal, right), normal, diameter, diameter);
                drawn = true;
            }
        }
        if (drawn)
            ++shown_[hand];
    }
    return count;
}

void Laser::TakeStats(unsigned shown[2])
{
    shown[0] = shown_[0];
    shown[1] = shown_[1];
    shown_[0] = shown_[1] = 0;
}

} // namespace xr
