#pragma once
#include <windows.h>

namespace vrlog {

// Ouvre le fichier de log à côté du module (vidé à chaque session)
void Init(HMODULE self, const char* fileName = "sr2_vr.log");

// Écrit une ligne : Write("[render]", "Present hooké à %p", addr)
void Write(const char* tag, const char* fmt, ...);

} // namespace vrlog

#define LOG_CORE(...)   ::vrlog::Write("[core]", __VA_ARGS__)
#define LOG_RENDER(...) ::vrlog::Write("[render]", __VA_ARGS__)
#define LOG_XR(...)     ::vrlog::Write("[xr]", __VA_ARGS__)
#define LOG_ENGINE(...) ::vrlog::Write("[engine]", __VA_ARGS__)
