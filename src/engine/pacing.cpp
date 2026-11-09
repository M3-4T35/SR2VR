#include "engine/pacing.h"
#include "util/config.h"
#include "util/log.h"
#include "util/pattern.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace engine::pacing {
namespace {

// Fin du frein : fld [durée à dormir] ; call <flottant vers entier> ; push eax ;
// call [Sleep] ; jmp +25h ; fld qword [10.0] ; fcomp ...
constexpr char kSleepSig[] = "D9 85 ?? ?? ?? ?? E8 ?? ?? ?? ?? 50 FF 15 ?? ?? ?? ?? EB 25 DD 05 ?? ?? ?? ?? DC 9D";
constexpr int kCallOffset = 12;    // FF 15 <adresse de l'import Sleep>
constexpr int kOperandOffset = 14; // l'adresse seule : 4 octets, écrits d'un coup

constexpr ULONGLONG kStatsIntervalMs = 10000;

uint8_t* g_site = nullptr;
uint32_t g_originalOperand = 0;

// Lus et écrits par le thread du Present seulement
LARGE_INTEGER g_frequency{};
LONGLONG g_last = 0;         // fin de notre dernière attente
ULONGLONG g_configTick = 0;
int g_cap = 100;
int g_capShown = -1;
ULONGLONG g_statsTick = 0;
unsigned g_frames = 0;
unsigned long long g_gameMs = 0; // ce que le frein du jeu voulait dormir
double g_ourMs = 0.0;            // ce que nous avons dormi
int g_statsLogged = 0;

void ReadConfig(ULONGLONG now)
{
    if (g_configTick && now - g_configTick < 1000)
        return;
    g_configTick = now;
    int cap = config::Int("Display", "frame_cap", 100);
    if (cap < 0)
        cap = 0;
    if (cap > 0 && cap < 30)
        cap = 30; // en dessous, le jeu ralentit ses propres chargements
    g_cap = cap;
    if (cap != g_capShown) {
        g_capShown = cap;
        if (cap)
            LOG_ENGINE("cadence : frein du jeu remplace par un plafond de %d images par seconde ([Display] frame_cap)", cap);
        else
            LOG_ENGINE("cadence : frein d'origine du jeu ([Display] frame_cap=0)");
    }
}

// Appelée par le jeu à la place de Sleep, après chaque Present plus rapide que 35 images
// par seconde. gameMs : la sieste que voulait le frein d'origine.
void __stdcall Limiter(DWORD gameMs)
{
    const ULONGLONG tick = GetTickCount64();
    ReadConfig(tick);
    ++g_frames;
    g_gameMs += gameMs;
    if (g_cap <= 0) {
        Sleep(gameMs);
        g_ourMs += gameMs;
    } else {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const double period = 1000.0 / g_cap;
        const double elapsed =
            g_last ? static_cast<double>(now.QuadPart - g_last) * 1000.0 / static_cast<double>(g_frequency.QuadPart) : period;
        if (elapsed >= 0.0 && elapsed < period) {
            // Sleep n'est précis qu'à la milliseconde : on s'arrête juste avant plutôt qu'après
            const DWORD wait = static_cast<DWORD>(period - elapsed);
            if (wait >= 1) {
                Sleep(wait);
                g_ourMs += wait;
            }
        }
        QueryPerformanceCounter(&now);
        g_last = now.QuadPart;
    }

    if (!g_statsTick) {
        g_statsTick = tick;
    } else if (tick - g_statsTick >= kStatsIntervalMs) {
        // Relevé toutes les 10 s pendant 5 min, puis toutes les minutes
        if (g_statsLogged < 30 || g_statsLogged % 6 == 0)
            LOG_ENGINE("cadence : %u images au-dessus de 35 par seconde en %.0f s ; le frein du jeu aurait dormi %.1f ms "
                       "par image, dormi %.1f ms",
                       g_frames, static_cast<double>(tick - g_statsTick) / 1000.0,
                       static_cast<double>(g_gameMs) / g_frames, g_ourMs / g_frames);
        ++g_statsLogged;
        g_statsTick = tick;
        g_frames = 0;
        g_gameMs = 0;
        g_ourMs = 0.0;
    }
}

// Le jeu appelle par « call [adresse] » : l'adresse d'une case qui contient notre fonction
void* g_limiterSlot = reinterpret_cast<void*>(&Limiter);

bool WriteOperand(uint32_t value)
{
    uint8_t* at = g_site + kOperandOffset;
    DWORD protection = 0;
    if (!VirtualProtect(at, sizeof(value), PAGE_EXECUTE_READWRITE, &protection))
        return false;
    // Un seul mot de 4 octets, dans la même ligne de cache : jamais lu à moitié par le thread
    // du rendu
    InterlockedExchange(reinterpret_cast<volatile LONG*>(at), static_cast<LONG>(value));
    VirtualProtect(at, sizeof(value), protection, &protection);
    FlushInstructionCache(GetCurrentProcess(), at, sizeof(value));
    return true;
}

} // namespace

bool Install()
{
    QueryPerformanceFrequency(&g_frequency);
    auto* site = static_cast<uint8_t*>(pattern::Find("frein d'images du jeu", kSleepSig));
    if (!site) {
        LOG_ENGINE("cadence : frein d'images du jeu introuvable (deja retire par Juiced, UncapFPS=1 ?) : rien a faire");
        return false;
    }
    if (site[kCallOffset] != 0xFF || site[kCallOffset + 1] != 0x15) {
        LOG_ENGINE("cadence : appel inattendu dans le frein d'images, laisse tel quel");
        return false;
    }
    g_site = site;
    memcpy(&g_originalOperand, site + kOperandOffset, sizeof(g_originalOperand));
    if (!WriteOperand(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_limiterSlot)))) {
        LOG_ENGINE("cadence : ECHEC de l'ecriture dans le frein d'images, le jeu garde sa cadence");
        g_site = nullptr;
        return false;
    }
    LOG_ENGINE("cadence : frein d'images du jeu exe+0x%lX detourne (au-dela de 35 images par seconde, le jeu dormait "
               "(28,6 ms - duree de l'image) / 2 apres chaque Present)",
               static_cast<unsigned long>(reinterpret_cast<uintptr_t>(site) -
                                          reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))));
    return true;
}

void Remove()
{
    if (g_site)
        WriteOperand(g_originalOperand);
    g_site = nullptr;
}

} // namespace engine::pacing
