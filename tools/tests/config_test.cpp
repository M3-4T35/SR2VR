// SR2VR - test de src/util/config : le cache en mémoire de sr2_vr.ini doit lire chaque clé
// exactement comme GetPrivateProfileInt / GetPrivateProfileString, l'API qu'il remplace.
// Écrit un sr2_vr.ini piégé à côté de l'exe, compare les deux lectures, puis vérifie la
// relecture après modification et la suppression du fichier. Code de sortie : nombre d'écarts.
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "util/config.h"

namespace {

char g_ini[MAX_PATH];
int g_errors = 0;

int ApiInt(const char* section, const char* key, int fallback)
{
    return static_cast<int>(GetPrivateProfileIntA(section, key, fallback, g_ini));
}

// Lecture d'un nombre à virgule telle que la faisait config::Float avant le cache
float ApiFloat(const char* section, const char* key, float fallback)
{
    char text[64]{};
    if (!GetPrivateProfileStringA(section, key, "", text, sizeof(text), g_ini) || !text[0])
        return fallback;
    char* end = nullptr;
    const double value = strtod(text, &end);
    return end != text ? static_cast<float>(value) : fallback;
}

void Expect(bool ok, const char* what)
{
    printf("%s %s\n", ok ? "  ok   " : "  ECART", what);
    if (!ok)
        ++g_errors;
}

} // namespace

int main()
{
    config::Init(nullptr);
    strcpy_s(g_ini, config::Path());

    FILE* f = nullptr;
    if (fopen_s(&f, g_ini, "wb") != 0 || !f) {
        printf("impossible d'ecrire %s\n", g_ini);
        return 100;
    }
    fputs("; commentaire\r\n"
          "[Display]\r\nmode=3\r\n  fov_scale = 1.25 \r\n"
          "[ camera ]\r\nHFOV=110\r\nmirror_x=\r\nmirror_y=abc\r\nmirror_z=\"1\"\r\n"
          "[Controls]\r\njump_scan=0x39\r\ncrouch_scan=-5\r\nturn_speed=1e3\r\nsnap_angle=45deg\r\n"
          "layout = 2 ; note\r\n;menu_pad=0\r\nmenu_pad=1\r\nmenu_pad=0\r\n"
          "[Display]\r\nmode=1\r\ncutscene_screen=0\n"
          "[Hud]\nscale=0.5\n",
          f);
    fclose(f);

    static const char* const kInts[][2] = {
        {"Display", "mode"},       {"Camera", "hfov"},        {"Camera", "mirror_x"},   {"Camera", "mirror_y"},
        {"Camera", "mirror_z"},    {"Controls", "jump_scan"}, {"Controls", "crouch_scan"}, {"Controls", "layout"},
        {"Controls", "menu_pad"},  {"Display", "cutscene_screen"}, {"Hud", "absente"}, {"Absente", "x"},
        {"display", "MODE"},
    };
    for (const auto& k : kInts) {
        const int api = ApiInt(k[0], k[1], 7), cached = config::Int(k[0], k[1], 7);
        char what[128];
        snprintf(what, sizeof(what), "Int   [%s] %s : api %d, cache %d", k[0], k[1], api, cached);
        Expect(api == cached, what);
    }
    static const char* const kFloats[][2] = {
        {"Display", "fov_scale"}, {"Camera", "hfov"}, {"Controls", "turn_speed"}, {"Controls", "snap_angle"},
        {"Hud", "scale"},         {"Camera", "mirror_y"}, {"Hud", "absente"},
    };
    for (const auto& k : kFloats) {
        const float api = ApiFloat(k[0], k[1], 9.5f), cached = config::Float(k[0], k[1], 9.5f);
        char what[128];
        snprintf(what, sizeof(what), "Float [%s] %s : api %g, cache %g", k[0], k[1], api, cached);
        Expect(api == cached, what);
    }

    // Relecture après modification (le cache vérifie le fichier toutes les 250 ms)
    Sleep(300);
    WritePrivateProfileStringA("Hud", "scale", "0.75", g_ini);
    Sleep(300);
    Expect(config::Float("Hud", "scale", 0.0f) == 0.75f, "relecture apres modification");

    DeleteFileA(g_ini);
    Sleep(300);
    Expect(config::Int("Display", "mode", 42) == 42, "fichier supprime : valeurs par defaut");

    printf(g_errors ? "%d ECART(S)\n" : "OK\n", g_errors);
    return g_errors;
}
