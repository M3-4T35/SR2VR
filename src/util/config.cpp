#include "util/config.h"

#include <cstdlib>
#include <cstring>

namespace config {
namespace {
char g_path[MAX_PATH]{};
} // namespace

void Init(HMODULE self)
{
    GetModuleFileNameA(self, g_path, MAX_PATH);
    if (char* slash = strrchr(g_path, '\\'))
        strcpy_s(slash + 1, MAX_PATH - (slash + 1 - g_path), "sr2_vr.ini");
}

const char* Path()
{
    return g_path;
}

int Int(const char* section, const char* key, int defaultValue)
{
    return g_path[0] ? static_cast<int>(GetPrivateProfileIntA(section, key, defaultValue, g_path)) : defaultValue;
}

float Float(const char* section, const char* key, float defaultValue)
{
    char text[64]{};
    if (!g_path[0] || !GetPrivateProfileStringA(section, key, "", text, sizeof(text), g_path) || !text[0])
        return defaultValue;
    char* end = nullptr;
    const double value = strtod(text, &end);
    return end != text ? static_cast<float>(value) : defaultValue;
}

} // namespace config
