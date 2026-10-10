#pragma once
// Réglages de mise au point : sr2_vr.ini à côté de la DLL (ou de l'exe du banc d'essai).
// Fichier facultatif, lu directement à chaque appel : les appelants relisent leurs réglages
// une fois par seconde, ce qui permet de les changer pendant que le jeu tourne.
#include <windows.h>

namespace config {

void Init(HMODULE self);
const char* Path();
int Int(const char* section, const char* key, int defaultValue);
float Float(const char* section, const char* key, float defaultValue);

} // namespace config
