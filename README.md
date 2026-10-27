# ndsemu

Émulateur Nintendo DS écrit en C++17, avec une bibliothèque de jeux et une fenêtre de jeu pour Windows.

Le projet est un hobby en cours de développement. Deux jeux ont été testés et sont jouables :

| Jeu | État |
|---|---|
| New Super Mario Bros. (EUR) | Jouable : graphismes, son, écran tactile, sauvegarde |
| Mario Kart DS (EUR) | Jouable : menus 3D, courses à 60 images/s, ombres, sauvegarde |

Les autres jeux n'ont pas encore été essayés.

## Compiler

Il faut Windows 10 ou 11 (64 bits), Visual Studio 2022 (outils C++) et CMake 3.16 ou plus récent.

```
cmake -S . -B build
cmake --build build --config Release
```

Le programme est écrit dans `build\Release\ndsemu.exe`. Le runtime Visual C++ est inclus dans l'exe : il fonctionne
sur un autre PC sans rien installer.

Le cœur de l'émulateur (`src/core`) compile aussi sous Linux (`g++` ou `clang++`), sans fenêtre ni son : c'est le mode
utilisé pour les tests et les captures.

## Utilisation

### La bibliothèque

Lancé **sans argument** (double-clic sur `ndsemu.exe`), le programme ouvre la bibliothèque de jeux :

- **Ajouter un dossier** : les fichiers `.nds` qu'il contient sont listés (sous-dossiers compris, sur 3 niveaux).
  Les dossiers sont surveillés : un jeu ajouté ou retiré apparaît ou disparaît tout seul.
- On peut aussi **glisser** des fichiers `.nds` ou un dossier sur la fenêtre.
- Chaque jeu montre l'icône et le titre lus dans la ROM, l'éditeur, le code du jeu, la taille, et « Sauvegarde »
  quand un fichier `.sav` existe.
- **Double-clic** (ou Entrée) : le jeu s'ouvre dans une nouvelle fenêtre ; « En cours » s'affiche à côté.
- **Clic droit** sur un jeu : lancer, arrêter, choisir la compatibilité, ouvrir l'emplacement.
- **Récemment joués** : les derniers jeux lancés en haut de la liste, avec la date de la dernière partie et le
  temps de jeu total (compté quand le jeu n'est pas en pause, gardé dans `ndsemu_temps_de_jeu.ini`). Se masque
  dans **View**.
- Recherche (Ctrl+F), vue liste ou grille, tri par colonne, ouverture directe d'une ROM (Ctrl+O), nouvelle analyse
  des dossiers (F5).

Les réglages de la bibliothèque sont gardés dans `ndsemu_bibliotheque.ini`, à côté de l'exe.

### Lancer un jeu directement

```
ndsemu.exe "C:\chemin\vers\jeu.nds"
```

Glisser un fichier `.nds` sur `ndsemu.exe` fait la même chose.

### Touches

| Clavier | Console |
|---|---|
| Flèches | Croix directionnelle |
| Z / X | A / B |
| S / A | X / Y |
| Q / W | L / R |
| Entrée | Start |
| Retour arrière | Select |
| Clic gauche sur l'écran du bas | Stylet |

Ce sont les touches par défaut. Elles se changent dans **Jeu > Touches et manette** (fenêtre de jeu) ou dans
**Help > Touches et manette** et la roue des réglages (bibliothèque). La même fenêtre règle la **manette** : les
manettes Xbox et compatibles XInput, PS5 (DualSense) et PS4 (DualShock 4), en USB ou en Bluetooth, sont reconnues
dès qu'elles sont branchées. Par défaut, la disposition suit celle de la DS : le bouton de droite (B Xbox, Rond PS)
= A, celui du bas (A Xbox, Croix PS) = B, Y / Triangle = X, X / Carré = Y, LB / L1 et RB / R1 = L et R,
Menu / Options = Start, View / Create = Select, croix et stick gauche = croix directionnelle. Les réglages sont
gardés dans `ndsemu_touches.ini` ; une fenêtre de jeu ouverte les reprend dès qu'ils changent.

### Fenêtre de jeu

| Raccourci | Action |
|---|---|
| P | Pause |
| Ctrl+R | Réinitialiser (la sauvegarde est écrite avant) |
| M | Couper / remettre le son |
| Ctrl+1 à Ctrl+4 | Taille ×1 à ×4 |
| F11 ou Alt+Entrée | Plein écran (Échap pour sortir) |
| F1 à F4 | Charger l'état 1 à 4 |
| Maj+F1 à Maj+F4 | Sauvegarder l'état 1 à 4 |
| F12 | Instantané de diagnostic |

Les menus **Jeu**, **Affichage** et **Son** donnent aussi accès à la disposition des écrans (l'un sous l'autre ou côte
à côte), au lissage de l'image, au volume et à un panneau de performances. Ces réglages sont gardés dans
`ndsemu_fenetre.ini`.

### Sauvegardes et firmware

- **Sauvegardes d'état** : 4 emplacements par jeu (menu **Jeu > Sauvegarder l'état / Charger l'état**, avec une
  miniature et la date, ou Maj+F1..F4 / F1..F4). Elles contiennent tout l'état de la console et se rechargent à
  l'instant près. Fichiers : `etats/<nom du jeu>.etat1` à `.etat4`, à côté de l'exe (environ 6 Mo chacun). Une
  sauvegarde d'état ne se charge que dans le jeu qui l'a créée. La bibliothèque indique combien un jeu en a.
- La partie est enregistrée automatiquement dans un fichier `.sav` à côté de la ROM, avec le même nom. Le type de
  puce de sauvegarde (EEPROM 512 o, EEPROM / FRAM 8-64 Ko, flash 256 Ko et plus) est déduit de la taille du `.sav`,
  ou reconnu à la première lecture du jeu.
- Si un jeu modifie les réglages de la console, ils sont gardés dans `firmware.bin` à côté de l'exe.

### Mises à jour

Au démarrage, la bibliothèque demande à GitHub la dernière version publiée (GitHub Releases du dépôt réglé dans
`CMakeLists.txt`, `NDSEMU_GITHUB_REPO`). S'il y en a une plus récente, un bandeau le signale : **Voir les
nouveautés** affiche le texte de la version, **Installer** télécharge l'archive, remplace l'exe et les fichiers
fournis (jamais les réglages, sauvegardes, états ni `firmware.bin`), puis **Redémarrer maintenant** relance ndsemu.
On peut aussi **Ignorer cette version**. **Help > Rechercher des mises à jour** fait la recherche à la demande ; la
recherche au démarrage se coupe dans les réglages (roue dentée). Les jeux en cours doivent être fermés avant
d'installer. L'ancien exe est gardé en `ndsemu.exe.old` jusqu'au lancement suivant.

### Options de la ligne de commande

| Option | Effet |
|---|---|
| `--fenetre` | Ouvre la fenêtre de jeu (implicite avec un seul argument) |
| `--sans-fenetre` | Émule sans fenêtre (tests, captures) |
| `--sans-son` | Pas de sortie audio |
| `--rapport` | Rapport complet à la fermeture (à rediriger dans un fichier) |
| `--diag` | Surveillance détaillée (journal IPC, plus lent) |
| `--enregistrer-son` | Écrit le son produit dans `son.wav` |
| `--journal-son` | Écrit `son_journal.txt` (départs et arrêts des canaux) |
| `--sans-culling`, `--culling-inverse` | Diagnostic de la 3D |

Sans fenêtre, deux nombres optionnels suivent la ROM : le nombre maximal d'instructions ARM9, puis « une capture
toutes les N images » (écrites dans `images/`).

Exemple de rapport sous PowerShell :

```
.\build\Release\ndsemu.exe "jeu.nds" --fenetre --rapport | Out-File -Encoding utf8 diag.txt
```

## Version PS5 (et Linux)

Une version SDL2 tourne sur PS5 avec jailbreak (programme maison lancé par le Homebrew Launcher de websrv) et sur
Linux. Elle a sa propre interface pensée pour la manette : liste des jeux, menu en jeu (pavé tactile) avec
sauvegardes d'état, disposition des écrans, volume, réinitialisation.

- Compilation PS5 (Linux ou WSL Ubuntu 24.04, avec le [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk)) :
  `export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk` puis `./tools/build_ps5.sh`. Résultat : `build_ps5/ndsemu-ps5.zip`
  (dossier `homebrew/ndsemu` avec `eboot.elf`, l'icône et le mode d'emploi `LISEZMOI_PS5.txt`).
- Compilation Linux : `cmake -S . -B build -DNDSEMU_SDL=ON && cmake --build build`, puis `./build/ndsemu_sdl [jeu.nds]`.
- Le code commun (console complète : `src/core/emulator.*`) ne dépend d'aucun système.

## Publier une mise à jour

1. Augmenter la version dans `CMakeLists.txt` (`project(ndsemu VERSION 1.1.0 ...)`), puis recompiler en Release.
2. Écrire les nouveautés de la version dans `NOUVEAUTES.md` (Markdown simple : titres, listes, gras).
3. Avec [GitHub CLI](https://cli.github.com) connecté (`gh auth login`) :
   `powershell -ExecutionPolicy Bypass -File .\preparer_partage.ps1 -Publier`.
   Le script crée l'archive `ndsemu-windows-<version>.zip` et la version `v<version>` sur GitHub.

Sans GitHub CLI : lancer le script sans `-Publier`, puis sur GitHub **Releases > Draft a new release**, étiquette
`v<version>`, coller les nouveautés et joindre `ndsemu-windows-<version>.zip`. Le dépôt doit être public (ndsemu
interroge GitHub sans compte). Une archive dont le nom contient « ps5 » n'est jamais prise par la version Windows.

## Diagnostic

- **F12** dans la fenêtre de jeu écrit `etat_NNN.txt` (registres 2D et 3D, liste des polygones), `etat_NNN.png` (les
  deux écrans) et `etat_NNN_3d.png` (l'image 3D seule, le magenta est transparent).
- `--rapport` affiche à la fermeture : l'état des processeurs, les accès mémoire non gérés, les registres des moteurs
  2D et 3D, la capture d'écran, le son, la sauvegarde et la cartouche.
- Si l'émulation s'arrête sur une erreur, un rapport est écrit dans `ndsemu_erreur.txt`, à côté de l'exe.

## Ce qui est émulé

**Processeurs** : ARM9 (ARMv5TE) et ARM7 (ARMv4T), jeux d'instructions ARM et Thumb, interpréteur. Appels BIOS émulés
en haut niveau (division, racine carrée, décompression, CRC, attente d'interruption...). Pas de BIOS ni de firmware
d'origine nécessaires.

**Mémoire** : RAM principale, WRAM partagée et de l'ARM7, TCM, banques de VRAM A à I et leurs miroirs, DMA (y compris
les registres de remplissage), minuteries, interruptions, FIFO et synchronisation entre processeurs, diviseur et
racine carrée matériels.

**Graphismes 2D** : deux moteurs, fonds en tuiles, à rotation/zoom, étendus et bitmap, palettes étendues, sprites
(normaux, à rotation/zoom, semi-transparents, fenêtre, bitmap), fenêtres WIN0/WIN1/sprites, transparence,
éclaircissement, assombrissement, luminosité générale, **capture d'écran** (utilisée pour la 3D sur les deux écrans).

**Graphismes 3D** : géométrie complète (matrices, éclairage, textures dans tous les formats dont le format compressé),
tests BOX/POS/VEC, polygones translucides, **volumes d'ombre** (masque et pochoir), **contours** (edge marking),
**brouillard**, toon et surbrillance, test alpha, **anticrénelage** (bords adoucis selon la part du pixel
couverte), défilement de la couche 3D.

**Son** : 16 canaux (PCM 8/16 bits, IMA-ADPCM, ondes carrées, bruit), capture, mixage, sortie Windows à 48 kHz avec
rééchantillonnage et réglage automatique du retard.

**Divers** : écran tactile (TSC2046), firmware SPI avec écriture, horloge temps réel, puces de sauvegarde de la
cartouche avec détection automatique.

### Pas encore fait

- 3D : fil de fer, mode w-buffer.
- 2D : mosaïque, affichage depuis la mémoire principale.
- Micro, Wi-Fi, jeux DSi.

## Structure du code

```
src/
  main.cpp            ligne de commande, boucle d'émulation, rapports
  core/               cœur de l'émulateur, sans dépendance au système
    arm.*             interpréteur ARM9 / ARM7, appels BIOS
    memory.*          bus mémoire, VRAM, DMA, minuteries, IPC, SPI, RTC, sauvegarde
    video.*           moteurs 2D, composition, capture d'écran, PNG
    gx.*              moteur 3D
    sound.*           son
    nds_header.*      en-tête de la ROM
    rom.*             lecture des fichiers
    savestate.*       sauvegardes d'état
    emulator.*        la console complète (chargement, images, sauvegardes, états) pour toute interface
  platform/           Windows : fenêtre de jeu (Direct3D 11 + menus), sortie audio (waveOut),
                      touches et manette (XInput, PlayStation en HID)
  platform/sdl/       version SDL2 (PS5, Linux) : boucle, interface manette, rendu logiciel de Dear ImGui
  ui/                 bibliothèque de jeux : analyse des dossiers, interface, hôte Windows, mises à jour
third_party/imgui/    Dear ImGui 1.91.9 (licence MIT)
assets/               icône PS5, polices DejaVu réduites (version SDL)
tools/                build_ps5.sh (compilation PS5), embed_fonts.py
```

## Dépendances

- [Dear ImGui](https://github.com/ocornut/imgui) 1.91.9, fournie dans `third_party/imgui` (licence MIT).
- Windows SDK : Direct3D 11, DWM, waveOut, boîtes de dialogue du Shell, XInput (chargé à l'exécution), HID et SetupAPI
  (manettes PlayStation), WinHTTP (mises à jour).

## Avertissement

Aucun jeu, BIOS ni firmware n'est fourni. Utilisez des copies de vos propres cartouches, et ne mettez jamais de ROMs
dans le dépôt (voir `.gitignore`).
