# SR2VR

Mod de réalité virtuelle pour **Saints Row 2** (PC, version Steam avec
Juiced Patch) : affichage en vrai relief dans un casque
OpenXR, tête suivie par la caméra du jeu, vue aux yeux du personnage, visée, tir et coups
avec les manettes VR.

Le mod ne modifie aucun fichier du jeu : un injecteur lance `SR2_pc.exe` et y charge
`sr2_vr.dll`, qui pose ses détours en mémoire (MinHook) et ouvre sa propre session OpenXR.

Version actuelle : voir `project(SR2VR VERSION …)` dans `CMakeLists.txt`.

## Ce que fait le mod

- **Affichage** (`[Display] mode`) : écran virtuel, immersif sans relief, relief un œil par
  image, ou **vrai relief** (les deux yeux rendus pour chaque image du jeu, conseillé).
- **Caméra** : orientation du casque composée avec celle du jeu ; rendu, son et visée suivent
  le regard. Vue à la première personne (yeux du personnage), à pied comme en véhicule.
- **Écran virtuel automatique** pour le menu de pause, les cinématiques, les boutiques, la
  garde-robe et le garage.
- **Interface** ramenée vers le centre de l'image et posée à quelques mètres en relief.
- **Mains** : un rayon de visée par manette, armes et bras posés sur les manettes, tir à deux
  armes (une gâchette par main), point d'impact dessiné par le casque.
- **Corps à corps au geste** : poings, armes blanches et coup de crosse.
- **Commandes** complètes à pied (déplacement, rotation continue ou par crans, saut,
  accroupissement, roue des armes, actions du jeu), menus pilotés comme une manette Xbox,
  boutons des manettes VR affichés dans les messages du jeu.
- **Dispositions** Steam Frame et Quest 2 (`[Controls] layout`, automatique par défaut).

## Prérequis

- Saints Row 2 (Steam) avec **Juiced Patch** installé (le mod s'appuie sur ses détours et sur
  sa SDL3 pour la manette virtuelle).
- Un runtime OpenXR **32 bits** : SteamVR (Steam Frame, Index, Quest par Link / Steam Link /
  Virtual Desktop). Le runtime 32 bits de Meta plante à la création de la session :
  l'injecteur choisit alors SteamVR automatiquement.
- Pour compiler : Visual Studio (MSVC, outils C++), CMake 3.20 ou plus, et
  [vcpkg](https://github.com/microsoft/vcpkg) (dépendances `minhook` et `openxr-loader`,
  liées statiquement, triplet `x86-windows-static`).

## Compiler et jouer

```bat
build.bat          :: compile en Release 32 bits, détail dans build\build.log
play.bat           :: compile, ouvre le lanceur puis le jeu avec le mod
play_steamvr.bat   :: idem en forçant le runtime OpenXR de SteamVR
config.bat         :: ouvre seulement les réglages (un jeu en cours les applique en 1 s)
probe.bat          :: banc d'essai OpenXR hors du jeu (tools/xr_probe)
```

`build.bat` cherche vcpkg dans la variable `SR2VR_VCPKG`. À la main :

```bat
cmake -S . -B build -A Win32 -DCMAKE_TOOLCHAIN_FILE=<vcpkg>\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Release
```

Résultat dans `build\Release\` : `sr2_vr.dll`, `sr2_vr_injector.exe`, `sr2_vr_config.exe`
et `xr_probe.exe`. Le jeu est trouvé dans les bibliothèques Steam (ou `[Launch] game_dir`,
ou `--game-dir`).

### Options de l'injecteur

```
sr2_vr_injector [--game-dir <dossier>] [--dll <chemin>] [--early]
                [--steamvr | --runtime <json> | --default-runtime]
                [--launcher | --no-launcher] [--wait] [--timeout <s>] [-- <arguments du jeu>]
```

Le détail de chaque option est en tête de `injector/main.cpp`.

## Réglages

Tout se règle dans le lanceur (`sr2_vr_config.exe`, français ou anglais), qui écrit
`sr2_vr.ini` à côté de la DLL. La DLL relit ce fichier une fois par seconde : la plupart des
réglages s'appliquent pendant que le jeu tourne. Une clé absente prend la valeur par défaut,
la même dans la DLL et dans le lanceur (vérifié par `tools/check_settings.py`).

Sections principales : `[Display]`, `[Camera]`, `[Stereo]`, `[Hud]`, `[FirstPerson]`,
`[Hands]`, `[Melee]`, `[Controls]`, `[Launch]`. La table complète (libellés, bornes,
valeurs par défaut, aide) est dans `config_tool/settings.cpp`.

## Journaux

- `build\Release\sr2_vr.log` : processus, modules chargés, hooks, session OpenXR, mesures
  par seconde (images, paires d'yeux, capture).
- `build\Release\sr2_vr_injector.log` : lancement et injection.
- `build\Release\xr_probe*.log` : banc d'essai.

## Organisation du code

| Dossier | Rôle |
| --- | --- |
| `src/dllmain.cpp` | Point d'entrée de la DLL : description du processus, installation des hooks |
| `src/render/` | Détours Direct3D 9 (Present, Reset…), relecture de l'image du jeu, repère de visée |
| `src/xr/` | Session OpenXR (device D3D11 dédié, calques, swapchains), actions des manettes, laser |
| `src/engine/` | Détours du moteur du jeu, trouvés par signature : caméra, joueur, mains, visée, corps à corps, commandes, interface, messages |
| `src/input/` | Clavier (SendInput), manette virtuelle SDL, actions de haut niveau |
| `src/util/` | Journal, hooks MinHook, recherche de signatures, lecture de `sr2_vr.ini` |
| `injector/` | Lance le jeu et injecte la DLL, choix du runtime OpenXR |
| `config_tool/` | Lanceur et fenêtre de réglages (Win32 / GDI) |
| `tools/xr_probe/` | Faux jeu D3D9 32 bits pour essayer la capture et OpenXR hors du jeu |
| `tools/re/` | Scripts Python (capstone) d'analyse statique de `SR2_pc.exe`, voir leur README |
| `tools/check_settings.py` | Vérifie que DLL, injecteur et lanceur ont les mêmes valeurs par défaut |
| `tools/tests/` | Tests lancés par `ctest -C Release` : lecture de `sr2_vr.ini` comparée à l'API Windows, capture d'image synchrone et asynchrone sur un vrai device Direct3D 9 |

## Vérifications

```bash
python3 tools/check_settings.py -v
```

Une compilation Windows (MSVC, Win32, vcpkg) et ce contrôle tournent sur GitHub Actions à
chaque envoi (`.github/workflows/build.yml`) ; les fichiers compilés sont joints au résultat.
