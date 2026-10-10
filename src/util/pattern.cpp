#include "util/pattern.h"
#include "util/log.h"

#include <cstdlib>

namespace pattern {
namespace {

// -1 = joker
std::vector<int> Parse(const char* sig)
{
    std::vector<int> bytes;
    for (const char* p = sig; *p;) {
        if (*p == ' ') {
            ++p;
        } else if (*p == '?') {
            bytes.push_back(-1);
            while (*p == '?')
                ++p;
        } else {
            char* end = nullptr;
            bytes.push_back(static_cast<int>(strtoul(p, &end, 16)));
            p = end;
        }
    }
    return bytes;
}

bool Matches(const uint8_t* at, const std::vector<int>& sig)
{
    size_t k = 0;
    while (k < sig.size() && (sig[k] < 0 || at[k] == sig[k]))
        ++k;
    return k == sig.size();
}

} // namespace

uint8_t* FindIn(uint8_t* start, size_t size, const char* signature)
{
    const std::vector<int> sig = Parse(signature);
    if (sig.empty() || size < sig.size())
        return nullptr;
    for (size_t off = 0; off <= size - sig.size(); ++off)
        if (Matches(start + off, sig))
            return start + off;
    return nullptr;
}

void* Find(const char* name, const char* signature, HMODULE module)
{
    if (!module)
        module = GetModuleHandleW(nullptr);
    auto* base = reinterpret_cast<uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const std::vector<int> sig = Parse(signature);

    uint8_t* found = nullptr;
    int hits = 0;
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE))
            continue;
        uint8_t* start = base + sec->VirtualAddress;
        const size_t size = sec->Misc.VirtualSize;
        if (size < sig.size())
            continue;
        for (size_t off = 0; off <= size - sig.size(); ++off) {
            if (Matches(start + off, sig) && ++hits == 1)
                found = start + off;
        }
    }

    if (hits == 1) {
        LOG_CORE("signature %s trouvee : exe+0x%lX", name, static_cast<unsigned long>(found - base));
        return found;
    }
    LOG_CORE("ERREUR : signature %s %s (%d occurrences)", name, hits ? "ambigue" : "introuvable", hits);
    return nullptr;
}

} // namespace pattern
