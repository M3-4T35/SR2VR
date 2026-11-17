#include "render/panel_image.h"

#include <atomic>
#include <cstring>
#include <mutex>

namespace render {
namespace {

std::mutex g_mutex;
CpuFrame g_latest;
ULONGLONG g_tick = 0;
uint64_t g_seq = 0;
std::atomic<unsigned> g_left{0}, g_redirected{0}, g_read{0};
std::atomic<long long> g_us{0};

} // namespace

void PublishPanel(const uint8_t* pixels, int pitch, UINT width, UINT height, const FrameView& view)
{
    std::lock_guard lock(g_mutex);
    g_latest.width = width;
    g_latest.height = height;
    g_latest.pixels.resize(static_cast<size_t>(width) * height * 4);
    for (UINT y = 0; y < height; ++y)
        memcpy(g_latest.pixels.data() + static_cast<size_t>(y) * width * 4, pixels + static_cast<size_t>(y) * pitch,
               static_cast<size_t>(width) * 4);
    g_latest.view = view;
    g_latest.seq = ++g_seq;
    g_tick = GetTickCount64();
}

bool TakePanel(CpuFrame& frame, uint64_t since, ULONGLONG* tick)
{
    std::lock_guard lock(g_mutex);
    if (!g_seq || g_seq == since)
        return false;
    frame = g_latest;
    *tick = g_tick;
    return true;
}

void CountPanel(PanelEvent event, long long microseconds)
{
    if (event == PanelEvent::LeftFrame)
        ++g_left;
    else if (event == PanelEvent::Read)
        ++g_read;
    else {
        ++g_redirected;
        g_us += microseconds;
    }
}

void TakePanelStats(unsigned* left, unsigned* redirected, unsigned* read, double* averageMs)
{
    *left = g_left.exchange(0);
    *redirected = g_redirected.exchange(0);
    *read = g_read.exchange(0);
    const long long us = g_us.exchange(0);
    *averageMs = *redirected ? static_cast<double>(us) / 1000.0 / *redirected : 0.0;
}

} // namespace render
