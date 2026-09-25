JACoop — Jedi Academy en coopération (2 à 16 joueurs, campagne solo)
=====================================================================

Ce dossier ne contient AUCUN fichier du jeu : il faut posséder Star Wars Jedi
Knight: Jedi Academy (Steam ou GOG, version PC). Le lanceur trouve le jeu tout
seul ; sinon il demande le dossier GameData (celui qui contient base\assets0.pk3).
Rien n'est écrit dans le dossier du jeu : les sauvegardes vont dans
JACoop\saves, la config et les captures dans JACoop\base.

Jouer
-----
  JACoop.exe             Lance le jeu. Menu principal > COOPERATION :
                           HEBERGER             tu arrives dans le SALON et tu attends tes amis.
                                                NOUVELLE PARTIE : tu choisis la difficulté et ton
                                                personnage ; chaque invité crée alors le sien et
                                                la partie démarre quand tous ont validé (ou
                                                COMMENCER pour ne pas attendre).
                                                CONTINUER : charger une sauvegarde (chacun y
                                                retrouve son personnage et sa progression).
                           REJOINDRE            liste des parties du réseau local, ou l'adresse
                                                IP:PORT de l'hôte (Internet).
                           OPTIONS COOPERATION  pseudo, joueurs max, touche pour relever un
                                                coéquipier, joueurs à terre, lissage caméra...
                         Tu peux aussi jouer en solo normalement et ouvrir la
                         partie aux amis en jeu avec F6 > OUVRIR AUX AMIS.
  Rejoindre.cmd          (= JACoop.exe join) va directement à REJOINDRE.
                         Rejoindre.cmd ADRESSE saute la liste et va droit chez cet hôte.
  Solo.cmd               (= JACoop.exe solo) Solo classique avec ce moteur, sans réseau.

  F6 en jeu : salon / joueurs, personnage / skin, points de Force (invités),
  ramener les invités / rejoindre l'hôte (téléportation de secours), options.

À terre / relever (touche G)
----------------------------
  Un coup mortel ne tue pas : le joueur tombe à terre (immunisé, 60 s de
  « saignement »). Un coéquipier s'approche et maintient G (touche modifiable
  dans OPTIONS COOPERATION) 3 s pour le relever avec 40 % de vie. Un marqueur
  rouge indique où il est. Sans secours, il meurt vraiment et réapparaît 10 s
  plus tard à côté d'un coéquipier debout. Si tout le monde est à terre,
  l'hôte se voit proposer de RECHARGER LE DERNIER POINT DE CONTROLE (rechargé
  tout seul après 20 s). Tout cela se règle dans OPTIONS COOPERATION (ou se
  désactive : Joueurs à terre = Non, retour à la réapparition simple).

  Un joueur relevé récupère ses armes : s'il s'était fait désarmer, son sabre
  lui revient en main, et il ne se retrouve jamais les mains vides.

  Un écran qui reste figé sur un fondu (blanc ou noir) se lève tout seul au
  bout de 2,5 s tant qu'un joueur est debout. Certains scénarios du jeu, comme
  la bombe du train, fondent au blanc en comptant sur la mort du joueur et sur
  l'écran d'échec de mission pour reprendre la main - ce qui n'arrivait pas en
  coop. Un « tu devrais être mort ici » scripté touche désormais tous les
  joueurs, comme le scénario l'entendait, et plus seulement celui qui a
  déclenché le piège.

  Les trous ne font plus recharger la partie. Un joueur qui tombe dans le vide
  est reposé à terre sur le dernier sol où il se tenait, et ses coéquipiers
  peuvent aller le relever comme d'habitude (cvar g_coopPitRescue, 1 par
  défaut ; à 0 une chute redevient une vraie mort). Avant, la chute d'un seul
  joueur rembobinait la partie de tout le monde au dernier point de contrôle.

Skins / modèles de personnage
-----------------------------
  Le salon et le menu F6 ont deux entrees separees : PERSONNAGE pour le modele
  et SABRE LASER pour le sabre (type, manche, couleur), chacune utilisable a
  tout moment, meme en pleine partie.

  F6 > PERSONNAGE (ou PERSONNAGE dans le salon) liste tous les
  modèles de joueur installés : ceux du jeu (Kyle, Luke, Reborn...) et les
  skins ajoutés en .pk3 dans JACoop\base\ (skins Jedi Academy « MP » ou « SP »,
  dossier models/players/<nom>). Variante, portrait et aperçu 3D, changement
  immédiat même en pleine partie, vu par tous les joueurs.
  Les .pk3 de mods (skins ET sabres) voyagent tout seuls quand un joueur
  entre dans le salon, dans les deux sens : ceux de l'hôte descendent chez les
  invités, ceux d'un invité montent chez l'hôte puis repartent vers les autres
  invités. Ce sont des fichiers temporaires (coopdl_*.pk3 et coopup_*.pk3 dans
  JACoop\base) effacés à la fermeture du jeu, et l'hôte ne peut lancer la
  partie qu'une fois les transferts finis. Un mod trop gros passe son tour
  (cvars sv_coopTransferMaxMB côté hôte, 300 Mo par défaut, et
  cl_coopTransferMaxMB côté invité) : son porteur est alors le seul à voir son
  skin, les autres le voient en stormtrooper.

  Ton personnage se crée côté invité APRÈS la connexion, depuis le salon
  (bouton PERSONNAGE, ou l'écran que l'hôte ouvre en cliquant COMMENCER) :
  c'est à ce moment-là que les mods de l'hôte sont déjà arrivés, donc que ses
  modèles et ses manches sont dans tes listes.

PNJ (hôte)
---------
  F6 > PNJ liste tous les personnages que le jeu et tes mods connaissent (207
  dans le jeu d'origine), avec un aperçu 3D. FAIRE APPARAITRE le fait surgir
  devant toi ; les invités le voient comme n'importe quel personnage du niveau.
  L'écran active la triche tout seul (c'est une commande de triche du jeu) et
  te laisse la réactiver ou la couper à la main.

  Les skins de personnage sont dans la liste eux aussi, suivis de « (skin) »,
  y compris ceux reçus des autres joueurs. Un mod de skin n'apporte qu'un
  modèle : le jeu ne sait pas en faire un PNJ tout seul, il lui faut une fiche
  (vie, arme, camp). JACoop l'écrit à la volée et en fait un jedi au sabre. Ces
  fiches s'accumulent dans base/ext_data/npcs/zz_coop_models.npc : tu peux
  effacer ce fichier quand tu veux, ou l'éditer si tu veux d'autres réglages.

Sabres : manches et couleur de lame
-----------------------------------
  L'écran SABRE (création du personnage, ou PERSONNAGE depuis le salon) ne
  propose plus seulement les neuf manches du jeu.
  Le bouton du manche (il affiche le nom du manche courant, sous SABER HILT)
  ouvre la liste de TOUS les manches installés : ceux du jeu et
  ceux des mods de sabres posés en .pk3 dans JACoop\base\ (le mod doit
  contenir un ou plusieurs fichiers ext_data/sabers/*.sab ; testé avec la
  « Lightsaber Hilt Collection » de JKHub, 212 manches). Chaque manche porte
  le nom que lui donne son auteur, la liste suit le type de sabre choisi
  (simple / doubles / bâton), et l'aperçu 3D se met à jour quand tu changes
  de ligne. RETOUR remet le manche que tu avais.
  COULEUR EXACTE (RVB) ouvre un choix de couleur libre : trois curseurs
  rouge / vert / bleu de 0 à 255, une pastille et la lame en 3D qui suit le
  curseur. Les six couleurs du jeu restent là en raccourci et gardent leur
  rendu d'origine. Avec deux sabres, RVB DU SABRE GAUCHE fait la seconde lame.
  Le manche et la couleur voyagent avec ton personnage : les autres joueurs
  voient les tiens (le .pk3 de sabres leur est envoyé tout seul en arrivant
  dans le salon ; sans lui ils verraient un manche du jeu), et tout est gardé
  dans les sauvegardes.
  En console, g_saber_color / g_saber2_color acceptent aussi bien un nom
  (red, orange, yellow, green, blue, purple) qu'une couleur exacte, écrite
  "#ff8000" ou "rgb 255 128 0" ; la commande saberColor <1|2> <couleur> fait
  pareil en pleine partie.

Affichage
---------
  Par défaut le jeu s'ouvre en plein écran à la résolution du bureau (16/9,
  21/9...). OPTIONS > VIDEO > Mode vidéo propose aussi 1280x720 à 3840x2160.
  En écran large le champ de vision s'élargit (rien n'est coupé en haut/bas),
  les menus restent en 4/3 centrés et les éléments du HUD gardent leurs
  proportions dans les coins. Pour retrouver l'affichage étiré d'origine :
  console (Shift+²), r_aspect2D 0.
  ATH (le HUD) : barres fines à bouts arrondis, dans l'esprit de Jedi: Fallen
  Order — vie à gauche (turquoise, elle glisse vers un corail et respire quand
  elle descend), bouclier juste au-dessus quand il t'en reste, Force à droite
  avec ses crans, et la pastille ronde de l'arme avec ses munitions. Les barres
  sont dessinées en géométrie à partir d'une image blanche haute définition,
  donc nettes à n'importe quelle résolution. cg_coopHud 0 remet celui du jeu.

  Police : Titillium Web (SIL Open Font License, fichier fonts/OFL-TitilliumWeb.txt),
  en blanc ; l'entrée sélectionnée s'affiche en noir sur une barre blanche.

  Écrans larges (21/9, 3440x1440...) : le réticule dynamique, les barres de vie
  au-dessus des têtes et le verrouillage du lance-roquettes tombent maintenant
  à l'endroit exact qu'ils désignent. Ils étaient ramenés vers le centre de
  l'écran (de 44 % de leur écart en 3440x1440), d'où un tir qui ne partait pas
  dans le réticule. La lunette du disrupteur garde son ouverture étirée sur
  toute la largeur : ses graduations la suivent désormais au lieu de s'en
  détacher, mais l'anneau reste une ellipse - le redessiner en 4/3 centré avec
  des bandes noires sur les côtés serait plus joli, c'est un choix à faire.

  Champ de vision : OPTIONS COOPERATION > Champ de vision (FOV), de 80 à 120.
  La valeur est celle d'un écran 4/3 : sur un écran large l'image est élargie
  toute seule, tu n'as rien à compenser.

  Langue / Language
    Les textes du mod suivent la langue du jeu : en français si le jeu est en
    français, en anglais pour toutes les autres langues. Au premier lancement
    de cette version, la langue suit celle de Windows ; ensuite, OPTIONS >
    langue du jeu. En coop, chaque joueur voit les messages dans SA langue.
    The mod follows the game language: French for a French game, English for
    any other language. First launch: the language of Windows; then OPTIONS.

Réseau
------
  L'hôte écoute en UDP sur le port 29070 (29071-29079 si occupé). Sur Internet,
  l'hôte ouvre/redirige ce port UDP sur sa box, ou tout le monde passe par un
  VPN de type Hamachi/ZeroTier/Radmin. En LAN, COOPERATION > REJOINDRE liste
  les parties automatiquement.

  Le débit se règle aussi dans OPTIONS COOPERATION > Debit d'envoi des mods
  (jusqu'à 100 Mo/s ; au-delà de quelques Mo/s le réseau du jeu plafonne de
  lui-même, voir plus bas).

  Vitesse d'envoi des mods (console de l'hôte, effet immédiat) :
    sv_coopTransferRate     Ko/s par joueur qui télécharge, 1024 par défaut
                            (8 Mbit/s d'envoi). Un hôte en fibre peut monter
                            à 4096 ; un hôte avec peu d'envoi descend à 256.
                            0 = pas de limite.
    sv_coopTransferRateLan  pareil pour un joueur du même réseau local,
                            0 (pas de limite) par défaut.
  Attention : un VPN (Radmin, Hamachi) n'est pas vu comme un réseau local,
  c'est sv_coopTransferRate qui s'applique.

Ce qui marche
-------------
  - toute la campagne, cinématiques comprises (les invités voient la caméra de
    l'hôte), sous-titres, objectifs, transitions de niveau ;
  - chaque joueur a son propre personnage et ses sabres ; la difficulté est
    celle de l'hôte pour tout le monde ;
  - à terre / relever (voir plus haut), rechargement du dernier point de
    contrôle quand tout le monde est tombé ;
  - les ennemis s'en prennent à tous les joueurs, les déclencheurs, portes,
    leviers et énigmes réagissent à tous les joueurs ;
  - progression des invités : pouvoirs de Force, armes, munitions et inventaire
    sont gardés par l'hôte (fichier saves\<nom>.coop à côté de sa sauvegarde) et
    rendus au même joueur à la reconnexion ou au chargement (CONTINUER) ;
  - points de Force : les invités ont le même budget que l'hôte et les
    répartissent eux-mêmes (F6 > POINTS DE FORCE, ou depuis le salon).

Limites connues
---------------
  - le choix des armes entre missions reste celui de l'hôte (les invités
    ramassent les leurs sur le terrain) ;
  - seul l'hôte sauvegarde et charge ;
  - une sauvegarde faite avant cette version n'a pas de fichier .coop : les
    invités repartent avec le niveau de l'hôte.

Basé sur OpenJK (GPL v2) et sur le travail de jedi-outcast-coop (Benehiko).
Sources : dossier openjk (branche coop).
