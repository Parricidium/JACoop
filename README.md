<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/img/logo-white.png">
    <img src="docs/img/logo-dark.png" width="560" alt="Star Wars Jedi Knight: Jedi Academy COOP">
  </picture>
</p>

<p align="center">
  <a href="https://github.com/Parricidium/JACoop/releases/latest"><img src="https://img.shields.io/github/v/release/Parricidium/JACoop?label=Download&style=for-the-badge" alt="Download the latest release"></a>
  <a href="LICENSE.txt"><img src="https://img.shields.io/badge/license-GPLv2-blue?style=for-the-badge" alt="GPLv2"></a>
  <a href="https://www.nexusmods.com/starwarsjediknightjediacademy/mods/149"><img src="https://img.shields.io/badge/Nexus%20Mods-JACoop-D98F40?style=for-the-badge" alt="JACoop on Nexus Mods"></a>
  <a href="https://ko-fi.com/parricidium"><img src="https://img.shields.io/badge/Ko--fi-Support%20me-FF5E5B?style=for-the-badge&logo=ko-fi&logoColor=white" alt="Support me on Ko-fi"></a>
</p>

# JACoop — the Jedi Academy campaign in co-op, 2 to 16 players

A co-op mod for **Star Wars Jedi Knight: Jedi Academy**, built on the open-source
[OpenJK](https://github.com/JACoders/OpenJK) engine. One player hosts, the others join: everybody
plays the **single-player campaign** together, in the same world, each with their own character,
lightsaber, Force powers and progress. English and French.

It also brings the 2003 engine up to date, all of it optional: widescreen, a new HUD, modern
lighting with **software ray tracing** (shadows, occlusion, saber light, reflections, water, lava),
ragdoll corpses, custom skins and saber hilts that travel between players by themselves.

*[Version française plus bas.](#version-française)*

> You need your own copy of Jedi Academy (Steam or GOG, PC). No game data is provided here.

---

## Contents

- [Download and install](#download-and-install)
- [Playing](#playing)
- [Features](#features)
- [Modern rendering and ray tracing](#modern-rendering-and-ray-tracing)
- [Network](#network)
- [Known issues](#known-issues)
- [Building](#building)
- [Credits and license](#credits-and-license)
- [Version française](#version-française)

---

## Download and install

1. Download `JACoop-<version>.zip` from the [releases](https://github.com/Parricidium/JACoop/releases) or from [Nexus Mods](https://www.nexusmods.com/starwarsjediknightjediacademy/mods/149).
2. Unzip it **anywhere** (desktop, games folder...). It is a standalone folder: nothing is written
   into the game's folder.
3. Start `JACoop.exe`. It finds Jedi Academy by itself (Steam or GOG); otherwise it asks for the
   game's `GameData` folder (the one that contains `base\assets0.pk3`).
4. On first launch Windows may ask for network permission: allow it (private network).

Saves go to `JACoop\saves`, settings and screenshots to `JACoop\base`. Your original game and its
saves are never touched.

**Updating**: unzip the new version over the old one. Everybody in a party must run the same
version (the network protocol is checked when joining).

**Language**: the mod follows the game language — French for a French game, English for every other
language. On the first launch of a bilingual version it follows the language of Windows; after that,
*Options > Language* decides. In a party, each player reads the messages in **their own** language.

## Playing

`JACoop.exe` starts the game. Main menu > **CO-OP**:

| | |
|---|---|
| **HOST** | Opens the lobby; your friends join you. **NEW GAME**: pick the difficulty and your character, then each guest creates theirs and the game starts once everyone has confirmed (or **START** without waiting). **CONTINUE**: load a saved game, everyone gets their character and progress back. |
| **JOIN** | Games on the local network, or the host's `IP:PORT` (Internet). |
| **CO-OP OPTIONS** | Name, revive key, field of view, and the host's game settings (max players: 2 to 16). |

You can also play solo and open the game to friends at any time: **F6 > OPEN TO FRIENDS**.

- `Rejoindre.cmd` (= `JACoop.exe join`) goes straight to JOIN; `Rejoindre.cmd ADDRESS` joins that host.
- `Solo.cmd` (= `JACoop.exe solo`) plays the classic single player with this engine, no network.
- **F6** in game: lobby / players, character / skin, lightsaber, Force points (guests),
  bring the guests / go to the host (rescue teleport), NPC spawner (host), options.

**Downed / revive (G key)**: a fatal blow puts you down instead of killing you (60 s of bleeding
out). A teammate gets close and holds **G** for 3 s to bring you back with 40 % health; a red marker
shows where you are. Without help you really die and come back 10 s later next to a teammate who is
standing. If everyone is down, the last checkpoint reloads (by itself after 20 s). All of it is set
in CO-OP OPTIONS > HOST.

## Features

- **The whole campaign in co-op**: enemies target every player; triggers, doors, levers, puzzles
  and scripts react to every player; level changes, the mission map with a vote on the next mission,
  the loadout screen; cutscenes and videos shown to everybody.
- **Each player keeps their own progress**: Force powers, weapons, ammo and inventory are kept by the
  host next to its save and given back to the same player on reconnect or load. Guests spend their
  own Force points (F6 > FORCE POINTS).
- **Characters and skins**: every installed player model, including skins added as `.pk3` in
  `JACoop\base` (Jedi Academy MP or SP skins), with portrait and 3D preview, changed live even in
  game, seen by every player.
- **Lightsabers**: single, staff or dual; every installed hilt including saber packs (tested with a
  212-hilt collection); free RGB blade colour with sliders — kept in saves and seen by everyone.
- **Mods travel by themselves**: skin and saber `.pk3` files go through the lobby in both directions
  (host to guests, guest to host to the others) before the game starts.
- **NPC spawner (host)**: every character the game and your mods know, with a 3D preview; skins
  become NPCs on the fly.
- **Widescreen**: desktop resolution by default (16:9, 21:9...), Hor+ field of view, menus and HUD
  kept in proportion; the dynamic crosshair lands exactly where the shot goes, ultrawide included.
- **New HUD** in the spirit of *Jedi: Fallen Order* (health, shield, Force with its notches, weapon
  and ammo, saber stance), crisp at any resolution. `cg_coopHud 0` brings back the original.
- **Ragdoll corpses**: dead NPCs fall with physics when pushed, pulled or sent flying, then settle
  back into their lying animation; the host chooses whether the pose is synchronised for guests.
- **Pit rescue**: a player who falls into the void is put back, downed, on the last ground they stood
  on, instead of rewinding everybody to the last checkpoint.
- **English and French**: menus, HUD and messages.

## Modern rendering and ray tracing

CO-OP OPTIONS > **MODERN RENDERING** and **RAY TRACING**. Off, the game draws exactly as before;
everything is local (it changes nothing for the other players).

- **Modern rendering**: ambient occlusion, sun shadows from a real shadow map, light shafts.
- **Ray tracing** (any OpenGL 4.3 card, no RTX needed): exact soft sun shadows, true occlusion,
  per-pixel lights with cast shadows — saber blades light the scene **with their colour**, so do
  shots, explosions and lava — reflections on shiny surfaces and glass, water with ripples,
  refraction and a traced reflection, glowing lava, relief (normal maps) on the scenery. Characters,
  doors and lifts cast and receive the shadows. *Resolution* (Quarter / Half / Full) and the number of
  rays set the cost.

## Network

The host listens on **UDP 29070** (29071-29079 if busy). Over the Internet, the host forwards this
UDP port on their router, or everybody uses a VPN such as Hamachi, ZeroTier or Radmin. On a LAN,
CO-OP > JOIN lists the games automatically.

## Known issues

Being worked on or not reproduced yet — a report with the `qconsole.log` of **both** players helps a
lot (`JACoop\base\qconsole.log`, overwritten at each launch: copy it before restarting).

1. **A guest sometimes loses their lightsaber** after being pushed or pulled with the Force while
   holding another weapon. Not reproduced on the test bench yet; this version logs the exact moment
   (`coop: SABRE PERDU` in the logs). Pulling a teammate no longer disarms them.
2. **A guest can stay stuck in the saber guard stance** until someone hits them. Not diagnosed yet.
3. **A guest's saber blade became short** once while they were in the lightsaber menu, until the
   map was reloaded. Not reproduced yet.
4. **Ambient occlusion is heavy** on some graphics cards at high resolution: lower *Resolution* /
   *Rays* in RAY TRACING, or turn occlusion off.
5. **Force pull at level 3 on a teammate** throws them quite far (it is the game's own pull).
6. **The disruptor scope ring stays an ellipse** on wide screens (its graduations follow it and the
   shot is exact; purely cosmetic).
7. **Ragdolls**: a corpse can pop slightly when it settles back into its lying animation; with
   *Ragdoll sync* off, guests simulate their own and the pose can differ from the host's.
8. **A guest who joins while the host is watching a level's opening cutscene can freeze that
   cutscene** on the host. Join once the cutscene is over (or the host skips it); reload if it happens.
9. **Only the host saves and loads.**
10. **Other players' skins and hilts** need the same `.pk3`: it is sent in the lobby, but a mod over
    the size limit (300 MB by default, `sv_coopTransferMaxMB` / `cl_coopTransferMaxMB`) is skipped and
    its owner shows up as a stormtrooper for the others.
11. **No kick and no password** for the lobby (by choice for now).
12. **A save made before 16 players (versions up to 2026.09.24l)** still loads, but its level keeps
    some of the new player slots: up to 4 players until the next level, then 16.
13. **16 players** was tested with 6 on one machine; beyond, the host's upload and CPU are the limit,
    and the single-player levels (lifts, doors, cutscenes) get crowded.

## Building

This repository is OpenJK (branch `coop-gfx`) with the co-op mod in the engine and game code. The
original OpenJK readme is [README.OpenJK.md](README.OpenJK.md).

1. Build with CMake and Visual Studio 2022 (x64), as OpenJK: the single-player targets
   `openjk_sp.x86_64.exe`, `rdsp-vanilla_x86_64.dll` and `jagamex86_64.dll`.
2. The mod's data and tools are in [`jacoop/`](jacoop). The menus are generated:
   `python jacoop/gen_coop_menus.py`, then `python jacoop/skin_menus.py` (which also writes the
   English / French strings from `jacoop/i18n/`). `jacoop/pk3/` zipped becomes `base/zz_jacoop.pk3`.
3. The launcher: `jacoop/launcher-src/build-launcher.cmd`.

## Credits and license

- Engine: [OpenJK](https://github.com/JACoders/OpenJK) (GPLv2), from the Jedi Academy source code
  released by Raven Software. Co-op groundwork inspired by *jedi-outcast-coop* (Benehiko).
- Font: Titillium Web (SIL Open Font License, `jacoop/fonts-src/OFL.txt`).
- Star Wars, Jedi Knight and Jedi Academy are trademarks of Lucasfilm / Disney. This is a fan project,
  not affiliated with them; you need your own copy of the game.
- License: **GPLv2**, like OpenJK ([LICENSE.txt](LICENSE.txt)).

If you enjoy it, you can [support me on Ko-fi](https://ko-fi.com/parricidium). ❤️

---

# Version française

**JACoop** — la campagne solo de **Star Wars Jedi Knight: Jedi Academy** en coopération, de 2 à 16
joueurs, sur le moteur libre [OpenJK](https://github.com/JACoders/OpenJK). Un joueur héberge, les
autres le rejoignent : tout le monde joue la **campagne solo** ensemble, dans le même monde, chacun
avec son personnage, son sabre laser, ses pouvoirs de Force et sa progression. Anglais et français.

Le mod modernise aussi le moteur de 2003, tout en option : écran large, nouvel ATH, éclairage moderne
avec **ray tracing logiciel** (ombres, occlusion, lumière des sabres, reflets, eau, lave), cadavres en
ragdoll, skins et manches de sabre qui voyagent tout seuls entre les joueurs.

> Il faut posséder Jedi Academy (Steam ou GOG, PC). Aucun fichier du jeu n'est fourni ici.

## Téléchargement et installation

1. Télécharge `JACoop-<version>.zip` dans les [releases](https://github.com/Parricidium/JACoop/releases) ou sur [Nexus Mods](https://www.nexusmods.com/starwarsjediknightjediacademy/mods/149).
2. Décompresse-le **où tu veux** (bureau, dossier de jeux...). C'est un dossier autonome : rien n'est
   écrit dans le dossier du jeu.
3. Lance `JACoop.exe`. Il trouve Jedi Academy tout seul (Steam ou GOG) ; sinon il demande le dossier
   `GameData` du jeu (celui qui contient `base\assets0.pk3`).
4. Au premier lancement, Windows peut demander l'accès réseau : accepte (réseau privé).

Les sauvegardes vont dans `JACoop\saves`, la config et les captures dans `JACoop\base`. Le jeu
d'origine et ses sauvegardes ne sont jamais touchés.

**Mise à jour** : décompresse la nouvelle version par-dessus l'ancienne. Tout le monde doit avoir la
même version (le protocole réseau est vérifié à la connexion).

**Langue** : le mod suit la langue du jeu — français pour un jeu en français, anglais pour toutes les
autres langues. Au premier lancement d'une version bilingue, il suit la langue de Windows ; ensuite,
*Options > Langue* décide. En partie, chaque joueur lit les messages dans **sa** langue.

## Jouer

`JACoop.exe` lance le jeu. Menu principal > **COOPERATION** :

| | |
|---|---|
| **HEBERGER** | Ouvre le salon ; tes amis te rejoignent. **NOUVELLE PARTIE** : tu choisis la difficulté et ton personnage, puis chaque invité crée le sien et la partie démarre quand tous ont validé (ou **COMMENCER** sans attendre). **CONTINUER** : charger une sauvegarde, chacun retrouve son personnage et sa progression. |
| **REJOINDRE** | Parties du réseau local, ou l'`IP:PORT` de l'hôte (Internet). |
| **OPTIONS COOPERATION** | Pseudo, touche pour relever, champ de vision, et les réglages de partie de l'hôte (joueurs max : 2 à 16). |

Tu peux aussi jouer en solo et ouvrir la partie aux amis à tout moment : **F6 > OUVRIR AUX AMIS**.

- `Rejoindre.cmd` (= `JACoop.exe join`) va droit à REJOINDRE ; `Rejoindre.cmd ADRESSE` rejoint cet hôte.
- `Solo.cmd` (= `JACoop.exe solo`) : le solo classique avec ce moteur, sans réseau.
- **F6** en jeu : salon / joueurs, personnage / skin, sabre laser, points de Force (invités),
  ramener les invités / rejoindre l'hôte (téléportation de secours), PNJ (hôte), options.

**À terre / relever (touche G)** : un coup mortel met à terre au lieu de tuer (60 s de saignement).
Un coéquipier s'approche et maintient **G** 3 s pour te relever avec 40 % de vie ; un marqueur rouge
montre où tu es. Sans secours, tu meurs vraiment et tu reviens 10 s plus tard à côté d'un coéquipier
debout. Si tout le monde est à terre, le dernier point de contrôle se recharge (tout seul après 20 s).
Tout se règle dans OPTIONS COOPERATION > HOTE.

## Fonctionnalités

- **Toute la campagne en coop** : les ennemis visent tous les joueurs ; déclencheurs, portes, leviers,
  énigmes et scripts réagissent à tous ; changements de niveau, carte des missions avec vote de la
  mission suivante, écran d'équipement ; cinématiques et vidéos montrées à tout le monde.
- **Chacun garde sa progression** : pouvoirs de Force, armes, munitions et inventaire sont gardés par
  l'hôte à côté de sa sauvegarde et rendus au même joueur à la reconnexion ou au chargement. Les
  invités répartissent leurs propres points de Force (F6 > POINTS DE FORCE).
- **Personnages et skins** : tous les modèles installés, y compris les skins ajoutés en `.pk3` dans
  `JACoop\base` (skins Jedi Academy MP ou SP), avec portrait et aperçu 3D, changés en direct même en
  pleine partie, vus par tous.
- **Sabres laser** : simple, double lame ou deux sabres ; tous les manches installés, packs de sabres
  compris (testé avec une collection de 212 manches) ; couleur de lame RVB libre — gardés dans les
  sauvegardes et vus par tous.
- **Les mods voyagent tout seuls** : les `.pk3` de skins et de sabres passent par le salon dans les
  deux sens (de l'hôte vers les invités, d'un invité vers l'hôte puis les autres) avant la partie.
- **PNJ (hôte)** : tous les personnages que le jeu et tes mods connaissent, avec aperçu 3D ; les skins
  deviennent des PNJ à la volée.
- **Écran large** : résolution du bureau par défaut (16/9, 21/9...), champ de vision Hor+, menus et ATH
  en proportions ; le réticule dynamique tombe exactement où part le tir, ultra-large compris.
- **Nouvel ATH** dans l'esprit de *Jedi: Fallen Order* (vie, bouclier, Force et ses crans, arme et
  munitions, style de sabre), net à toute résolution. `cg_coopHud 0` remet celui du jeu.
- **Cadavres en ragdoll** : les PNJ morts tombent en physique quand on les pousse, les tire ou qu'ils
  volent, puis reprennent leur animation allongée ; l'hôte choisit si la pose est synchronisée.
- **Sauvetage des chutes** : un joueur qui tombe dans le vide est reposé à terre sur le dernier sol où
  il se tenait, au lieu de rembobiner tout le monde au dernier point de contrôle.
- **Anglais et français** : menus, ATH et messages.

## Rendu moderne et ray tracing

OPTIONS COOPERATION > **RENDU MODERNE** et **RAY TRACING**. Éteint, le jeu dessine exactement comme
avant ; tout est local (ça ne change rien pour les autres joueurs).

- **Rendu moderne** : occlusion ambiante, ombres du soleil par une vraie carte d'ombre, rayons
  crépusculaires.
- **Ray tracing** (toute carte OpenGL 4.3, pas besoin de RTX) : ombres douces exactes du soleil,
  occlusion vraie, lumières par pixel avec ombres portées — les lames de sabre éclairent la scène **de
  leur couleur**, les tirs, les explosions et la lave aussi — reflets sur les surfaces brillantes et le
  verre, eau avec ondulations, réfraction et reflet tracé, lave lumineuse, relief (normal maps) du
  décor. Personnages, portes et ascenseurs projettent et reçoivent les ombres. *Finesse*
  (Quart / Moitié / Pleine) et le nombre de rayons règlent le coût.

## Réseau

L'hôte écoute en **UDP 29070** (29071-29079 si occupé). Sur Internet, l'hôte redirige ce port UDP sur
sa box, ou tout le monde passe par un VPN de type Hamachi, ZeroTier ou Radmin. En réseau local,
COOPERATION > REJOINDRE liste les parties automatiquement.

## Bugs connus

En cours ou pas encore reproduits — un signalement avec le `qconsole.log` des **deux** joueurs aide
beaucoup (`JACoop\base\qconsole.log`, écrasé à chaque lancement : le copier avant de relancer).

1. **Un invité perd parfois son sabre laser** après une poussée ou un tirage de Force alors qu'il tient
   une autre arme. Pas encore reproduit en test ; cette version note le moment exact
   (`coop: SABRE PERDU` dans les journaux). Tirer un coéquipier ne le désarme plus.
2. **Un invité peut rester bloqué en position de garde** jusqu'à ce qu'on le frappe. Pas encore
   diagnostiqué.
3. **La lame d'un invité est devenue courte** une fois pendant qu'il était dans le menu du sabre,
   jusqu'au rechargement de la carte. Pas encore reproduit.
4. **L'occlusion ambiante est lourde** sur certaines cartes graphiques en haute résolution : baisser
   *Finesse* / *Rayons* dans RAY TRACING, ou couper l'occlusion.
5. **Le tirage de Force niveau 3 sur un coéquipier** l'envoie assez loin (c'est le tirage du jeu).
6. **L'anneau de la lunette du disrupteur reste une ellipse** en écran large (les graduations le
   suivent et le tir est exact ; purement visuel).
7. **Ragdolls** : un cadavre peut « sauter » légèrement en reprenant son animation allongée ; avec
   *Sync ragdoll* coupé, les invités simulent le leur et la pose peut différer de celle de l'hôte.
8. **Un invité qui rejoint pendant la cinématique d'ouverture d'un niveau peut la figer** chez l'hôte.
   Rejoindre une fois la cinématique finie (ou l'hôte la passe) ; recharger si ça arrive.
9. **Seul l'hôte sauvegarde et charge.**
10. **Les skins et manches des autres** demandent le même `.pk3` : il est envoyé dans le salon, mais un
    mod au-delà de la limite (300 Mo par défaut, `sv_coopTransferMaxMB` / `cl_coopTransferMaxMB`) passe
    son tour et son porteur apparaît en stormtrooper chez les autres.
11. **Pas d'exclusion ni de mot de passe** pour le salon (choix actuel).
12. **Une sauvegarde faite avant les 16 joueurs (versions jusqu'à 2026.09.24l)** se charge toujours, mais
    son niveau garde certaines des nouvelles places de joueur : 4 joueurs jusqu'au niveau suivant, puis 16.
13. **16 joueurs** : testé à 6 sur une seule machine ; au-delà, le débit montant et le processeur de
    l'hôte sont la limite, et les niveaux solo (ascenseurs, portes, cinématiques) deviennent encombrés.

## Compiler

Ce dépôt est OpenJK (branche `coop-gfx`) avec le mod coop dans le code du moteur et du jeu. Le readme
d'origine d'OpenJK est [README.OpenJK.md](README.OpenJK.md). Compilation CMake + Visual Studio 2022
(x64) comme OpenJK ; les données et outils du mod sont dans [`jacoop/`](jacoop) : menus générés par
`gen_coop_menus.py` puis `skin_menus.py`, textes anglais / français dans `jacoop/i18n/`, `jacoop/pk3/`
zippé = `base/zz_jacoop.pk3`, lanceur dans `jacoop/launcher-src/`.

## Crédits et licence

- Moteur : [OpenJK](https://github.com/JACoders/OpenJK) (GPLv2), issu du code source de Jedi Academy
  publié par Raven Software. Bases de la coop inspirées de *jedi-outcast-coop* (Benehiko).
- Police : Titillium Web (SIL Open Font License, `jacoop/fonts-src/OFL.txt`).
- Star Wars, Jedi Knight et Jedi Academy sont des marques de Lucasfilm / Disney. Projet de fan, sans
  lien avec eux ; il faut posséder le jeu.
- Licence : **GPLv2**, comme OpenJK ([LICENSE.txt](LICENSE.txt)).

Si le mod te plaît, tu peux [me soutenir sur Ko-fi](https://ko-fi.com/parricidium). ❤️
