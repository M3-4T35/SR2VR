#include "settings.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <iterator>

namespace settings {
namespace {

constexpr Text kNone{nullptr, nullptr};

// Valeurs par défaut = réglages validés au casque (Quest 2 et Steam Frame par SteamVR)
const Definition kDefs[] = {
    // --- Lancement (lu par l'injecteur) ---
    {L"Launch", L"game_dir", kTabLaunch, {L"Game folder", L"Dossier du jeu"}, Kind::Text, 0, 0, 0, 0, kNone,
     {L"Folder containing SR2_pc.exe. Empty: found automatically in the Steam libraries.",
      L"Dossier contenant SR2_pc.exe. Vide : trouvé automatiquement dans les bibliothèques Steam."}},
    {L"Launch", L"show_launcher", kTabLaunch, {L"Show this window at startup", L"Afficher cette fenêtre au démarrage"},
     Kind::Bool, 1, 0, 1, 0, kNone,
     {L"When the game is started with the mod, this window opens first. Off: the game starts directly "
      L"(sr2_vr_config.exe still opens the settings).",
      L"Au lancement du jeu avec le mod, cette fenêtre s'ouvre d'abord. Désactivé : le jeu démarre directement "
      L"(sr2_vr_config.exe ouvre toujours les réglages)."}},
    {L"Launch", L"openxr_runtime", kTabLaunch, {L"VR runtime (OpenXR)", L"Runtime VR (OpenXR)"}, Kind::Choice, 0, 0, 2,
     0, {L"Automatic|SteamVR|Windows default", L"Automatique|SteamVR|Celui de Windows"},
     {L"Automatic: the one set in Windows, except Meta's (its 32-bit version crashes), replaced by SteamVR. "
      L"SteamVR: Steam Frame, Index, Quest through Steam Link or Virtual Desktop. Applies at the next launch.",
      L"Automatique : celui de Windows, sauf celui de Meta (sa version 32 bits plante), remplacé par SteamVR. "
      L"SteamVR : Steam Frame, Index, Quest par Steam Link ou Virtual Desktop. Pris en compte au prochain lancement."}},

    // --- Affichage ---
    {L"Display", L"mode", kTabDisplay, {L"Display mode", L"Mode d'affichage"}, Kind::Choice, 3, 0, 3, 0,
     {L"Virtual screen|Immersive, no depth|Depth, one eye per frame|True stereo",
      L"Écran virtuel|Immersif sans relief|Relief, un œil par image|Vrai relief"},
     {L"True stereo: both eyes drawn for every game frame (recommended). One eye per frame: lighter, depth less "
      L"stable while moving. Virtual screen: the game on a big screen in front of you.",
      L"Vrai relief : les deux yeux dessinés pour chaque image du jeu (conseillé). Un œil par image : plus léger, "
      L"relief moins stable en mouvement. Écran virtuel : le jeu sur un grand écran devant vous."}},
    {L"Camera", L"hfov", kTabDisplay, {L"Rendered field of view (degrees)", L"Champ de vue du rendu (degrés)"},
     Kind::Float, 125, 60, 150, 1, kNone,
     {L"Horizontal field of view drawn by the game. It must cover the headset's: too small leaves black borders.",
      L"Champ de vue horizontal dessiné par le jeu. Il doit couvrir celui du casque : trop petit, des bords noirs "
      L"apparaissent."}},
    {L"Stereo", L"world_scale", kTabDisplay, {L"World scale", L"Échelle du monde"}, Kind::Float, 1.0f, 0.5f, 2.0f,
     0.01f, kNone,
     {L"Game units per metre. Higher: the world looks smaller (you feel taller). Lower: the world looks bigger.",
      L"Unités du jeu par mètre. Plus grand : le monde paraît plus petit (on se sent plus grand). Plus petit : le "
      L"monde paraît plus grand."}},
    {L"Stereo", L"ipd_scale", kTabDisplay, {L"Depth strength", L"Force du relief"}, Kind::Float, 1.0f, 0.0f, 2.0f,
     0.05f, kNone,
     {L"Correction of the distance between the eyes. 1: the one measured by the headset. 0: no depth.",
      L"Correction de l'écart entre les yeux. 1 : celui mesuré par le casque. 0 : pas de relief."}},
    {L"Display", L"interface_screen", kTabDisplay,
     {L"Shops and wardrobe on the virtual screen", L"Boutiques et garde-robe sur l'écran virtuel"}, Kind::Bool, 1, 0,
     1, 0, kNone,
     {L"Wardrobe, garage, shops and customisation are shown on a screen in front of you instead of around you.",
      L"Garde-robe, garage, boutiques et personnalisation s'affichent sur un écran devant vous plutôt qu'autour de "
      L"vous."}},
    {L"Display", L"cutscene_screen", kTabDisplay, {L"Cutscenes on the virtual screen", L"Cinématiques sur l'écran virtuel"},
     Kind::Bool, 1, 0, 1, 0, kNone,
     {L"Cutscenes are shown on a screen in front of you (their camera moves a lot).",
      L"Les cinématiques s'affichent sur un écran devant vous (leur caméra bouge beaucoup)."}},
    {L"Hud", L"scale", kTabDisplay, {L"HUD: size", L"Interface en partie : taille"}, Kind::Float, 0.38f, 0.1f, 1.0f,
     0.01f, kNone,
     {L"1: at the edges of the image, as in the game. Smaller: brought towards the centre, where the headset is sharp.",
      L"1 : aux bords de l'image, comme dans le jeu. Plus petit : ramenée vers le centre, là où le casque est net."}},
    {L"Hud", L"offset_y", kTabDisplay, {L"HUD: height", L"Interface en partie : hauteur"}, Kind::Float, 0, -300, 300,
     5, kNone,
     {L"Vertical shift of the HUD. Positive: lower.", L"Décalage vertical de l'interface. Positif : plus bas."}},
    {L"Hud", L"distance", kTabDisplay, {L"HUD: distance (m)", L"Interface en partie : distance (m)"}, Kind::Float, 5,
     0, 20, 0.5f, kNone,
     {L"Distance at which the HUD floats. 0: at infinity.",
      L"Distance à laquelle l'interface flotte. 0 : à l'infini."}},

    // --- Vue ---
    {L"FirstPerson", L"enabled", kTabView, {L"First-person view", L"Vue à la première personne"}, Kind::Bool, 1, 0, 1,
     0, kNone,
     {L"Camera at the character's eyes. Off: the game's third-person camera.",
      L"Caméra aux yeux du personnage. Désactivé : caméra du jeu, à la troisième personne."}},
    {L"FirstPerson", L"body_follow", kTabView, {L"Body turns towards where you look", L"Le corps se tourne vers le regard"},
     Kind::Bool, 1, 0, 1, 0, kNone,
     {L"On foot, standing or walking forward: the character (and its arrow on the minimap) turns to face where "
      L"you look. Off: it only turns towards where it walks, as in the game.",
      L"À pied, sur place ou en avançant : le personnage (et sa flèche sur la mini-carte) se tourne vers là où vous "
      L"regardez. Désactivé : il ne se tourne que vers là où il marche, comme dans le jeu."}},
    {L"FirstPerson", L"forward", kTabView, {L"Eyes: forward (m)", L"Yeux : vers l'avant (m)"}, Kind::Float, 0.12f,
     -0.3f, 0.5f, 0.01f, kNone,
     {L"Moves the camera forward from the centre of the head. Increase if you see the inside of the face.",
      L"Avance la caméra depuis le centre de la tête. À augmenter si l'intérieur du visage est visible."}},
    {L"FirstPerson", L"up", kTabView, {L"Eyes: upward (m)", L"Yeux : vers le haut (m)"}, Kind::Float, 0.06f, -0.3f,
     0.5f, 0.01f, kNone,
     {L"Raises the camera from the centre of the head.", L"Remonte la caméra depuis le centre de la tête."}},
    {L"FirstPerson", L"smooth", kTabView, {L"Head smoothing (s)", L"Lissage de la tête (s)"}, Kind::Float, 0.12f, 0,
     1, 0.01f, kNone,
     {L"Higher: less bobbing while running. 0: the camera follows every movement of the animation.",
      L"Plus grand : moins de balancement en courant. 0 : la caméra suit chaque mouvement de l'animation."}},
    {L"FirstPerson", L"vehicle_view", kTabView, {L"View in a vehicle", L"Vue en véhicule"}, Kind::Choice, 0, 0, 2, 0,
     {L"Driver's eyes|In front of the windshield|Game camera", L"Yeux du conducteur|Devant le pare-brise|Caméra du jeu"},
     {L"View used when entering a vehicle. Clicking the left stick switches to the next one while playing.",
      L"Vue prise en montant dans un véhicule. Le clic du stick gauche passe à la suivante en jeu."}},
    {L"FirstPerson", L"vehicle_forward", kTabView, {L"Windshield view: forward (m)", L"Vue pare-brise : avance (m)"},
     Kind::Float, 0.9f, -1, 3, 0.05f, kNone,
     {L"View in front of the windshield: distance ahead of the driver's head.",
      L"Vue devant le pare-brise : distance en avant de la tête du conducteur."}},
    {L"FirstPerson", L"vehicle_up", kTabView, {L"Windshield view: height (m)", L"Vue pare-brise : hauteur (m)"},
     Kind::Float, 0.15f, -1, 2, 0.05f, kNone,
     {L"View in front of the windshield: height above the driver's head.",
      L"Vue devant le pare-brise : hauteur au-dessus de la tête du conducteur."}},

    // --- Contrôles ---
    {L"Controls", L"layout", kTabControls, {L"Button layout", L"Disposition des boutons"}, Kind::Choice, 0, 0, 2, 0,
     {L"Automatic|Steam Frame|Quest 2 (Touch)", L"Automatique|Steam Frame|Quest 2 (Touch)"},
     {L"Automatic: Steam Frame layout with its controllers, Quest 2 layout with the others. Quest 2 also works on "
      L"Steam Frame controllers (A, B, X, Y only). Applied at once, even while playing.",
      L"Automatique : disposition Steam Frame avec ses manettes, Quest 2 avec les autres. Quest 2 marche aussi sur "
      L"les manettes du Steam Frame (A, B, X, Y seulement). Appliqué tout de suite, même en cours de partie."}},
    {L"Hud", L"prompts", kTabControls, {L"Game messages show", L"Les messages du jeu montrent"}, Kind::Choice, 1, 0, 2, 0,
     {L"Keyboard keys|Controller buttons|Controller buttons (pad drawings, trial)",
      L"Les touches du clavier|Les boutons des manettes|Les boutons (dessins de manette, essai)"},
     {L"\"Press ... to enter\": the game names the VR controller button for the chosen layout instead of the keyboard key.",
      L"« Appuyez sur ... pour entrer » : le jeu nomme le bouton de la manette VR, selon la disposition choisie, au lieu "
      L"de la touche du clavier."}},
    {L"Controls", L"turn_mode", kTabControls, {L"Turning with the right stick", L"Rotation au stick droit"},
     Kind::Choice, 0, 0, 1, 0, {L"Smooth|Snap", L"Continue|Par crans"},
     {L"Smooth: the view turns while the stick is pushed. Snap: one step per push, more comfortable.",
      L"Continue : la vue tourne tant que le stick est poussé. Par crans : un cran par poussée, plus confortable."}},
    {L"Controls", L"snap_angle", kTabControls, {L"Snap turning: step (degrees)", L"Rotation par crans : angle (degrés)"},
     Kind::Float, 30, 15, 90, 5, kNone,
     {L"Angle of one step in snap turning.", L"Angle d'un cran en rotation par crans."}},
    {L"Controls", L"turn_speed", kTabControls, {L"Smooth turning: speed", L"Rotation continue : vitesse"}, Kind::Float,
     1200, 100, 5000, 50, kNone,
     {L"Speed with the stick fully pushed, in mouse pixels per second (it also depends on the mouse sensitivity "
      L"set in the game).",
      L"Vitesse stick à fond, en pixels de souris par seconde (elle dépend aussi de la sensibilité de la souris "
      L"réglée dans le jeu)."}},
    {L"Controls", L"crouch_hold", kTabControls, {L"Crouch while the stick is held down", L"Accroupi tant que le stick est tenu en bas"},
     Kind::Bool, 1, 0, 1, 0, kNone,
     {L"On: the character crouches while the right stick is held down and stands up when it is released. Off: "
      L"pushing the stick down toggles crouching, as in the game.",
      L"Activé : le personnage s'accroupit tant que le stick droit est tenu vers le bas et se relève quand il est "
      L"relâché. Désactivé : pousser le stick vers le bas bascule l'accroupissement, comme dans le jeu."}},
    {L"Controls", L"wheel_toggle", kTabControls, {L"Weapon wheel: one click opens it", L"Roue des armes : un clic l'ouvre"},
     Kind::Bool, 1, 0, 1, 0, kNone,
     {L"On: click the right stick, choose with the stick, release it to take the weapon. Off: hold the click "
      L"while choosing.",
      L"Activé : clic du stick droit, choix au stick, le relâcher prend l'arme. Désactivé : tenir le clic pendant "
      L"le choix."}},

    // --- Mains et visée ---
    {L"Hands", L"aim", kTabHands, {L"Aim with the controllers", L"Viser avec les manettes"}, Kind::Bool, 1, 0, 1, 0,
     kNone,
     {L"Shots leave from the controller and go where it points; with a gun in each hand, each trigger fires its "
      L"own. Off: shots go to the centre of the view.",
      L"Le tir part de la manette et va où elle pointe ; avec une arme dans chaque main, chaque gâchette tire avec "
      L"la sienne. Désactivé : le tir va au centre du regard."}},
    {L"Hands", L"laser", kTabHands, {L"Aiming mark", L"Repère de visée"}, Kind::Choice, 2, 0, 3, 0,
     {L"None (game crosshair)|Beam and dot|Dot only|Drawn in the game image (old)",
      L"Aucun (réticule du jeu)|Trait et point|Point seul|Dessin dans l'image du jeu (ancien)"},
     {L"Red mark of each controller. Dot only: a dot where the shot will land, without the beam.",
      L"Repère rouge de chaque manette. Point seul : un point à l'endroit où le tir arrivera, sans le trait."}},
    {L"Hands", L"laser_length", kTabHands, {L"Beam length (m)", L"Longueur du trait (m)"}, Kind::Float, 3, 0.5f, 10,
     0.5f, kNone,
     {L"Length of the beam from the controller (it fades towards its end). Used by \"Beam and dot\".",
      L"Longueur du trait depuis la manette (il s'efface vers son bout). Sert au choix « Trait et point »."}},
    {L"Hands", L"arms", kTabHands, {L"Arms follow the controllers", L"Les bras suivent les manettes"}, Kind::Bool, 1,
     0, 1, 0, kNone,
     {L"First person, on foot: the character's arms and weapons follow your hands. Off: the game's animation.",
      L"Première personne, à pied : les bras et les armes du personnage suivent vos mains. Désactivé : animation "
      L"du jeu."}},
    {L"Hands", L"torso", kTabHands, {L"Torso follows the view (max. degrees)", L"Le buste suit le regard (degrés max.)"},
     Kind::Float, 90, 0, 180, 5, kNone,
     {L"The character's torso turns towards where you look, up to this angle. 0: the game's animation.",
      L"Le buste du personnage se tourne vers le regard, jusqu'à cet angle. 0 : buste de l'animation du jeu."}},

    {L"Melee", L"enabled", kTabHands, {L"Hit with your hands (fists, melee weapons)", L"Coups au geste (poings, armes blanches)"},
     Kind::Bool, 1, 0, 1, 0, kNone,
     {L"A fast move of a fist, or of the melee weapon held in the right hand, hits what it goes through. The "
      L"trigger still starts the game's animated attacks.",
      L"Un geste rapide du poing, ou de l'arme blanche tenue dans la main droite, frappe ce qu'il traverse. La "
      L"gâchette lance toujours les attaques animées du jeu."}},
    {L"Melee", L"speed", kTabHands, {L"Hits: hand speed needed (m/s)", L"Coups : vitesse de main nécessaire (m/s)"},
     Kind::Float, 1.8f, 0.8f, 4.0f, 0.1f, kNone,
     {L"Speed of the hand from which a move counts as a hit. Lower: hits land more easily, also by accident.",
      L"Vitesse de la main à partir de laquelle un geste est un coup. Plus bas : les coups partent plus facilement, "
      L"y compris sans le vouloir."}},
    {L"Melee", L"gun_bash", kTabHands, {L"Gun bash with a fast move", L"Coup de crosse au geste"}, Kind::Bool, 1, 0, 1, 0,
     kNone,
     {L"A fast move of a hand holding a firearm hits with it (it needs a slightly faster move than a punch).",
      L"Un geste rapide d'une main qui tient une arme à feu donne un coup de crosse (il demande un geste un peu "
      L"plus rapide qu'un coup de poing)."}},
    {L"Melee", L"weapon_tilt", kTabHands, {L"Melee weapon: tilt (degrees)", L"Arme blanche : inclinaison (degrés)"},
     Kind::Float, 0, -60, 60, 5, kNone,
     {L"Tilts the weapon held in the hand. Positive: the blade leans forward, towards where the controller points.",
      L"Incline l'arme tenue dans la main. Positif : la lame se couche vers l'avant, vers où pointe la manette."}},

    // --- Avancé ---
    {L"Camera", L"head_tracking", kTabAdvanced, {L"Head tracking", L"Suivi de la tête"}, Kind::Bool, 1, 0, 1, 0, kNone,
     {L"The camera follows the headset. Off: diagnostic only.",
      L"La caméra suit le casque. Désactivé : pour un diagnostic seulement."}},
    {L"Display", L"fov_scale", kTabAdvanced, {L"Announced field of view (correction)", L"Champ de vue annoncé (correction)"},
     Kind::Float, 1.0f, 0.5f, 1.5f, 0.01f, kNone,
     {L"Correction of the field of view announced to the headset. Leave at 1 unless the world seems to stretch "
      L"when you turn your head.",
      L"Correction du champ de vue annoncé au casque. Laisser à 1, sauf si le monde semble s'étirer quand la tête "
      L"tourne."}},
    {L"Stereo", L"swap_eyes", kTabAdvanced, {L"Swap the eyes", L"Inverser les deux yeux"}, Kind::Bool, 0, 0, 1, 0,
     kNone, {L"If depth looks inside out.", L"Si le relief paraît à l'envers."}},
    {L"Controls", L"fire_mode", kTabAdvanced, {L"How triggers fire", L"Tir aux gâchettes : méthode"}, Kind::Choice, 0,
     0, 2, 0, {L"Direct (recommended)|Mouse buttons|Virtual gamepad", L"Directe (conseillé)|Boutons de la souris|Manette virtuelle"},
     {L"Direct: the command is placed straight into the game. The other two are fallbacks; the virtual gamepad "
      L"turns Juiced's aim assist back on.",
      L"Directe : la commande est posée dans le jeu. Les deux autres sont des solutions de repli ; la manette "
      L"virtuelle réactive l'aide à la visée de Juiced."}},
    {L"Controls", L"wheel_button", kTabAdvanced, {L"Weapon wheel: gamepad button", L"Roue des armes : bouton de manette"},
     Kind::Choice, 1, 0, 3, 0, {L"A|B|X|Y", L"A|B|X|Y"},
     {L"Button of the game's gamepad layout that opens the weapon wheel (B by default in the game).",
      L"Bouton de la manette du jeu qui ouvre la roue des armes (B par défaut dans le jeu)."}},
};
constexpr int kCount = static_cast<int>(std::size(kDefs));

float g_values[kCount];
std::wstring g_texts[kCount];
bool g_present[kCount];  // clé trouvée dans le fichier
bool g_modified[kCount]; // changée dans l'outil

float Clamp(const Definition& d, float v)
{
    if (d.kind == Kind::Bool)
        return v != 0 ? 1.0f : 0.0f;
    if (d.kind == Kind::Choice)
        v = static_cast<float>(static_cast<int>(v + 0.5f));
    return std::clamp(v, d.minValue, d.maxValue);
}

struct Defaults {
    Defaults()
    {
        for (int i = 0; i < kCount; ++i)
            g_values[i] = kDefs[i].defaultValue;
    }
} g_defaults;

} // namespace

int Count()
{
    return kCount;
}

const Definition& Def(int index)
{
    return kDefs[index];
}

float Get(int index)
{
    return g_values[index];
}

void Set(int index, float value)
{
    value = Clamp(kDefs[index], value);
    if (value != g_values[index] || !g_present[index])
        g_modified[index] = true;
    g_values[index] = value;
}

std::wstring GetText(int index)
{
    return g_texts[index];
}

void SetText(int index, const std::wstring& value)
{
    if (value != g_texts[index])
        g_modified[index] = true;
    g_texts[index] = value;
}

void Reset(Tab tab)
{
    for (int i = 0; i < kCount; ++i)
        if (kDefs[i].kind != Kind::Text && (tab == kTabCount || kDefs[i].tab == tab))
            Set(i, kDefs[i].defaultValue);
}

bool Modified()
{
    return std::any_of(std::begin(g_modified), std::end(g_modified), [](bool m) { return m; });
}

void Load(const std::wstring& path)
{
    for (int i = 0; i < kCount; ++i) {
        const Definition& d = kDefs[i];
        wchar_t text[1024]{};
        GetPrivateProfileStringW(d.section, d.key, L"", text, static_cast<DWORD>(std::size(text)), path.c_str());
        g_modified[i] = false;
        g_present[i] = text[0] != 0;
        if (d.kind == Kind::Text) {
            g_texts[i] = text;
            continue;
        }
        wchar_t* end = nullptr;
        const double value = wcstod(text, &end);
        // Valeur du fichier gardée telle quelle, même hors des bornes de l'outil (réglée à la main)
        if (end != text)
            g_values[i] = d.kind == Kind::Float ? static_cast<float>(value) : Clamp(d, static_cast<float>(value));
        else
            g_values[i] = d.defaultValue, g_present[i] = false;
    }
}

bool Save(const std::wstring& path)
{
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        // Nouveau fichier : en-tête, puis toutes les clés
        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"w") != 0 || !f)
            return false;
        fputs("; SR2VR - reglages, relus chaque seconde pendant que le jeu tourne.\n"
              "; Se modifie avec sr2_vr_config.exe (ou a la main).\n",
              f);
        fclose(f);
    }
    bool ok = true;
    for (int i = 0; i < kCount; ++i) {
        const Definition& d = kDefs[i];
        // Dossier du jeu vide et absent : rien à écrire (trouvé automatiquement)
        if (!g_modified[i] && (g_present[i] || (d.kind == Kind::Text && g_texts[i].empty())))
            continue;
        wchar_t text[64];
        if (d.kind != Kind::Text)
            swprintf_s(text, L"%g", g_values[i]);
        if (WritePrivateProfileStringW(d.section, d.key, d.kind == Kind::Text ? g_texts[i].c_str() : text, path.c_str())) {
            g_modified[i] = false;
            g_present[i] = true;
        } else {
            ok = false;
        }
    }
    return ok;
}

} // namespace settings
