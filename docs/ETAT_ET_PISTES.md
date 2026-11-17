# État du projet et pistes

Notes de réflexion du 10 novembre 2026, écrites pendant que le casque n'était pas disponible.
Rien de ce qui est marqué « non essayé » n'a été vu au casque.

## 1. Où en est le projet

| État | Quoi | Branche |
| --- | --- | --- |
| Validé au casque | Dispositions Steam Frame / Quest 2 (0.23.0) | `main` |
| Validé au casque | Boutons des manettes dans les messages (0.24.1), menus pilotés comme une manette (0.25.0) | `dev` |
| Compilé, tests automatiques verts, **non essayé en jeu** | Valeurs par défaut alignées, README, CI (0.25.1) | `claude/defauts-readme-ci` |
| idem | Réglages en cache, journal gardé ouvert, LTCG (0.26.0) | `claude/optimisation` |
| idem | Capture asynchrone (0.27.0) | `claude/capture-asynchrone` |
| idem | Deux pistolets : chaque gâchette tire même si l'autre est tenue (0.27.1) | `claude/tir-deux-pistolets` |
| idem | Frein d'images du jeu remplacé par un plafond, mesures (0.28.0) ; lecture de l'image par un autre thread (0.29.0) | `claude/cadence-et-mesures` |
| idem | Reconnexion du casque quand la liaison tombe (0.30.0) | `claude/reconnexion-casque` |
| Essais sans casque verts (casque simulé), **non essayé au casque réel** | Casque simulé, script d'entrées, captures des deux yeux, `smoke.bat`, `mesures.bat` (0.31.0) | `claude/tests-sans-casque` |
| idem | Interface du jeu sur un calque montré aux deux yeux (0.32.0) | `claude/interface-panneau` |
| Abandonné (plantages, œil droit blanc) | Interface recopiée dans l'œil droit (0.26.1 à 0.26.3) | `essai/interface-deux-yeux`, sur le PC seulement |

Les branches `claude/…` sont empilées dans l'ordre du tableau : la dernière contient tout.

Problèmes ouverts, signalés au casque :

1. **L'interface en jeu (mini-carte, vie, roue des armes) n'apparaît que dans l'œil gauche** en
   vrai relief. Traité le 17 novembre (section 5), à confirmer au casque.
2. **Le cadre de la mini-carte était plus petit que la carte** : les jauges de notoriété
   (police, gangs) suivaient `[Hud]`, pas la carte, dessinée par un autre code du jeu. Traité
   le 17 novembre (section 5, cadre agrandi), à confirmer au casque.
3. **SteamVR se ferme** quand la liaison sans fil du Steam Frame tombe (il quitte de lui-même
   après 30 s sans casque) ; le jeu restait alors sur l'écran jusqu'à être relancé. La
   reconnexion (0.30.0) vise ce second point ; la coupure elle-même est côté réseau.

## 2. Où part le temps d'une image

En vrai relief le jeu prépare et dessine deux images par instant du jeu (œil gauche puis œil
droit). Mesuré en jeu sur la machine de mise au point (RTX 3060 Ti, 1920x1080) : environ
**26 paires par seconde, soit 38 ms par paire**. Le casque, lui, affiche bien plus vite et
recale l'image sur la tête entre deux paires ; ce sont les objets qui bougent et les
déplacements qui paraissent saccadés.

Ce que j'ai pu chiffrer dans ces 38 ms :

| Poste | Par paire | Comment je le sais |
| --- | --- | --- |
| Frein du jeu : il dort après chaque image dès qu'il dépasse 35 images par seconde | ≈ 9 ms | Lecture du code du jeu (0x00D20D5F) : `Sleep((28,57 ms − durée de l'image) / 2)` |
| Relecture des deux images pour le casque | 7 à 9 ms | `bench.bat` sur la carte : 3,5 à 4,4 ms par image |
| Le jeu lui-même (simulation, préparation, dessin) | le reste, ≈ 20 ms | Par différence ; les lignes « temps par image de simulation » du journal (0.28.0) le diront |

Mesures de `bench.bat` du 10 novembre (le vrai code de capture, 100 paires, 3000 appels de
dessin et 12 ms de calcul par image) :

| `async_capture` | Le rendu ne s'arrête jamais | Le rendu attend la simulation 8 ms par paire |
| --- | --- | --- |
| 0, par le jeu tout de suite | 33,3 ms par paire | 42,3 ms |
| 1, par le jeu une image plus tard | 34,2 ms | 42,3 ms |
| 2, par un autre thread | 31,8 ms (+5 % d'images) | 36,5 ms (+16 %) |

Lecture de ces chiffres :

- Le mode 1 (branche 0.27.0, qui l'avait mis par défaut) **ne gagne rien** sur cette carte :
  l'attente quitte la capture pour se retrouver dans le Present suivant, et l'image arrive une
  image plus tard. Je l'ai donc remis à 0 par défaut.
- Le mode 2 gagne d'autant plus que le thread du rendu a des temps morts (quand c'est la
  simulation qui limite la cadence, ce que je crois être le cas en jeu). Direct3D ne laisse
  passer qu'un thread à la fois : pendant que le thread de lecture relit, le jeu ne peut pas
  dessiner.
- Une relecture coûte environ 2 ms quelle que soit la taille de l'image (un quart d'image :
  2,0 ms ; l'image entière : 2,0 ms ; 16 bits : 2,1 ms). **Baisser la résolution, rogner
  l'image ou passer en 16 bits ne ferait donc rien gagner sur ce poste.**

## 3. Ce qui a été fait sur `claude/cadence-et-mesures` et `claude/reconnexion-casque`

- `[Display] frame_cap` (100 par défaut, 0 = comportement du jeu) : l'appel `Sleep` du frein
  est détourné vers un simple plafond. C'est ce que fait l'option `UncapFPS` de Juiced, sans
  toucher à son fichier. Gain attendu : jusqu'à 9 ms par paire, soit 26 → 30 à 34 paires par
  seconde si rien d'autre ne retient le jeu. Risque : un jeu de 2008 peut mal se comporter
  au-dessus de sa cadence prévue (physique, scripts) ; en vrai relief on reste loin de 60
  images par instant du jeu.
- `[Display] async_capture` à trois valeurs (0 par défaut), voir section 2. Le mode 2 est
  vérifié par `capture_test` sur la vraie carte (pixels, paires, Reset, changement de mode)
  mais pas en jeu.
- `[Debug] d3d_stats` (0 par défaut) : les détours de comptage (BeginScene, Clear, etc.) ne
  sont plus posés ; restent Present et Reset.
- Journal : toutes les 10 s, « temps par image de simulation : X ms de simulation, Y ms à
  préparer l'œil gauche, Z ms pour l'œil droit » et « cadence : le frein du jeu aurait dormi
  X ms par image ». Ces deux lignes disent où est le goulot et valent mieux que mes
  estimations.
- `test.bat` (tests automatiques, résultat dans `build\test.log`) et `bench.bat` (mesures,
  `build\bench.log`, une quarantaine de secondes, carte graphique peu sollicitée).
- `[Display] reconnect` (0 par défaut) : après une session perdue, nouvelle tentative pendant
  dix minutes. Effet de bord possible : SteamVR relancé tant que le casque n'est pas revenu.

## 4. Pistes, de la plus rentable à la moins sûre

### Fluidité

1. **Essayer au casque ce qui est déjà écrit**, dans cet ordre : `frame_cap` (100 contre 0),
   puis `async_capture=2`. Dix minutes de jeu et les lignes du journal suffisent à trancher.
2. **Ne plus rien relire du tout** : faire dessiner le jeu dans une texture que le casque lit
   directement. Impossible avec le device Direct3D 9 du jeu tel quel (le journal le confirme :
   « partage GPU impossible »). Deux façons d'y arriver :
   - **DXVK** (Direct3D 9 traduit en Vulkan) : il sait en principe partager une texture avec
     Direct3D 11, et réduit souvent le coût des appels de dessin, donc le poste « le jeu
     lui-même ». Je n'ai vérifié ni l'un ni l'autre sur ce jeu. Demande de poser une
     `d3d9.dll` dans le dossier du jeu : c'est à toi de décider, et à essayer d'abord sans le
     mod. Le mod teste déjà au démarrage si le partage est possible et l'écrit dans le
     journal (ligne « texture partagee D3D9 »).
   - Créer le device en Direct3D 9Ex à la place du jeu : plus intrusif, risque de casser
     Juiced. Je ne le ferais qu'après DXVK.
   Gain : les 7 à 9 ms de relecture, plus la copie vers Direct3D 11.
3. **Mode « un œil par image »** (`[Display] mode=2`, il existe déjà) : deux fois plus
   d'instants du jeu par seconde, relief moins stable. À réessayer une fois le frein levé,
   pour comparer le ressenti.
4. Une seule mise en place de l'interface par paire au lieu de deux : gain minuscule, à faire
   avec la section 5.

Écarté par les mesures : résolution plus basse ou image rognée (la relecture ne dépend
presque pas de la taille), 16 bits, les deux yeux relus d'un seul appel en 32 bits (4,8 ms
contre 2 fois 2,0).

### Confort et fonctions

1. **Interface en panneau flottant** (section 5) : règle les deux défauts d'interface ouverts.
2. **Deux pistolets** (0.27.1) : à essayer, la ligne « tir : » du journal compte les
   pressions mises en file.
3. **Reconnexion du casque** (0.30.0) : à essayer en coupant la liaison volontairement.
4. Fusionner ce qui est validé : six branches empilées, c'est trop. Après un essai concluant,
   tout ramener dans `dev`, puis `dev` dans `main`.
5. Un vrai fichier de version des adresses du jeu : le mod cherche ses points d'ancrage par
   signature, mais quelques adresses fixes restent dans le code (liste de rendu, facteurs de
   la mini-carte). Les regrouper éviterait des surprises avec un autre exécutable.

## 5. Interface en jeu dans les deux yeux (fait le 17 novembre, à voir au casque)

Ce qui avait échoué en octobre : recopier les commandes de dessin de l'interface de l'œil
gauche dans la liste de l'œil droit (deux plantages, un œil blanc).

Ce qui est en place (`render/ui_panel`, `[Hud] panel=1`, version 0.32.0), mis au point sur le
casque simulé, capture par capture :

1. Les appels Direct3D ne partent pas du thread qui exécute la liste des commandes, mais du
   thread de la fenêtre. C'est là que le mod intervient, sur les méthodes du device du jeu
   (SetRenderTarget, SetRenderState, SetTexture et les quatre dessins).
2. Une image du jeu se termine toujours de la même façon : retour au back buffer, deux ou
   trois dessins dont la texture est une cible de rendu de la taille de l'écran (la scène finie
   qu'on recopie), puis, pour l'œil gauche seulement, 150 à 260 dessins : l'interface.
3. À partir du premier de ces dessins, et jusqu'au Present, la cible devient une texture
   transparente à nous. L'image des deux yeux ne contient plus d'interface.
4. Cette texture est relue 12 à 15 fois par seconde (`[Hud] panel_rate`) et montrée par le
   casque **par-dessus les deux yeux**, avec la vue qui a servi à la dessiner : ce qui est
   accroché au décor (le « $3000 » au-dessus d'une liasse) reste à sa place. Elle est décalée
   d'un œil à l'autre pour paraître à `[Hud] distance`.

Détails qui ont compté :

- Le jeu dessine d'abord un voile qui multiplie toute l'image (assombrissement ou teinte),
  dans l'œil gauche seulement. Sur un calque transparent il donnait un fond noir opaque. Il
  n'est plus dessiné du tout : les deux yeux ont maintenant la même image.
- L'alpha du calque suit le mélange choisi par le jeu pour chaque dessin : « par-dessus » pour
  un mélange ordinaire, inchangé pour la lumière ajoutée et pour ce qui est découpé par
  l'alpha déjà en place (la carte ronde).
- Coût mesuré : 46 paires par seconde avec le calque, contre 47 à 48 sans. À 30 lectures par
  seconde il tombait à 35 : chaque lecture coûte 2 à 3 ms au jeu.

Ce que le casque simulé ne dit pas, et qu'il faut regarder au casque réel : le calque est-il
bien superposé à l'image, à la bonne distance, sans bord sombre autour des textes ? La
composition des calques par SteamVR n'est pas visible sans casque ;
`docs/apercu_interface_deux_yeux.png` est un aperçu calculé à partir des captures (œil gauche,
œil droit), une simulation et non ce que le casque affiche.

### Cadre de la mini-carte (0.33.0)

La mini-carte est dessinée par un autre code du jeu, qui ne suit pas l'échelle de l'interface :
elle gardait sa taille pendant que son cadre (le groupe `map_grp` : anneau, jauges de
notoriété) rapetissait avec le reste et disparaissait dessous. `[Hud] map_frame=1` (par
défaut) rend au cadre sa taille d'origine (échelle 1 / échelle de l'interface) ; la carte se
place d'elle-même au milieu du cadre. Comme le cadre grandit depuis son ancre, celle-ci est
reculée pour que l'ensemble reste dans le coin bas gauche de l'interface, sans recouvrir les
barres de vie ni les messages. Ancre du jeu mesurée sur des captures : (97 ; 664) ; la
fonction Lua `vint_get_property` n'a pas permis de la lire.

Vérifié par capture : anneau et carte concentriques. Non vérifié : les jauges de notoriété
elles-mêmes (il faut être recherché), et l'allure au casque.

## 6. Ce qui se vérifie sans le casque (mis à jour le 17 novembre)

Repris de WarbandVR : un **casque simulé** (SteamVR avec son pilote « null », dans une
configuration à part) et un **script d'entrées** rejoué par la DLL. `smoke.bat` lance le jeu,
entre dans la dernière sauvegarde par « Continuer », avance, capture les deux yeux, lit le
journal et ferme le jeu. `mesures.bat` rejoue la même scène avec plusieurs réglages.

| Vérification | Possible ? | Comment |
| --- | --- | --- |
| Le code compile | Oui | Conteneur (zig) puis MSVC sur le PC (`build.bat`) |
| La capture d'image est juste (pixels, paires, Reset, trois modes) | Oui, sur la vraie carte | `test.bat` |
| Le jeu démarre avec le mod, signatures et détours en place | Oui | `smoke.bat`, essai « démarrage » |
| La session OpenXR va jusqu'au bout et le casque reçoit les images | Oui, casque simulé | `smoke.bat`, essai « menu » |
| Le vrai relief en partie : paires complètes, œil droit non vide | Oui, casque simulé | `smoke.bat`, essai « partie » |
| Ce que voit chaque œil | Oui : captures `.bmp` dans `build\smoke\<essai>` | commande `shot` du script |
| La cadence en partie selon les réglages | Oui, sur une scène fixe | `mesures.bat` |
| Le suivi de tête, les manettes VR, la latence, le confort | Non : tête immobile, pas de manettes simulées | Toi, au casque |

### Mesures du 17 novembre

Même scène (réveil dans la planque, quelques pas), casque simulé à 60 Hz, RTX 3060 Ti :

| Réglages | Paires par seconde | Temps d'une paire |
| --- | --- | --- |
| Frein du jeu (`frame_cap=0`, comme avant la 0.28.0) | 30,2 | 33,1 ms |
| Frein levé (`frame_cap=100`, par défaut depuis la 0.28.0) | 47,7 | 20,7 ms |
| Frein levé et lecture par un autre thread (`async_capture=2`) | 49,4 (bute sur le plafond de 100 images) | 20,4 ms |
| Lecture par un autre thread, plafond à 240 | 51,7 | 18,1 ms |

Lever le frein donne donc **environ 58 % de paires en plus** sur cette scène. Au casque réel la
machine a d'autres charges (SteamVR envoie l'image au Steam Frame) : les chiffres y seront
sans doute plus bas, et l'écart reste à confirmer.

### Ce que les captures montrent déjà

- **L'interface n'était que dans l'œil gauche** : le défaut signalé au casque se voyait sur les
  captures de la 0.31.0. C'est ce qui a permis de le traiter sans le casque (section 5).
- La mini-carte est dessinée à l'écart de sa jauge circulaire : second défaut visible aussi.
- La tête et le chapeau du personnage apparaissent en haut de l'image à la première personne.
