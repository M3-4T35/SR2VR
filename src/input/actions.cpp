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
std::atomic<float> g_speedPeak[2]{-1.0f, -1.0f}; // plus grande vitesse depuis la dernière lecture
std::atomic<float> g_speedLast[2]{-1.0f, -1.0f};
std::atomic<ULONGLONG> g_speedTick[2]{};
constexpr ULONGLONG kSpeedMaxAgeMs = 300;
std::atomic<unsigned> g_haptic[2]{}; // 0 : rien ; sinon durée (ms) << 8 | force (1 à 255)

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

void ReportHandSpeed(int hand, float speed)
{
    if (hand < 0 || hand > 1)
        return;
    g_speedLast[hand] = speed;
    g_speedTick[hand] = GetTickCount64();
    float peak = g_speedPeak[hand];
    while (speed > peak && !g_speedPeak[hand].compare_exchange_weak(peak, speed)) {
    }
}

float TakeHandSpeed(int hand)
{
    if (hand < 0 || hand > 1)
        return -1.0f;
    const float peak = g_speedPeak[hand].exchange(-1.0f);
    const ULONGLONG tick = g_speedTick[hand];
    if (!tick || GetTickCount64() - tick > kSpeedMaxAgeMs)
        return -1.0f;
    // Pas de rapport depuis la dernière lecture (simulation plus rapide que le casque sur cette
    // image) : la dernière vitesse connue tient encore
    const float last = g_speedLast[hand];
    return last < 0.0f ? -1.0f : peak > last ? peak : last;
}

void RequestHaptic(int hand, float amplitude, unsigned milliseconds)
{
    if (hand < 0 || hand > 1)
        return;
    const float clamped = amplitude < 0.0f ? 0.0f : amplitude > 1.0f ? 1.0f : amplitude;
    const unsigned level = 1 + static_cast<unsigned>(clamped * 254.0f);
    g_haptic[hand] = (milliseconds > 2000 ? 2000 : milliseconds) << 8 | level;
}

bool TakeHaptic(int hand, float* amplitude, unsigned* milliseconds)
{
    if (hand < 0 || hand > 1)
        return false;
    const unsigned request = g_haptic[hand].exchange(0);
    if (!request)
        return false;
    *amplitude = static_cast<float>(request & 0xFF) / 255.0f;
    *milliseconds = request >> 8;
    return true;
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
