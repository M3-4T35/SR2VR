#include "util/log.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <mutex>

namespace vrlog {
namespace {
char g_path[MAX_PATH]{};
std::mutex g_mutex;
// Fichier gardé ouvert pour toute la session : une ligne = un WriteFile, au lieu d'ouvrir et de
// refermer le fichier à chaque ligne (coûteux sous Windows, surtout avec un antivirus, et
// parfois depuis le thread du jeu). WriteFile passe par le cache du système : une ligne
// écrite reste dans le fichier même si le jeu plante juste après.
HANDLE g_file = INVALID_HANDLE_VALUE;

// Partage large : le log peut être suivi, copié ou supprimé pendant que le jeu tourne
HANDLE Open(DWORD disposition)
{
    return CreateFileA(g_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                       disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void WriteAll(const char* text, DWORD length)
{
    DWORD written = 0;
    if (g_file != INVALID_HANDLE_VALUE && WriteFile(g_file, text, length, &written, nullptr) && written == length)
        return;
    // Fichier supprimé ou handle perdu : rouvert (quelques essais, il peut être tenu un instant)
    if (g_file != INVALID_HANDLE_VALUE)
        CloseHandle(g_file);
    g_file = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 5 && g_file == INVALID_HANDLE_VALUE; ++attempt) {
        g_file = Open(OPEN_ALWAYS);
        if (g_file == INVALID_HANDLE_VALUE)
            Sleep(2);
    }
    if (g_file != INVALID_HANDLE_VALUE)
        WriteFile(g_file, text, length, &written, nullptr);
}
} // namespace

void Init(HMODULE self, const char* fileName)
{
    std::lock_guard lock(g_mutex);
    GetModuleFileNameA(self, g_path, MAX_PATH);
    if (char* slash = strrchr(g_path, '\\'))
        strcpy_s(slash + 1, MAX_PATH - (slash + 1 - g_path), fileName);

    SYSTEMTIME t;
    GetLocalTime(&t);
    if (g_file != INVALID_HANDLE_VALUE)
        CloseHandle(g_file);
    g_file = Open(CREATE_ALWAYS); // vidé à chaque session
    char header[96];
    const int length = snprintf(header, sizeof(header), "=== SR2VR - session du %04u-%02u-%02u %02u:%02u:%02u ===\r\n",
                                t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    if (length > 0)
        WriteAll(header, static_cast<DWORD>(std::min<int>(length, sizeof(header) - 1)));
}

void Write(const char* tag, const char* fmt, ...)
{
    if (!g_path[0])
        return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[1100];
    int length = snprintf(line, sizeof(line), "%02u:%02u:%02u.%03u %-8s ", t.wHour, t.wMinute, t.wSecond,
                          t.wMilliseconds, tag);
    if (length < 0)
        return;
    va_list args;
    va_start(args, fmt);
    // Message borné à 1024 caractères comme avant, la fin de ligne a toujours sa place
    const int room = std::min<int>(1024, static_cast<int>(sizeof(line)) - length - 3);
    const int written = vsnprintf(line + length, static_cast<size_t>(room), fmt, args);
    va_end(args);
    if (written > 0)
        length += std::min(written, room - 1);
    line[length++] = '\r';
    line[length++] = '\n';

    std::lock_guard lock(g_mutex);
    WriteAll(line, static_cast<DWORD>(length));
}

} // namespace vrlog
