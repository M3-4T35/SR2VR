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
| Abandonné (plantages, œil droit blanc) | Interface recopiée dans l'œil droit (0.26.1 à 0.26.3) | `essai/interface-deux-yeux`, sur le PC seulement |

Les branches `claude/…` sont empilées dans l'ordre du tableau : la dernière contient tout.

Problèmes ouverts, signalés au casque :

1. **L'interface en jeu (mini-carte, vie, roue des armes) n'apparaît que dans l'œil gauche** en
   vrai relief. Voir la section 5.
2. **La mini-carte ne rétrécit pas avec le reste de l'interface** : les jauges de notoriété
   (police, gangs) autour d'elle suivent `[Hud]`, pas la carte, dessinée par un autre code du
   jeu. Réglé du même coup par la section 5.
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

## 5. Interface en jeu dans les deux yeux : le plan

Ce qui a échoué : recopier les commandes de dessin de l'interface de l'œil gauche dans la
liste de l'œil droit. Les commandes dépendent d'états posés par d'autres commandes ; trois
essais, deux plantages et un œil blanc. Chaque essai demandait le casque.

Ce que je propose à la place, comme le fait le mod VR de Mount & Blade Warband :

1. Le thread du rendu exécute la liste des commandes (fonction 0x00CF8FA0). Les dessins 2D de
   l'interface (types 0x1C, 0x1D, texte 0x29) arrivent en fin de liste.
2. Au premier dessin 2D d'une image, détourner la cible de rendu vers **une texture à nous,
   transparente** ; la remettre à la fin de la liste.
3. Relire cette texture (elle change peu : une fois par paire suffit) et la montrer au casque
   comme **un panneau posé dans l'espace** (calque « quad » OpenXR, comme le point du laser).
   Les deux yeux le voient, net, à la distance choisie, et sa taille ne dépend plus des
   facteurs d'échelle du jeu : la mini-carte et ses jauges restent ensemble.
4. Les images du jeu ne contiennent plus d'interface : plus de différence entre les yeux.

Points durs connus : la transparence (le jeu ne tient pas le canal alpha de sa cible, il
faudra forcer un mélange séparé pour l'alpha), le device en PUREDEVICE (on ne peut pas relire
ses états, il faut les suivre), et repérer sans erreur la frontière entre la scène et
l'interface. Il faut le casque pour itérer : à faire en séance, pas à distance.

## 6. Ce que Claude peut vérifier sans le casque

| Vérification | Possible ? | Comment |
| --- | --- | --- |
| Le code compile | Oui | Dans le conteneur (zig) puis MSVC sur le PC (`build.bat`) |
| La capture d'image est juste (pixels, paires, Reset, trois modes) | Oui, sur la vraie carte | `test.bat` |
| Ce que coûte la capture | Oui | `bench.bat` |
| Les réglages du lanceur et de la DLL concordent | Oui | `tools/check_settings.py` |
| Le jeu démarre avec le mod, les détours se posent, pas de plantage | Oui, **avec ton accord** : lancer le jeu sur l'écran, sans casque, et lire le journal | `play.bat` par l'explorateur, puis `sr2_vr.log` |
| Les lignes « cadence » et « temps par image » en jeu | En partie : sans casque, pas de paires d'images à mesurer comme en vrai relief | idem |
| L'image dans le casque, le relief, la latence, les manettes, le confort | Non | Toi, au casque |

Limite du lancement sur l'écran : je ne sais pas fermer le jeu proprement moi-même, et un jeu
en plein écran gêne une session à distance. D'où l'accord demandé à chaque fois.
