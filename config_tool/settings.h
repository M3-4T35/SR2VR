#pragma once
// Réglages de SR2VR présentés par l'outil de configuration : une table (section et clé de
// sr2_vr.ini, libellés français et anglais, bornes, valeur par défaut) et leur lecture /
// écriture dans le fichier.
//
// La DLL lit sr2_vr.ini elle-même, clé par clé (src/util/config), une fois par seconde : ce
// que l'outil enregistre est donc pris en compte pendant que le jeu tourne. L'outil n'écrit
// que les clés modifiées et celles qui manquent (WritePrivateProfileString) : commentaires et
// réglages hors de la table restent tels quels.
#include <string>

namespace settings {

enum class Kind { Float, Bool, Choice, Text };
enum Tab { kTabLaunch, kTabDisplay, kTabView, kTabControls, kTabHands, kTabAdvanced, kTabCount };

struct Text {
    const wchar_t* en;
    const wchar_t* fr;
    const wchar_t* operator()(bool french) const { return french ? fr : en; }
};

struct Definition {
    const wchar_t* section;
    const wchar_t* key;
    Tab tab;
    Text label;
    Kind kind;
    float defaultValue;
    float minValue, maxValue;
    float step;   // pas des boutons et du curseur (0 : choisi d'après les bornes)
    Text choices; // Choice : libellés séparés par '|', la valeur est le rang
    Text help;
};

int Count();
const Definition& Def(int index);

float Get(int index);
void Set(int index, float value); // borné à [min, max] ; la clé sera écrite
std::wstring GetText(int index);
void SetText(int index, const std::wstring& value);
// Valeurs par défaut pour un onglet (ou tous : kTabCount), hors dossier du jeu
void Reset(Tab tab);
bool Modified();

// Fichier : clé absente = valeur par défaut. Save écrit les clés modifiées ou absentes.
void Load(const std::wstring& path);
bool Save(const std::wstring& path);

} // namespace settings
