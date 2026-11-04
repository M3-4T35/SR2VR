#include "util/config.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace config {
namespace {

char g_path[MAX_PATH]{};

// Le fichier est gardé en mémoire et relu seulement quand il change (date ou taille) : les
// appelants relisent leurs réglages une fois par seconde, clé par clé, dont le thread du jeu
// (une quarantaine de clés). Avec GetPrivateProfileString chaque clé ouvrait, lisait et
// fermait le fichier : quelques millisecondes perdues d'un coup, une fois par seconde.
constexpr ULONGLONG kCheckIntervalMs = 250; // au plus une vérification du fichier par intervalle

std::mutex g_mutex;
std::unordered_map<std::string, std::string> g_values; // "section\nclé" en minuscules -> valeur
FILETIME g_stamp{};
ULONGLONG g_size = ~0ull;
ULONGLONG g_checkedTick = 0;
bool g_loaded = false;

std::string Lower(const char* text, size_t length)
{
    std::string out(text, length);
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

void Trim(const char*& begin, const char*& end)
{
    while (begin < end && (*begin == ' ' || *begin == '\t'))
        ++begin;
    while (end > begin && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
        --end;
}

// Même lecture que GetPrivateProfileString : sections et clés sans distinction de casse,
// espaces retirés autour, guillemets autour de la valeur retirés, première section et
// première clé retenues, lignes sans '=' ignorées (dont les commentaires ';').
void Parse(const std::string& data)
{
    g_values.clear();
    std::string section;
    std::unordered_set<std::string> seen;
    bool inSection = false;
    const char* p = data.data();
    const char* const stop = p + data.size();
    // En-tête UTF-8 éventuel
    if (data.size() >= 3 && static_cast<unsigned char>(p[0]) == 0xEF && static_cast<unsigned char>(p[1]) == 0xBB &&
        static_cast<unsigned char>(p[2]) == 0xBF)
        p += 3;
    while (p < stop) {
        const char* lineEnd = static_cast<const char*>(memchr(p, '\n', static_cast<size_t>(stop - p)));
        if (!lineEnd)
            lineEnd = stop;
        const char* b = p;
        const char* e = lineEnd;
        p = lineEnd + 1;
        Trim(b, e);
        if (b == e)
            continue;
        if (*b == '[') {
            const char* close = static_cast<const char*>(memchr(b, ']', static_cast<size_t>(e - b)));
            const char* nb = b + 1;
            const char* ne = close ? close : e;
            Trim(nb, ne);
            section = Lower(nb, static_cast<size_t>(ne - nb));
            // Section répétée : seule la première compte, comme pour GetPrivateProfileString
            inSection = seen.insert(section).second;
            continue;
        }
        if (!inSection || *b == ';')
            continue;
        const char* eq = static_cast<const char*>(memchr(b, '=', static_cast<size_t>(e - b)));
        if (!eq)
            continue;
        const char* kb = b;
        const char* ke = eq;
        const char* vb = eq + 1;
        const char* ve = e;
        Trim(kb, ke);
        Trim(vb, ve);
        if (ve - vb >= 2 && ((*vb == '"' && ve[-1] == '"') || (*vb == '\'' && ve[-1] == '\''))) {
            ++vb;
            --ve;
        }
        g_values.emplace(section + '\n' + Lower(kb, static_cast<size_t>(ke - kb)),
                         std::string(vb, static_cast<size_t>(ve - vb)));
    }
}

// Relit le fichier s'il a changé depuis la dernière lecture. Sous g_mutex.
void Refresh()
{
    const ULONGLONG now = GetTickCount64();
    if (g_loaded && now - g_checkedTick < kCheckIntervalMs)
        return;
    g_checkedTick = now;

    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExA(g_path, GetFileExInfoStandard, &info)) {
        // Fichier absent : valeurs par défaut partout
        g_values.clear();
        g_size = ~0ull;
        g_stamp = {};
        g_loaded = true;
        return;
    }
    const ULONGLONG size = (static_cast<ULONGLONG>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    if (g_loaded && size == g_size && CompareFileTime(&info.ftLastWriteTime, &g_stamp) == 0)
        return;

    // Partage large : l'outil de configuration ou un éditeur peut l'écrire au même moment
    HANDLE file = CreateFileA(g_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return; // tenu un instant : on garde l'ancien contenu, nouvel essai au prochain intervalle
    std::string data;
    LARGE_INTEGER length{};
    bool ok = GetFileSizeEx(file, &length) && length.QuadPart < (16 << 20);
    if (ok) {
        data.resize(static_cast<size_t>(length.QuadPart));
        DWORD read = 0;
        ok = data.empty() || (ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr) != 0);
        data.resize(read);
    }
    CloseHandle(file);
    if (!ok)
        return;
    // Fichier en UTF-16 (en-tête FF FE) : WritePrivateProfileString garde alors cet encodage
    if (data.size() >= 2 && static_cast<unsigned char>(data[0]) == 0xFF && static_cast<unsigned char>(data[1]) == 0xFE) {
        const auto* wide = reinterpret_cast<const wchar_t*>(data.data() + 2);
        const int count = static_cast<int>((data.size() - 2) / sizeof(wchar_t));
        std::string ansi(static_cast<size_t>(WideCharToMultiByte(CP_ACP, 0, wide, count, nullptr, 0, nullptr, nullptr)), '\0');
        WideCharToMultiByte(CP_ACP, 0, wide, count, ansi.data(), static_cast<int>(ansi.size()), nullptr, nullptr);
        data.swap(ansi);
    }
    Parse(data);
    g_size = size;
    g_stamp = info.ftLastWriteTime;
    g_loaded = true;
}

// Valeur brute de la clé, false si elle est absente
bool Lookup(const char* section, const char* key, std::string* value)
{
    if (!g_path[0])
        return false;
    const std::string id = Lower(section, strlen(section)) + '\n' + Lower(key, strlen(key));
    std::lock_guard lock(g_mutex);
    Refresh();
    const auto it = g_values.find(id);
    if (it == g_values.end())
        return false;
    *value = it->second;
    return true;
}

// Comme GetPrivateProfileInt : signe, préfixes 0x / 0o / 0b, sinon décimal ; lecture
// arrêtée au premier caractère qui n'est pas un chiffre (0 si aucun chiffre)
void ParseInt(const std::string& text, int* out)
{
    const char* p = text.c_str();
    while (*p == ' ' || *p == '\t')
        ++p;
    bool negative = false;
    if (*p == '-' || *p == '+')
        negative = *p++ == '-';
    int base = 10;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
        base = 16, p += 2;
    else if (p[0] == '0' && (p[1] == 'o' || p[1] == 'O'))
        base = 8, p += 2;
    else if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B'))
        base = 2, p += 2;
    unsigned value = 0;
    bool digits = false;
    for (;; ++p) {
        int d;
        if (*p >= '0' && *p <= '9')
            d = *p - '0';
        else if (*p >= 'a' && *p <= 'f')
            d = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F')
            d = *p - 'A' + 10;
        else
            break;
        if (d >= base)
            break;
        value = value * static_cast<unsigned>(base) + static_cast<unsigned>(d);
        digits = true;
    }
    *out = digits ? static_cast<int>(negative ? 0u - value : value) : 0;
}

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
    std::string text;
    // Comme GetPrivateProfileInt : clé absente ou vide = valeur par défaut, illisible = 0
    if (!Lookup(section, key, &text) || text.empty())
        return defaultValue;
    int value = 0;
    ParseInt(text, &value);
    return value;
}

float Float(const char* section, const char* key, float defaultValue)
{
    std::string text;
    if (!Lookup(section, key, &text) || text.empty())
        return defaultValue;
    char* end = nullptr;
    const double value = strtod(text.c_str(), &end);
    return end != text.c_str() ? static_cast<float>(value) : defaultValue;
}

} // namespace config
