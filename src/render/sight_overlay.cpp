#include "render/sight_overlay.h"

#include <algorithm>
#include <cmath>

namespace render {
namespace {

struct Color {
    float b, g, r;
};
constexpr Color kBeam{40.0f, 40.0f, 255.0f};
constexpr Color kDot{50.0f, 50.0f, 255.0f};
constexpr Color kRim{0.0f, 0.0f, 0.0f};

inline void Blend(uint8_t* p, const Color& c, float alpha)
{
    if (alpha <= 0.0f)
        return;
    if (alpha > 1.0f)
        alpha = 1.0f;
    p[0] = static_cast<uint8_t>(p[0] + (c.b - p[0]) * alpha);
    p[1] = static_cast<uint8_t>(p[1] + (c.g - p[1]) * alpha);
    p[2] = static_cast<uint8_t>(p[2] + (c.r - p[2]) * alpha);
}

// Disque plein de rayon r autour de (cx, cy), bord adouci sur un pixel
void Disc(uint8_t* bgra, int width, int height, float cx, float cy, float r, const Color& color, float alpha)
{
    const int x0 = std::max(0, static_cast<int>(std::floor(cx - r - 1.0f)));
    const int x1 = std::min(width - 1, static_cast<int>(std::ceil(cx + r + 1.0f)));
    const int y0 = std::max(0, static_cast<int>(std::floor(cy - r - 1.0f)));
    const int y1 = std::min(height - 1, static_cast<int>(std::ceil(cy + r + 1.0f)));
    for (int y = y0; y <= y1; ++y) {
        uint8_t* row = bgra + static_cast<size_t>(y) * static_cast<size_t>(width) * 4;
        for (int x = x0; x <= x1; ++x) {
            const float dx = static_cast<float>(x) + 0.5f - cx, dy = static_cast<float>(y) + 0.5f - cy;
            const float coverage = r + 0.5f - std::sqrt(dx * dx + dy * dy);
            if (coverage > 0.0f)
                Blend(row + static_cast<size_t>(x) * 4, color, alpha * std::min(coverage, 1.0f));
        }
    }
}

// Trait de demi-épaisseur r entre deux points, en avançant le long de son grand axe
void Line(uint8_t* bgra, int width, int height, float ax, float ay, float bx, float by, float r, const Color& color,
          float alpha)
{
    const float dx = bx - ax, dy = by - ay;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (!(length > 0.5f) || !(length < 1.0e5f))
        return;
    const bool steep = std::fabs(dy) > std::fabs(dx);
    // u : grand axe, v : petit axe
    float u0 = steep ? ay : ax, v0 = steep ? ax : ay;
    float u1 = steep ? by : bx, v1 = steep ? bx : by;
    if (u0 > u1) {
        std::swap(u0, u1);
        std::swap(v0, v1);
    }
    const int uLimit = steep ? height : width, vLimit = steep ? width : height;
    const float slope = (v1 - v0) / (u1 - u0);
    // Distance perpendiculaire = écart sur le petit axe x cosinus de la pente
    const float cosine = (u1 - u0) / length;
    const float reach = (r + 1.0f) / cosine;
    const int first = std::max(0, static_cast<int>(std::floor(u0)));
    const int last = std::min(uLimit - 1, static_cast<int>(std::ceil(u1)));
    for (int u = first; u <= last; ++u) {
        const float centre = v0 + (static_cast<float>(u) + 0.5f - u0) * slope;
        const int vFirst = std::max(0, static_cast<int>(std::floor(centre - reach)));
        const int vLast = std::min(vLimit - 1, static_cast<int>(std::ceil(centre + reach)));
        for (int v = vFirst; v <= vLast; ++v) {
            const float distance = std::fabs(static_cast<float>(v) + 0.5f - centre) * cosine;
            const float coverage = r + 0.5f - distance;
            if (coverage <= 0.0f)
                continue;
            const int x = steep ? v : u, y = steep ? u : v;
            Blend(bgra + (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4, color,
                  alpha * std::min(coverage, 1.0f));
        }
    }
}

} // namespace

void DrawSight(uint8_t* bgra, unsigned width, unsigned height, float hfovDeg, const SightOverlay& sight)
{
    if (!bgra || width < 16 || height < 16 || !(hfovDeg > 1.0f && hfovDeg < 179.0f) || (!sight.dot && !sight.beam))
        return;
    const float w = static_cast<float>(width), h = static_cast<float>(height);
    const float tanH = std::tan(hfovDeg * 0.5f * 0.01745329f);
    const float tanV = tanH * h / w;
    const auto px = [&](float t) { return (0.5f + 0.5f * t / tanH) * w; };
    const auto py = [&](float t) { return (0.5f - 0.5f * t / tanV) * h; };
    const int iw = static_cast<int>(width), ih = static_cast<int>(height);
    // Tailles pensées pour une image de 1920 pixels de large, mises à l'échelle
    const float unit = w / 1920.0f;

    if (sight.beam) {
        float ax = px(sight.beamA[0]), ay = py(sight.beamA[1]);
        const float bx = px(sight.beamB[0]), by = py(sight.beamB[1]);
        // Le départ peut tomber très loin hors de l'image (manette au bord du champ) : ramené à
        // quelques images de distance, la pente du trait ne change pas
        const float limit = 4.0f * w;
        const float dx = ax - bx, dy = ay - by;
        const float length = std::sqrt(dx * dx + dy * dy);
        if (length > limit) {
            ax = bx + dx * limit / length;
            ay = by + dy * limit / length;
        }
        Line(bgra, iw, ih, ax, ay, bx, by, std::max(0.6f, 1.0f * unit), kBeam, 0.45f);
    }
    if (sight.dot) {
        const float x = px(sight.dotT[0]), y = py(sight.dotT[1]);
        if (x > -50.0f && x < w + 50.0f && y > -50.0f && y < h + 50.0f) {
            Disc(bgra, iw, ih, x, y, std::max(3.0f, 6.5f * unit), kRim, 0.55f);
            Disc(bgra, iw, ih, x, y, std::max(2.0f, 4.5f * unit), kDot, 0.95f);
        }
    }
}

} // namespace render
