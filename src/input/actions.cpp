#include "input/actions.h"

#include <windows.h>

#include <atomic>

namespace input::actions {
namespace {

std::atomic<bool> g_down[kCount]{};
std::atomic<unsigned> g_presses[kCount]{};
std::atomic<bool> g_direct{false};
std::atomic<bool> g_dual{false};
std::atomic<float> g_yaw{0.0f};
std::atomic<bool> g_yawSteady{false};
std::atomic<unsigned> g_yawSequence{0};
std::atomic<ULONGLONG> g_yawTick{0};
constexpr ULONGLONG kYawMaxAgeMs = 400;

} // namespace

void Set(Action action, bool down)
{
    if (action < 0 || action >= kCount)
        return;
    // L'appui est compté avant d'être visible dans Get (voir TakePresses)
    if (down && !g_down[action])
        ++g_presses[action];
    g_down[action] = down;
}

void ReleaseAll()
{
    for (auto& down : g_down)
        down = false;
}

bool Get(Action action)
{
    return action >= 0 && action < kCount && g_down[action];
}

unsigned TakePresses(Action action)
{
    return action >= 0 && action < kCount ? g_presses[action].exchange(0) : 0;
}

void SetDirect(bool ready)
{
    g_direct = ready;
}

bool Direct()
{
    return g_direct;
}

void SetDualWield(bool dual)
{
    g_dual = dual;
}

bool DualWield()
{
    return g_dual;
}

void SetGameYaw(float degrees, bool steady)
{
    g_yaw = degrees;
    g_yawSteady = steady;
    g_yawTick = GetTickCount64();
    ++g_yawSequence;
}

bool GameYaw(float* degrees, unsigned* sequence, bool* steady)
{
    const ULONGLONG tick = g_yawTick;
    if (!tick || GetTickCount64() - tick > kYawMaxAgeMs)
        return false;
    *sequence = g_yawSequence;
    *degrees = g_yaw;
    *steady = g_yawSteady;
    return true;
}

} // namespace input::actions
