# À reprendre en local (fichier temporaire, à supprimer une fois fusionné)

Travail fait à distance le 2026-11-04, sur trois branches **empilées** : chacune part de la
précédente, donc la dernière contient tout.

```
dev
 └─ claude/defauts-readme-ci   (0.25.1)
     └─ claude/optimisation    (0.26.0)
         └─ claude/capture-asynchrone   (0.27.0)  ← ce fichier
```

Les trois compilent avec MSVC (Win32, vcpkg) et leurs tests passent sur GitHub Actions
(onglet Actions, build « sr2vr-release » à télécharger). **Rien n'a été essayé en jeu ni au
casque.**

## Récupérer en local

```bat
git fetch origin
git checkout claude/capture-asynchrone
build.bat
```

Pour essayer une seule étape : `git checkout claude/defauts-readme-ci` ou
`claude/optimisation`, puis `build.bat`.

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
- **Réglage** `[Display] async_capture` (onglet Avancé du lanceur) : 1 par défaut, 0 =
  ancienne méthode. Changeable pendant que le jeu tourne.
- **`src/render/d3d9_hook.cpp`** : les surfaces de la capture sont libérées avant `Reset` /
  `ResetEx` (sinon le Reset échoue).
- **`tools/tests/capture_test.cpp`** : vrai device D3D9, vérifie pixels ↔ vue, paires, Reset,
  changement de mode. Il a trouvé un vrai bug (œil gauche rangé avec le mauvais numéro de
  paire), corrigé.
- Sous Wine (rendu logiciel, 8 ms de travail simulé) : 1,2 ms → 0,26 ms pris au jeu par
  capture, 1 Present de retard, aucune copie attendue. À confirmer sur une vraie carte.

## À vérifier au casque

1. **Lancement sans lanceur** (`play.bat --no-launcher`, `sr2_vr.ini` renommé) : vrai relief,
   vue à la première personne et bras dès le départ.
2. **Saccade** : celle d'une fois par seconde a-t-elle disparu ? Un réglage changé dans le
   lanceur s'applique-t-il toujours en jeu, en une seconde environ ?
3. **Capture asynchrone**, dans `build\Release\sr2_vr.log` :
   - ligne `capture asynchrone : N images, publiees X Present apres leur copie en moyenne,
     Y attendue(s)`. Bon signe : X proche de 1, Y faible.
   - comparer avec `async_capture=0` : images par seconde (lignes `casque :` et
     `images :`), temps de capture, ressenti de la latence en tournant la tête.
   - changer de résolution ou d'affichage en jeu (Reset) : pas de plantage, la capture repart.
4. **Rien de cassé** : menus, boutiques, cinématiques, roue des armes, tir, corps à corps.

## Ensuite

- Si tout va bien : fusionner dans l'ordre (`claude/defauts-readme-ci`, puis
  `claude/optimisation`, puis `claude/capture-asynchrone`) dans `dev`, ou directement la
  dernière, qui contient tout. Puis supprimer ce fichier.
- Si la capture asynchrone pose problème : `async_capture=0` dans le lanceur, ou garder
  seulement `claude/optimisation`.
- Si la latence gêne : réduire `kMaxAgePresents` (2 → 1) dans `frame_capture.cpp`, ou en
  faire un réglage.

## Tests en local

```bat
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
python tools\check_settings.py -v
```
