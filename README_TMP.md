# À reprendre en local (fichier temporaire, à supprimer une fois fusionné)

Travail fait sans le casque, du 4 au 10 novembre 2026, sur six branches **empilées** : chacune
part de la précédente, donc la dernière contient tout.

```
dev
 └─ claude/defauts-readme-ci   (0.25.1)
     └─ claude/optimisation    (0.26.0)
         └─ claude/capture-asynchrone   (0.27.0)
             └─ claude/tir-deux-pistolets   (0.27.1)
                 └─ claude/cadence-et-mesures   (0.28.0, 0.29.0)
                     └─ claude/reconnexion-casque   (0.30.0)  ← ce fichier
```

Tout compile avec MSVC (Win32, vcpkg) et les tests automatiques passent sur le PC
(`test.bat`, carte RTX 3060 Ti). **Rien n'a été essayé en jeu ni au casque.**

État du projet, mesures et pistes : `docs/ETAT_ET_PISTES.md`.

## Récupérer en local

```bat
git fetch origin
git checkout claude/reconnexion-casque
build.bat
```

Pour essayer une seule étape : `git checkout <branche>`, puis `build.bat`.

## 1. `claude/defauts-readme-ci` (0.25.1)

- **Correction** : cinq valeurs par défaut de la DLL ne correspondaient pas à celles du
  lanceur. Sans `sr2_vr.ini` (`--no-launcher`, arguments du jeu, fichier supprimé), le jeu
  démarrait sur l'écran virtuel, sans vue à la première personne ni bras.
  | Réglage | Avant (DLL) | Maintenant (= lanceur) |
  | --- | --- | --- |
  | `[Display] mode` | 0 | 3 |
  | `[FirstPerson] enabled` | 0 | 1 |
  | `[Hands] arms` | 0 | 1 |
  | `[Camera] hfov` | 0 | 125 |
  | `[Hud] distance` | 3 | 5 |
- `tools/check_settings.py` : compare les valeurs par défaut de la DLL et de l'injecteur avec
  la table du lanceur (`python tools/check_settings.py -v`). À relancer après chaque nouveau
  réglage.
- `README.md` : le dépôt n'en avait pas.
- `.github/workflows/build.yml` : compilation Windows et tests à chaque envoi.

## 2. `claude/optimisation` (0.26.0)

- **`src/util/config.cpp`** : `sr2_vr.ini` est gardé en mémoire et relu seulement quand il
  change (vérifié au plus toutes les 250 ms). Avant, chaque seconde, le thread du jeu
  ouvrait le fichier une quarantaine de fois (saccade possible une fois par seconde). Sous
  Wine : ~5 ms → ~5 µs. Lecture identique à `GetPrivateProfileString` (test
  `tools/tests/config_test.cpp`).
- **`src/util/log.cpp`** : le log reste ouvert pendant toute la session (une ligne = un
  `WriteFile`). Sous Wine : ~110 µs → ~2 µs par ligne. Les lignes restent écrites si le jeu
  plante.
- **`CMakeLists.txt`** : optimisation Release `/GL` + `/LTCG`, `/Oi`, `/Gy /Gw`,
  `/OPT:REF /OPT:ICF`. Volontairement pas de `/fp:fast` (casse les tests de NaN) ni d'AVX2.

## 3. `claude/capture-asynchrone` (0.27.0)

- **`src/render/frame_capture.cpp`** : au Present, l'image est recopiée par le GPU
  (`StretchRect`), le transfert vers la mémoire demandé tout de suite (`GetRenderTargetData`)
  et un événement GPU posé derrière. L'image est lue à un Present suivant, sans attendre le
  GPU, et publiée avec sa vue d'origine. Copie attendue si elle a plus de 2 Present de
  retard. Paires d'yeux toujours complètes et dans l'ordre. Gère aussi un back buffer
  multisamplé.
- **Réglage** `[Display] async_capture` (onglet Avancé du lanceur), changeable pendant que
  le jeu tourne. Par défaut à 1 dans cette branche, **remis à 0 par la branche 5**.
- **`src/render/d3d9_hook.cpp`** : les surfaces de la capture sont libérées avant `Reset` /
  `ResetEx` (sinon le Reset échoue).
- **`tools/tests/capture_test.cpp`** : vrai device D3D9, vérifie pixels ↔ vue, paires, Reset,
  changement de mode. Il a trouvé un vrai bug (œil gauche rangé avec le mauvais numéro de
  paire), corrigé.
- Sous Wine (rendu logiciel, 8 ms de travail simulé) : 1,2 ms → 0,26 ms pris au jeu par
  capture, 1 Present de retard. **Non confirmé sur la vraie carte** : l'attente passe dans le
  Present suivant (voir la branche 5).

## 4. `claude/tir-deux-pistolets` (0.27.1)

- **`src/engine/controls.cpp`** : avec deux armes, une pression sur une gâchette pendant que
  l'autre est tenue est mise en file et tirée à l'image suivante (la commande de tir du jeu
  est relâchée une image pour repartir). Ligne « tir : » du journal : nombre de pressions
  passées par la file.

## 5. `claude/cadence-et-mesures` (0.28.0, 0.29.0)

- **`src/engine/pacing.cpp`**, `[Display] frame_cap` (100 par défaut, 0 = comme avant) : le
  jeu dort après chaque image dès qu'il dépasse 35 images par seconde ; ce frein devient un
  simple plafond. Estimation : 9 ms de sommeil en moins par paire d'images.
- **`[Display] async_capture`** devient un choix : 0 par le jeu tout de suite (**de nouveau
  par défaut** : la mesure sur la vraie carte ne donne aucun gain au mode 1), 1 par le jeu une
  image plus tard, 2 par un autre thread (+5 à +16 % d'images dans `bench.bat`).
- `[Debug] d3d_stats` (0 par défaut) : détours de comptage retirés du chemin normal.
- Journal : lignes « cadence : » et « temps par image de simulation ».
- `test.bat` et `bench.bat` : tests et mesures d'un double-clic.

## 6. `claude/reconnexion-casque` (0.30.0)

- **`src/xr/xr_session.cpp`**, `[Display] reconnect` (0 par défaut) : quand la session se
  termine toute seule (liaison coupée, SteamVR fermé), nouvelle tentative pendant dix minutes.
- Dans tous les cas, une session perdue se termine proprement au lieu de remplir le journal.

## À vérifier au casque, dans cet ordre

1. **Rien de cassé** avec les réglages par défaut : menus, boutiques, cinématiques, roue des
   armes, tir, corps à corps. Lancement sans lanceur (`play.bat --no-launcher`) : vrai relief
   et vue à la première personne dès le départ.
2. **Frein levé** (`frame_cap`, onglet Avancé) : jouer deux minutes à 100, deux minutes à 0.
   Dans `build\Release\sr2_vr.log`, comparer « images du jeu par seconde » (ligne
   `casque :`) et lire la ligne `cadence :`. Surveiller la physique (véhicules, chutes).
3. **Lecture par un autre thread** (`Relecture de l'image`, onglet Avancé) : passer de 0 à 2
   en jeu, comparer les mêmes lignes et la ligne « thread de lecture ». Changer de résolution
   en jeu : pas de plantage.
4. **Deux pistolets** : tenir une gâchette, appuyer sur l'autre ; chaque pression doit tirer.
5. **Reconnexion** (`Reconnexion du casque`, onglet Avancé) : couper la liaison du casque
   trente secondes en jeu, la rétablir ; l'image doit revenir sans relancer le jeu.
6. **Saccade** d'une fois par seconde (0.26.0) : disparue ?

## Ensuite

- Si tout va bien : fusionner la dernière branche dans `dev` (elle contient tout), puis
  supprimer ce fichier.
- Si une étape pose problème : son réglage la coupe (`frame_cap=0`, `async_capture=0`,
  `reconnect=0`), ou repartir de la branche précédente.

## Tests en local

```bat
test.bat      :: tests automatiques, résultat dans build\test.log
bench.bat     :: mesures de la capture, résultat dans build\bench.log
python tools\check_settings.py -v
```
