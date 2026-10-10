// SR2VR - outil de configuration et lanceur (sr2_vr_config.exe)
// Édite sr2_vr.ini, à côté de l'outil et de la DLL. La liste des réglages est dans settings.* ;
// la DLL relit le fichier chaque seconde, donc « Enregistrer » agit pendant que le jeu tourne.
// Français ou anglais ([Tool] language ; par défaut la langue de Windows).
//
// Interface reprise de l'outil de WarbandVR : entièrement dessinée dans la fenêtre (GDI, double
// tampon), thème sombre, gros textes lisibles dans un casque, zoom réglable ([Tool] ui_zoom),
// listes ouvertes dans la fenêtre même. Seul le champ texte du dossier du jeu est natif.
//
// --launcher : ouvert par sr2_vr_injector avant le jeu. Jouer enregistre et rend 0, Quitter (ou
// la fermeture) rend 1 : le jeu n'est pas lancé.
#define SR2VR_WIDEN2(x) L##x
#define SR2VR_WIDEN(x) SR2VR_WIDEN2(x)
#define SR2VR_WVERSION SR2VR_WIDEN(SR2VR_VERSION)
#include "settings.h"

#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

#ifdef _MSC_VER
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

namespace {

using settings::Kind;

constexpr UINT_PTR kTimerStatus = 1;
constexpr int kIdEdit = 200;

// --- État ---------------------------------------------------------------------------------
HWND g_window, g_edit;
std::wstring g_path;
bool g_french = true;
bool g_launcher = false;
int g_exitCode = 0;
int g_dpi = 96;
float g_zoom = 1.0f;
HFONT g_fontBody, g_fontBold, g_fontSmall, g_fontTitle, g_fontButton;
HBRUSH g_editBrush;

int g_tab = settings::kTabLaunch;
int g_scroll = 0, g_contentHeight = 0;
RECT g_content{};

std::wstring g_help, g_status;
int g_hoverRow = -1;
std::vector<int> g_rows; // réglages de l'onglet courant (rangs dans la table)
int g_textSetting = -1;  // réglage texte (dossier du jeu), édité par le champ natif

// Liste ouverte dans la fenêtre (remplace les listes déroulantes)
struct Overlay {
    bool open = false;
    int row = -1;
    std::wstring title;
    std::vector<std::wstring> items;
    int selected = -1;
    int scroll = 0;
    RECT panel{}, list{};
    int itemHeight = 0;
} g_overlay;

// Zones cliquables, reconstruites à chaque dessin ; la dernière ajoutée est au-dessus
enum ZoneType {
    kZNone,
    kZPlay, kZQuit, kZLanguage, kZZoomOut, kZZoomIn, kZDefaults,
    kZTab, kZRow, kZToggle, kZPrev, kZNext, kZValue, kZMinus, kZPlus, kZSlider,
    kZScrollTrack, kZOverlayBg, kZOverlayItem, kZOverlayClose,
};
struct Zone {
    RECT r;
    ZoneType type;
    int a; // onglet, ligne, élément
};
std::vector<Zone> g_zones;
Zone g_down{{}, kZNone, -1};
Zone g_hover{{}, kZNone, -1};

enum DragKind { kDragNone, kDragSlider, kDragContent, kDragThumb, kDragOverlay };
DragKind g_drag = kDragNone;
int g_dragRow = -1;
RECT g_dragRect{};
POINT g_dragStart{};
int g_dragStartScroll = 0;
bool g_dragMoved = false;

// --- Couleurs (thème sombre, lisible dans un casque) ---------------------------------------
constexpr COLORREF kBg = RGB(24, 26, 31), kPanel = RGB(33, 36, 43), kSidebar = RGB(20, 22, 26),
                   kHover = RGB(46, 51, 61), kBorder = RGB(62, 68, 80), kText = RGB(236, 238, 242),
                   kDim = RGB(160, 167, 178), kAccent = RGB(140, 82, 220), kPlay = RGB(46, 160, 67),
                   kWarn = RGB(230, 170, 60), kOff = RGB(80, 86, 98);

int S(float v)
{
    return static_cast<int>(std::lround(v * g_zoom * static_cast<float>(g_dpi) / 96.0f));
}

const wchar_t* T(const wchar_t* en, const wchar_t* fr)
{
    return g_french ? fr : en;
}

std::vector<std::wstring> Split(const wchar_t* choices)
{
    std::vector<std::wstring> out(1);
    for (const wchar_t* c = choices; c && *c; ++c) {
        if (*c == L'|')
            out.emplace_back();
        else
            out.back() += *c;
    }
    return out;
}

void SetStatus(const std::wstring& text)
{
    g_status = text;
    SetTimer(g_window, kTimerStatus, 8000, nullptr);
    InvalidateRect(g_window, nullptr, FALSE);
}

// --- Lignes de réglages -------------------------------------------------------------------
const wchar_t* TabName(int tab)
{
    switch (tab) {
    case settings::kTabLaunch: return T(L"Launch", L"Lancement");
    case settings::kTabDisplay: return T(L"Display", L"Affichage");
    case settings::kTabView: return T(L"View", L"Vue");
    case settings::kTabControls: return T(L"Controls", L"Contrôles");
    case settings::kTabHands: return T(L"Hands and aiming", L"Mains et visée");
    default: return T(L"Advanced", L"Avancé");
    }
}

// Rappel affiché en tête d'onglet ("" : aucun)
std::wstring TabNote(int tab)
{
    switch (tab) {
    case settings::kTabLaunch:
        return T(L"Put the headset on and start SteamVR before clicking PLAY. Keep the game window in the foreground "
                 L"while playing: the sticks go through the keyboard and the mouse.",
                 L"Mettez le casque et lancez SteamVR avant de cliquer sur JOUER. Gardez la fenêtre du jeu au premier "
                 L"plan pendant la partie : les sticks passent par le clavier et la souris.");
    case settings::kTabControls:
        return T(L"Left stick: move (click: next view).  Right stick: turn, up = jump, down = crouch (click: weapon "
                 L"wheel).  Triggers: fire, each hand its own gun.  Right grip: action.  Left grip: sprint.  X: reload.  "
                 L"A: confirm.  B or left menu: back / pause.",
                 L"Stick gauche : déplacement (clic : vue suivante).  Stick droit : tourner, haut = sauter, bas = "
                 L"s'accroupir (clic : roue des armes).  Gâchettes : tir, chaque main son arme.  Poignée droite : action.  "
                 L"Poignée gauche : courir.  X : recharger.  A : valider.  B ou menu gauche : retour / pause.");
    default:
        return {};
    }
}

void BuildRows()
{
    g_rows.clear();
    for (int i = 0; i < settings::Count(); ++i)
        if (settings::Def(i).tab == g_tab)
            g_rows.push_back(i);
}

const settings::Definition& DefOf(int row)
{
    return settings::Def(g_rows[row]);
}

std::vector<std::wstring> Choices(int row)
{
    return Split(DefOf(row).choices(g_french));
}

void Changed()
{
    InvalidateRect(g_window, nullptr, FALSE);
}

void SetChoice(int row, int k)
{
    const int n = static_cast<int>(Choices(row).size());
    if (n == 0)
        return;
    settings::Set(g_rows[row], static_cast<float>((k % n + n) % n));
    Changed();
}

void SetValue(int row, float v)
{
    settings::Set(g_rows[row], v);
    Changed();
}

// Pas « rond » proche de x (1, 2 ou 5 × 10^n)
float NiceStep(float x)
{
    if (x <= 0)
        return 1;
    const float p = std::pow(10.0f, std::floor(std::log10(x)));
    const float m = x / p;
    return (m < 1.5f ? 1.0f : m < 3.5f ? 2.0f : m < 7.5f ? 5.0f : 10.0f) * p;
}

float Step(int row)
{
    const settings::Definition& d = DefOf(row);
    return d.step > 0 ? d.step : NiceStep((d.maxValue - d.minValue) / 20);
}

float FineStep(int row)
{
    const settings::Definition& d = DefOf(row);
    return d.step > 0 ? d.step : NiceStep((d.maxValue - d.minValue) / 200);
}

float Snap(int row, float v)
{
    const settings::Definition& d = DefOf(row);
    const float f = FineStep(row);
    v = std::round(v / f) * f;
    return std::clamp(v, d.minValue, d.maxValue);
}

std::wstring FormatNumber(float v, float fine)
{
    const int decimals = std::clamp(static_cast<int>(std::ceil(-std::log10(fine) - 1e-4f)), 0, 4);
    wchar_t buf[64];
    swprintf_s(buf, L"%.*f", decimals, static_cast<double>(v));
    return buf;
}

std::wstring HelpOf(int row)
{
    const settings::Definition& d = DefOf(row);
    std::wstring def;
    if (d.kind == Kind::Text)
        return d.help(g_french);
    if (d.kind == Kind::Bool) {
        def = d.defaultValue != 0 ? T(L"yes", L"oui") : T(L"no", L"non");
    } else if (d.kind == Kind::Choice) {
        def = Split(d.choices(g_french))[static_cast<size_t>(d.defaultValue)];
    } else {
        const float f = FineStep(row);
        def = FormatNumber(d.defaultValue, f) + T(L", from ", L", de ") + FormatNumber(d.minValue, f) + T(L" to ", L" à ") +
              FormatNumber(d.maxValue, f);
    }
    return std::wstring(d.help(g_french)) + T(L"  (default: ", L"  (défaut : ") + def + L")";
}

bool SaveAll()
{
    const bool ok = settings::Save(g_path);
    WritePrivateProfileStringW(L"Tool", L"language", g_french ? L"fr" : L"en", g_path.c_str());
    wchar_t zoom[16];
    swprintf_s(zoom, L"%g", static_cast<double>(g_zoom));
    WritePrivateProfileStringW(L"Tool", L"ui_zoom", zoom, g_path.c_str());
    if (!ok)
        SetStatus(std::wstring(T(L"Cannot write the settings: ", L"Écriture impossible des réglages : ")) + g_path);
    return ok;
}

// --- Polices -------------------------------------------------------------------------------
HFONT MakeFont(int px, int weight)
{
    return CreateFontW(-S(static_cast<float>(px)), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

void CreateFonts()
{
    for (HFONT f : {g_fontBody, g_fontBold, g_fontSmall, g_fontTitle, g_fontButton})
        if (f)
            DeleteObject(f);
    g_fontBody = MakeFont(20, FW_NORMAL);
    g_fontBold = MakeFont(20, FW_SEMIBOLD);
    g_fontSmall = MakeFont(16, FW_NORMAL);
    g_fontTitle = MakeFont(26, FW_BOLD);
    g_fontButton = MakeFont(19, FW_SEMIBOLD);
    if (g_edit)
        SendMessageW(g_edit, WM_SETFONT, reinterpret_cast<WPARAM>(g_fontBody), TRUE);
}

// --- Dessin --------------------------------------------------------------------------------
void Fill(HDC dc, const RECT& r, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

void Round(HDC dc, const RECT& r, int radius, COLORREF fill, COLORREF border)
{
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
}

void Text(HDC dc, RECT r, const std::wstring& s, HFONT font, COLORREF color, UINT flags)
{
    SelectObject(dc, font);
    SetTextColor(dc, color);
    DrawTextW(dc, s.c_str(), static_cast<int>(s.size()), &r, flags | DT_NOPREFIX);
}

// Triangle plein dans r : dir -1 gauche, 1 droite, 2 bas
void Triangle(HDC dc, const RECT& r, int dir, COLORREF c)
{
    const int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, s = S(7);
    POINT pts[3];
    if (dir == -1)
        pts[0] = {cx - s, cy}, pts[1] = {cx + s / 2 + 1, cy - s}, pts[2] = {cx + s / 2 + 1, cy + s};
    else if (dir == 1)
        pts[0] = {cx + s, cy}, pts[1] = {cx - s / 2 - 1, cy - s}, pts[2] = {cx - s / 2 - 1, cy + s};
    else
        pts[0] = {cx, cy + s * 2 / 3}, pts[1] = {cx - s, cy - s / 2}, pts[2] = {cx + s, cy - s / 2};
    HBRUSH b = CreateSolidBrush(c);
    HPEN p = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    Polygon(dc, pts, 3);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
}

// Signe − / + dessiné (barres), comme les triangles
void Sign(HDC dc, const RECT& r, bool plus, COLORREF c)
{
    const int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, l = S(9), t = std::max(2, S(3)) / 2;
    Fill(dc, RECT{cx - l, cy - t, cx + l, cy + t + 1}, c);
    if (plus)
        Fill(dc, RECT{cx - t, cy - l, cx + t + 1, cy + l}, c);
}

bool Hovered(ZoneType t, int a = -1)
{
    return g_hover.type == t && (a < 0 || g_hover.a == a);
}

void AddZone(RECT r, ZoneType t, int a = -1, const RECT* clip = nullptr)
{
    if (clip) {
        RECT c;
        if (!IntersectRect(&c, &r, clip))
            return;
        r = c;
    }
    g_zones.push_back({r, t, a});
}

enum ButtonStyle { kNormal, kPrimary, kGo };

COLORREF Lighter(COLORREF c)
{
    return RGB(std::min(255, GetRValue(c) + 25), std::min(255, GetGValue(c) + 25), std::min(255, GetBValue(c) + 25));
}

void Button(HDC dc, const RECT& r, const std::wstring& label, ZoneType t, ButtonStyle style = kNormal)
{
    COLORREF fill = style == kPrimary ? kAccent : style == kGo ? kPlay : kPanel;
    if (Hovered(t))
        fill = style == kNormal ? kHover : Lighter(fill);
    Round(dc, r, S(10), fill, style == kNormal ? kBorder : fill);
    Text(dc, r, label, g_fontButton, kText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    AddZone(r, t);
}

RECT g_editRect{}; // position voulue du champ texte (dossier du jeu), vide : masqué

void DrawRow(HDC dc, int i, RECT rr)
{
    const settings::Definition& d = DefOf(i);
    const float value = settings::Get(g_rows[i]);
    const bool hover = g_hoverRow == i;
    Round(dc, rr, S(12), hover ? kHover : kPanel, hover ? kBorder : kPanel);
    AddZone(rr, kZRow, i, &g_content);

    const int pad = S(16), btn = S(46), h = S(44);
    const int width = rr.right - rr.left;
    const int ctrlW = std::clamp(width * 48 / 100, S(300), S(560));
    RECT ctrl{rr.right - pad - ctrlW, (rr.top + rr.bottom - h) / 2, rr.right - pad, (rr.top + rr.bottom + h) / 2};
    // Libellé centré verticalement, sur deux lignes s'il le faut
    {
        RECT label{rr.left + pad, rr.top + S(4), ctrl.left - pad, rr.bottom - S(4)};
        const std::wstring l = d.label(g_french);
        RECT probe = label;
        SelectObject(dc, g_fontBold);
        DrawTextW(dc, l.c_str(), static_cast<int>(l.size()), &probe, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        const int th = std::min(probe.bottom - probe.top, label.bottom - label.top);
        label.top += (label.bottom - label.top - th) / 2;
        Text(dc, label, l, g_fontBold, kText, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
    }

    switch (d.kind) {
    case Kind::Bool: {
        const bool on = value != 0;
        RECT sw{ctrl.right - S(92), ctrl.top + S(4), ctrl.right, ctrl.bottom - S(4)};
        Round(dc, sw, sw.bottom - sw.top, on ? kAccent : kOff, on ? kAccent : kOff);
        const int k = sw.bottom - sw.top - S(8);
        RECT knob{on ? sw.right - S(4) - k : sw.left + S(4), sw.top + S(4), 0, 0};
        knob.right = knob.left + k;
        knob.bottom = knob.top + k;
        Round(dc, knob, k, kText, kText);
        RECT txt{ctrl.left, ctrl.top, sw.left - S(14), ctrl.bottom};
        Text(dc, txt, on ? T(L"Yes", L"Oui") : T(L"No", L"Non"), g_fontBody, on ? kText : kDim,
             DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        AddZone(RECT{sw.left - S(80), ctrl.top, ctrl.right, ctrl.bottom}, kZToggle, i, &g_content);
        break;
    }
    case Kind::Choice: {
        RECT prev{ctrl.left, ctrl.top, ctrl.left + btn, ctrl.bottom};
        RECT next{ctrl.right - btn, ctrl.top, ctrl.right, ctrl.bottom};
        RECT box{prev.right + S(6), ctrl.top, next.left - S(6), ctrl.bottom};
        Round(dc, prev, S(10), Hovered(kZPrev, i) ? kHover : kBg, kBorder);
        Round(dc, next, S(10), Hovered(kZNext, i) ? kHover : kBg, kBorder);
        Triangle(dc, prev, -1, kText);
        Triangle(dc, next, 1, kText);
        Round(dc, box, S(10), Hovered(kZValue, i) ? kHover : kBg, kBorder);
        const std::vector<std::wstring> items = Choices(i);
        const int k = static_cast<int>(value);
        Text(dc, RECT{box.left + S(12), box.top, box.right - S(30), box.bottom},
             k >= 0 && k < static_cast<int>(items.size()) ? items[static_cast<size_t>(k)] : std::wstring(), g_fontBody, kText,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        Triangle(dc, RECT{box.right - S(30), box.top, box.right - S(6), box.bottom}, 2, kDim);
        AddZone(prev, kZPrev, i, &g_content);
        AddZone(next, kZNext, i, &g_content);
        AddZone(box, kZValue, i, &g_content);
        break;
    }
    case Kind::Float: {
        const int valueW = S(96);
        RECT minus{ctrl.left, ctrl.top, ctrl.left + btn, ctrl.bottom};
        RECT valueBox{ctrl.right - valueW, ctrl.top, ctrl.right, ctrl.bottom};
        RECT plus{valueBox.left - S(8) - btn, ctrl.top, valueBox.left - S(8), ctrl.bottom};
        RECT slider{minus.right + S(14), ctrl.top, plus.left - S(14), ctrl.bottom};
        Round(dc, minus, S(10), Hovered(kZMinus, i) ? kHover : kBg, kBorder);
        Round(dc, plus, S(10), Hovered(kZPlus, i) ? kHover : kBg, kBorder);
        Sign(dc, minus, false, kText);
        Sign(dc, plus, true, kText);
        const float t = std::clamp((value - d.minValue) / std::max(1e-6f, d.maxValue - d.minValue), 0.0f, 1.0f);
        const int cy = (slider.top + slider.bottom) / 2, knob = S(26);
        Round(dc, RECT{slider.left, cy - S(4), slider.right, cy + S(4)}, S(8), kOff, kOff);
        const int kx = slider.left + knob / 2 + static_cast<int>(t * static_cast<float>(slider.right - slider.left - knob));
        Round(dc, RECT{slider.left, cy - S(4), kx, cy + S(4)}, S(8), kAccent, kAccent);
        Round(dc, RECT{kx - knob / 2, cy - knob / 2, kx + knob / 2, cy + knob / 2}, knob,
              Hovered(kZSlider, i) ? kText : kDim, kAccent);
        Round(dc, valueBox, S(10), kBg, kBorder);
        Text(dc, valueBox, FormatNumber(value, FineStep(i)), g_fontBody, kText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        AddZone(minus, kZMinus, i, &g_content);
        AddZone(plus, kZPlus, i, &g_content);
        AddZone(RECT{slider.left - S(8), ctrl.top, slider.right + S(8), ctrl.bottom}, kZSlider, i, &g_content);
        break;
    }
    case Kind::Text: {
        Round(dc, ctrl, S(10), kBg, kBorder);
        // Champ natif posé dessus s'il est entièrement visible
        RECT inner{ctrl.left + S(10), ctrl.top + S(9), ctrl.right - S(10), ctrl.bottom - S(6)};
        if (inner.top >= g_content.top && inner.bottom <= g_content.bottom)
            g_editRect = inner;
        break;
    }
    }
}

void Paint(HDC dc, const RECT& client)
{
    g_zones.clear();
    g_editRect = {};
    SetBkMode(dc, TRANSPARENT);
    Fill(dc, client, kBg);
    const int w = client.right, h = client.bottom, pad = S(16);

    // En-tête
    const int headerH = S(68);
    Fill(dc, RECT{0, 0, w, headerH}, kSidebar);
    int x = w - pad;
    const int bh = S(46), by = (headerH - bh) / 2;
    auto placeRight = [&](int bw) {
        RECT r{x - bw, by, x, by + bh};
        x -= bw + S(10);
        return r;
    };
    if (g_launcher) {
        Button(dc, placeRight(S(170)), T(L"PLAY", L"JOUER"), kZPlay, kGo);
        Button(dc, placeRight(S(130)), T(L"Quit", L"Quitter"), kZQuit);
    } else {
        Button(dc, placeRight(S(170)), T(L"Save", L"Enregistrer"), kZPlay, kPrimary);
        Button(dc, placeRight(S(130)), T(L"Close", L"Fermer"), kZQuit);
    }
    x -= S(16);
    Button(dc, placeRight(S(56)), L"+", kZZoomIn);
    Button(dc, placeRight(S(56)), L"-", kZZoomOut);
    Button(dc, placeRight(S(120)), g_french ? L"English" : L"Français", kZLanguage);
    Text(dc, RECT{pad, 0, x, headerH},
         std::wstring(L"SR2VR ") + SR2VR_WVERSION + L"  -  " + (g_launcher ? T(L"Launch", L"Lancement") : T(L"Settings", L"Réglages")),
         g_fontTitle, kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    // Pied : aide de la ligne survolée, message
    const int footerH = S(104);
    const int bodyTop = headerH, bodyBottom = h - footerH;
    Fill(dc, RECT{0, bodyBottom, w, h}, kSidebar);
    std::wstring help = g_help;
    if (help.empty())
        help = g_launcher ? T(L"Choose the settings, then click PLAY. Point at a setting to see what it does.",
                              L"Choisissez les réglages, puis cliquez sur JOUER. Pointez un réglage pour voir son rôle.")
                          : T(L"Point at a setting to see what it does. Save writes the settings: a running game applies them "
                              L"within a second.",
                              L"Pointez un réglage pour voir son rôle. Enregistrer écrit les réglages : un jeu en cours les "
                              L"applique en une seconde.");
    Text(dc, RECT{pad, bodyBottom + S(8), w - pad, bodyBottom + S(66)}, help, g_fontBody, kDim, DT_LEFT | DT_WORDBREAK);
    Text(dc, RECT{pad, h - S(34), w - pad, h - S(6)}, g_status.empty() ? g_path : g_status, g_fontSmall,
         g_status.empty() ? kOff : kWarn, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);

    // Onglets
    const int sideW = S(250);
    Fill(dc, RECT{0, bodyTop, sideW, bodyBottom}, kSidebar);
    int ty = bodyTop + S(6);
    for (int t = 0; t < settings::kTabCount; ++t) {
        RECT tr{S(8), ty, sideW - S(8), ty + S(54)};
        const bool sel = t == g_tab;
        if (sel || Hovered(kZTab, t))
            Round(dc, tr, S(10), sel ? kPanel : kHover, sel ? kPanel : kHover);
        if (sel)
            Fill(dc, RECT{tr.left, tr.top + S(10), tr.left + S(5), tr.bottom - S(10)}, kAccent);
        Text(dc, RECT{tr.left + S(18), tr.top, tr.right - S(8), tr.bottom}, TabName(t), sel ? g_fontBold : g_fontBody,
             sel ? kText : kDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        AddZone(tr, kZTab, t);
        ty += S(60);
    }
    // Valeurs par défaut de l'onglet, en bas de la colonne
    if (bodyBottom - S(60) > ty)
        Button(dc, RECT{S(8), bodyBottom - S(56), sideW - S(8), bodyBottom - S(10)}, T(L"Tab defaults", L"Défauts de l'onglet"),
               kZDefaults);

    // Contenu défilant
    g_content = {sideW + pad, bodyTop + S(8), w - pad, bodyBottom};
    const int saved = SaveDC(dc);
    IntersectClipRect(dc, g_content.left, g_content.top, g_content.right, g_content.bottom);
    int y = g_content.top + S(4) - g_scroll;
    const int rowH = S(74), scrollW = S(14);
    const int rowRight = g_content.right - scrollW - S(10);
    if (const std::wstring note = TabNote(g_tab); !note.empty()) {
        RECT probe{g_content.left + S(16), 0, rowRight - S(16), 0};
        SelectObject(dc, g_fontBody);
        DrawTextW(dc, note.c_str(), static_cast<int>(note.size()), &probe, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        const int nh = probe.bottom - probe.top + S(22);
        RECT nr{g_content.left, y, rowRight, y + nh};
        Round(dc, nr, S(12), kSidebar, kBorder);
        Text(dc, RECT{nr.left + S(16), nr.top + S(10), nr.right - S(16), nr.bottom - S(8)}, note, g_fontBody, kDim,
             DT_LEFT | DT_WORDBREAK);
        y += nh + S(10);
    }
    for (int i = 0; i < static_cast<int>(g_rows.size()); ++i) {
        RECT rr{g_content.left, y, rowRight, y + rowH};
        if (rr.bottom > g_content.top && rr.top < g_content.bottom)
            DrawRow(dc, i, rr);
        y += rowH + S(8);
    }
    g_contentHeight = y + g_scroll - g_content.top + S(8);
    RestoreDC(dc, saved);

    // Barre de défilement
    const int visible = g_content.bottom - g_content.top;
    if (g_contentHeight > visible) {
        RECT track{g_content.right - scrollW, g_content.top, g_content.right, g_content.bottom};
        Round(dc, track, scrollW, kSidebar, kSidebar);
        const int thumbH = std::max(S(40), visible * visible / g_contentHeight);
        const int thumbY = track.top + (visible - thumbH) * g_scroll / std::max(1, g_contentHeight - visible);
        Round(dc, RECT{track.left, thumbY, track.right, thumbY + thumbH}, scrollW, Hovered(kZScrollTrack) ? kDim : kOff, kOff);
        AddZone(track, kZScrollTrack);
    }

    // Liste ouverte : par-dessus tout
    if (g_overlay.open) {
        Overlay& o = g_overlay;
        AddZone(client, kZOverlayBg);
        o.itemHeight = S(54);
        const int titleH = S(60);
        const int pw = std::min(S(620), w - 2 * pad);
        const int listH =
            std::max(o.itemHeight, std::min(static_cast<int>(o.items.size()) * o.itemHeight, h - S(160) - titleH));
        const int ph = titleH + listH + S(16);
        o.panel = {(w - pw) / 2, (h - ph) / 2, (w + pw) / 2, (h + ph) / 2};
        Round(dc, RECT{o.panel.left - S(3), o.panel.top - S(3), o.panel.right + S(3), o.panel.bottom + S(3)}, S(16), kBg,
              kAccent);
        Round(dc, o.panel, S(14), kPanel, kPanel);
        AddZone(o.panel, kZNone);
        Text(dc, RECT{o.panel.left + S(18), o.panel.top, o.panel.right - S(70), o.panel.top + titleH}, o.title, g_fontBold,
             kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        Button(dc, RECT{o.panel.right - S(62), o.panel.top + S(8), o.panel.right - S(10), o.panel.top + titleH - S(8)}, L"X",
               kZOverlayClose);
        o.list = {o.panel.left + S(8), o.panel.top + titleH, o.panel.right - S(8), o.panel.top + titleH + listH};
        const int maxScroll = std::max(0, static_cast<int>(o.items.size()) * o.itemHeight - listH);
        o.scroll = std::clamp(o.scroll, 0, maxScroll);
        const int s2 = SaveDC(dc);
        IntersectClipRect(dc, o.list.left, o.list.top, o.list.right, o.list.bottom);
        for (int k = 0; k < static_cast<int>(o.items.size()); ++k) {
            RECT ir{o.list.left, o.list.top + k * o.itemHeight - o.scroll, o.list.right - S(10),
                    o.list.top + (k + 1) * o.itemHeight - o.scroll};
            if (ir.bottom < o.list.top || ir.top > o.list.bottom)
                continue;
            const bool sel = k == o.selected;
            if (sel || Hovered(kZOverlayItem, k))
                Round(dc, RECT{ir.left, ir.top + S(3), ir.right, ir.bottom - S(3)}, S(10), sel ? kAccent : kHover,
                      sel ? kAccent : kHover);
            Text(dc, RECT{ir.left + S(16), ir.top, ir.right - S(8), ir.bottom}, o.items[static_cast<size_t>(k)], g_fontBody,
                 kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            AddZone(ir, kZOverlayItem, k, &o.list);
        }
        RestoreDC(dc, s2);
        if (maxScroll > 0) { // indicateur de défilement
            const int th = std::max(S(30), listH * listH / (listH + maxScroll));
            const int tyy = o.list.top + (listH - th) * o.scroll / maxScroll;
            Round(dc, RECT{o.list.right - S(8), tyy, o.list.right - S(2), tyy + th}, S(6), kDim, kDim);
        }
    }
}

// Défilement du contenu, borné
void ScrollTo(int v)
{
    const int visible = g_content.bottom - g_content.top;
    g_scroll = std::clamp(v, 0, std::max(0, g_contentHeight - visible));
    InvalidateRect(g_window, nullptr, FALSE);
}

void SelectTab(int t)
{
    g_tab = t;
    g_scroll = 0;
    g_hoverRow = -1;
    g_help.clear();
    BuildRows();
    InvalidateRect(g_window, nullptr, FALSE);
}

const Zone* HitTest(POINT p)
{
    for (auto it = g_zones.rbegin(); it != g_zones.rend(); ++it)
        if (PtInRect(&it->r, p))
            return &*it;
    return nullptr;
}

void OpenOverlay(int row)
{
    Overlay& o = g_overlay;
    o = Overlay{};
    o.open = true;
    o.row = row;
    o.title = DefOf(row).label(g_french);
    o.items = Choices(row);
    o.selected = static_cast<int>(settings::Get(g_rows[row]));
    o.scroll = std::max(0, o.selected - 3) * S(54); // élément choisi visible
    InvalidateRect(g_window, nullptr, FALSE);
}

void SetSliderFromX(int x)
{
    if (g_dragRow < 0 || g_dragRow >= static_cast<int>(g_rows.size()))
        return;
    const settings::Definition& d = DefOf(g_dragRow);
    const int knob = S(26);
    const RECT& r = g_dragRect; // zone du curseur, élargie de S(8) de chaque côté
    const float t = std::clamp(static_cast<float>(x - r.left - S(8) - knob / 2) /
                                   static_cast<float>(std::max(1, static_cast<int>(r.right - r.left) - S(16) - knob)),
                               0.0f, 1.0f);
    SetValue(g_dragRow, Snap(g_dragRow, d.minValue + t * (d.maxValue - d.minValue)));
}

void Click(const Zone& z)
{
    const int row = z.a >= 0 && z.a < static_cast<int>(g_rows.size()) ? z.a : -1;
    switch (z.type) {
    case kZPlay:
        if (SaveAll()) {
            if (g_launcher) {
                g_exitCode = 0;
                DestroyWindow(g_window);
                return;
            }
            SetStatus(T(L"Saved. A running game applies the settings within a second.",
                        L"Enregistré. Un jeu en cours applique les réglages en une seconde."));
        }
        break;
    case kZQuit:
        DestroyWindow(g_window);
        break;
    case kZLanguage:
        g_french = !g_french;
        g_help.clear();
        g_hoverRow = -1;
        InvalidateRect(g_window, nullptr, FALSE);
        break;
    case kZZoomOut:
    case kZZoomIn:
        g_zoom = std::clamp(g_zoom + (z.type == kZZoomIn ? 0.125f : -0.125f), 0.75f, 2.5f);
        CreateFonts();
        ScrollTo(g_scroll);
        break;
    case kZDefaults:
        settings::Reset(static_cast<settings::Tab>(g_tab));
        SetStatus(std::wstring(T(L"Default values restored for the tab: ", L"Valeurs par défaut remises pour l'onglet : ")) +
                  TabName(g_tab) + (g_launcher ? T(L" (written when you click PLAY)", L" (écrites en cliquant sur JOUER)")
                                               : T(L" (written when you click Save)", L" (écrites en cliquant sur Enregistrer)")));
        break;
    case kZTab:
        SelectTab(z.a);
        break;
    case kZToggle:
        if (row >= 0)
            SetValue(row, settings::Get(g_rows[row]) != 0 ? 0.0f : 1.0f);
        break;
    case kZPrev:
    case kZNext:
        if (row >= 0)
            SetChoice(row, static_cast<int>(settings::Get(g_rows[row])) + (z.type == kZNext ? 1 : -1));
        break;
    case kZValue:
        if (row >= 0)
            OpenOverlay(row);
        break;
    case kZMinus:
    case kZPlus:
        if (row >= 0)
            SetValue(row, Snap(row, settings::Get(g_rows[row]) + (z.type == kZPlus ? Step(row) : -Step(row))));
        break;
    case kZOverlayItem:
        g_overlay.open = false;
        if (z.a >= 0 && z.a < static_cast<int>(g_overlay.items.size()))
            SetChoice(g_overlay.row, z.a);
        InvalidateRect(g_window, nullptr, FALSE);
        break;
    case kZOverlayBg:
    case kZOverlayClose:
        g_overlay.open = false;
        InvalidateRect(g_window, nullptr, FALSE);
        break;
    default:
        break;
    }
}

void OnMouseMove(POINT p)
{
    if (g_drag == kDragSlider) {
        SetSliderFromX(p.x);
        return;
    }
    if (g_drag == kDragContent || g_drag == kDragOverlay) {
        const int dy = p.y - g_dragStart.y;
        if (std::abs(dy) > S(8))
            g_dragMoved = true;
        if (g_dragMoved) {
            if (g_drag == kDragContent) {
                ScrollTo(g_dragStartScroll - dy);
            } else {
                g_overlay.scroll = g_dragStartScroll - dy;
                InvalidateRect(g_window, nullptr, FALSE);
            }
        }
        return;
    }
    if (g_drag == kDragThumb) {
        const int visible = g_content.bottom - g_content.top;
        ScrollTo(g_dragStartScroll + (p.y - g_dragStart.y) * g_contentHeight / std::max(1, visible));
        return;
    }
    const Zone* z = HitTest(p);
    const Zone hover = z ? *z : Zone{{}, kZNone, -1};
    int row = -1;
    if (!g_overlay.open && z && z->type >= kZRow && z->type <= kZSlider)
        row = z->a;
    if (hover.type != g_hover.type || hover.a != g_hover.a || row != g_hoverRow) {
        g_hover = hover;
        if (row != g_hoverRow) {
            g_hoverRow = row;
            if (row >= 0 && row < static_cast<int>(g_rows.size()))
                g_help = HelpOf(row);
        }
        InvalidateRect(g_window, nullptr, FALSE);
    }
    TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, g_window, 0};
    TrackMouseEvent(&tme);
}

void OnMouseDown(POINT p)
{
    SetFocus(g_window);
    SetCapture(g_window);
    const Zone* z = HitTest(p);
    g_down = z ? *z : Zone{{}, kZNone, -1};
    g_dragStart = p;
    g_dragMoved = false;
    if (g_overlay.open) {
        if (PtInRect(&g_overlay.list, p)) {
            g_drag = kDragOverlay; // glisser la liste (pointeur du casque, écran tactile)
            g_dragStartScroll = g_overlay.scroll;
        }
        return;
    }
    if (z && z->type == kZSlider) {
        g_drag = kDragSlider;
        g_dragRow = z->a;
        g_dragRect = z->r;
        SetSliderFromX(p.x);
    } else if (z && z->type == kZScrollTrack) {
        g_drag = kDragThumb;
        // Clic hors du curseur : saut à cet endroit
        const int visible = g_content.bottom - g_content.top;
        const int thumbH = std::max(S(40), visible * visible / std::max(1, g_contentHeight));
        const int thumbY = g_content.top + (visible - thumbH) * g_scroll / std::max(1, g_contentHeight - visible);
        if (p.y < thumbY || p.y > thumbY + thumbH)
            ScrollTo((p.y - g_content.top - thumbH / 2) * g_contentHeight / std::max(1, visible));
        g_dragStartScroll = g_scroll;
    } else if (PtInRect(&g_content, p) && (!z || z->type == kZRow)) {
        g_drag = kDragContent; // glisser le contenu
        g_dragStartScroll = g_scroll;
    }
}

void OnMouseUp(POINT p)
{
    ReleaseCapture();
    const DragKind drag = g_drag;
    g_drag = kDragNone;
    if (drag == kDragSlider || drag == kDragThumb || g_dragMoved)
        return;
    const Zone* z = HitTest(p);
    if (z && z->type == g_down.type && z->a == g_down.a)
        Click(*z);
}

POINT PointOf(LPARAM lp)
{
    return {static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, std::max(1L, rc.right), std::max(1L, rc.bottom));
        HGDIOBJ old = SelectObject(mem, bmp);
        Paint(mem, rc);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        // Champ texte natif : là où le dessin l'a placé, masqué sinon (ou sous une liste ouverte)
        if (g_edit) {
            const bool show = !IsRectEmpty(&g_editRect) && !g_overlay.open;
            RECT cur{};
            GetWindowRect(g_edit, &cur);
            MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT*>(&cur), 2);
            if (show != (IsWindowVisible(g_edit) != FALSE) || (show && !EqualRect(&cur, &g_editRect)))
                SetWindowPos(g_edit, HWND_TOP, g_editRect.left, g_editRect.top, g_editRect.right - g_editRect.left,
                             g_editRect.bottom - g_editRect.top, (show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW) | SWP_NOACTIVATE);
        }
        return 0;
    }
    case WM_SIZE:
        ScrollTo(g_scroll);
        return 0;
    case WM_MOUSEMOVE:
        OnMouseMove(PointOf(lp));
        return 0;
    case WM_MOUSELEAVE:
        if (g_hover.type != kZNone) {
            g_hover = Zone{{}, kZNone, -1};
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN:
        OnMouseDown(PointOf(lp));
        return 0;
    case WM_LBUTTONUP:
        OnMouseUp(PointOf(lp));
        return 0;
    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wp) * S(80) / WHEEL_DELTA;
        if (g_overlay.open) {
            g_overlay.scroll -= delta;
            InvalidateRect(hwnd, nullptr, FALSE);
        } else {
            ScrollTo(g_scroll - delta);
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE && g_overlay.open) {
            g_overlay.open = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (wp == VK_RETURN && g_launcher && !g_overlay.open) {
            Click(Zone{{}, kZPlay, -1});
        } else if (wp == VK_PRIOR || wp == VK_NEXT) {
            ScrollTo(g_scroll + (wp == VK_NEXT ? 1 : -1) * (g_content.bottom - g_content.top) * 3 / 4);
        }
        return 0;
    case WM_COMMAND:
        // Windows envoie EN_CHANGE dès la création du champ, avant que g_edit soit affecté
        if (LOWORD(wp) == kIdEdit && HIWORD(wp) == EN_CHANGE && g_edit && g_textSetting >= 0) {
            const HWND edit = reinterpret_cast<HWND>(lp);
            const int len = GetWindowTextLengthW(edit);
            std::wstring text(static_cast<size_t>(len), L'\0');
            if (len)
                GetWindowTextW(edit, text.data(), len + 1);
            settings::SetText(g_textSetting, text);
        }
        return 0;
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, kText);
        SetBkColor(dc, kBg);
        return reinterpret_cast<LRESULT>(g_editBrush);
    }
    case WM_TIMER:
        if (wp == kTimerStatus) {
            KillTimer(hwnd, kTimerStatus);
            g_status.clear();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_DPICHANGED: {
        g_dpi = HIWORD(wp);
        CreateFonts();
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

std::wstring DirOfSelf()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s = path;
    return s.substr(0, s.find_last_of(L'\\'));
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmdLine, int show)
{
    g_launcher = cmdLine && wcsstr(cmdLine, L"--launcher");
    g_exitCode = g_launcher ? 1 : 0; // lanceur fermé sans Jouer : pas de partie
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    g_path = DirOfSelf() + L"\\sr2_vr.ini";
    settings::Load(g_path);
    {
        wchar_t text[16]{};
        GetPrivateProfileStringW(L"Tool", L"language", L"", text, 16, g_path.c_str());
        g_french = text[0] ? _wcsicmp(text, L"fr") == 0 : PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_FRENCH;
        GetPrivateProfileStringW(L"Tool", L"ui_zoom", L"1", text, 16, g_path.c_str());
        g_zoom = std::clamp(wcstof(text, nullptr), 0.75f, 2.5f);
    }
    for (int i = 0; i < settings::Count(); ++i)
        if (settings::Def(i).kind == Kind::Text)
            g_textSetting = i;
    BuildRows();
    g_editBrush = CreateSolidBrush(kBg);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"SR2VRConfig";
    RegisterClassExW(&wc);

    g_window = CreateWindowExW(0, L"SR2VRConfig", g_launcher ? L"SR2VR - Lancement" : L"SR2VR - Réglages",
                               WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 10, 10, nullptr, nullptr,
                               instance, nullptr);
    if (!g_window)
        return 1;
    g_dpi = static_cast<int>(GetDpiForWindow(g_window));
    CreateFonts();
    if (g_textSetting >= 0) {
        HWND edit = CreateWindowExW(0, L"EDIT", settings::GetText(g_textSetting).c_str(), WS_CHILD | ES_LEFT | ES_AUTOHSCROLL,
                                    0, 0, 10, 10, g_window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdEdit)), instance,
                                    nullptr);
        g_edit = edit;
        SendMessageW(g_edit, WM_SETFONT, reinterpret_cast<WPARAM>(g_fontBody), TRUE);
    }

    // Taille : 1240 x 900 (mise à l'échelle), bornée à l'écran ; écran plus petit : plein écran
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int ww = S(1240), wh = S(900);
    const int aw = work.right - work.left, ah = work.bottom - work.top;
    const bool fits = ww <= aw && wh <= ah;
    SetWindowPos(g_window, nullptr, work.left + std::max(0, (aw - ww) / 2), work.top + std::max(0, (ah - wh) / 2),
                 std::min(ww, aw), std::min(wh, ah), SWP_NOZORDER);
    ShowWindow(g_window, fits ? show : SW_SHOWMAXIMIZED);
    UpdateWindow(g_window);
    if (g_launcher)
        SetForegroundWindow(g_window);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    for (HFONT f : {g_fontBody, g_fontBold, g_fontSmall, g_fontTitle, g_fontButton})
        DeleteObject(f);
    DeleteObject(g_editBrush);
    return g_exitCode;
}
