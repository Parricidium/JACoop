"""JACoop - habillage "style Arx" de tous les menus du jeu.

Copie les .menu du jeu (ref-ui/ui, extrait d'assets1.pk3) dans dist/pk3/ui en
remplacant les couleurs des boites/bordures bleues et des titres, et applique
les memes remplacements aux menus coop du mod. Le decor en images (fond,
cadres bleus, boites animees) est refait par dist/pk3/shaders/jacoop_ui.shader.

    python dist/skin_menus.py
"""
import io, os, re, glob

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'ref-ui', 'ui')
DST = os.path.join(ROOT, 'dist', 'pk3', 'ui')

# fichiers du mod deja ecrits a la main (pas recopies depuis le jeu)
OWN = {'main.menu', 'coop.menu', 'cooplobby.menu', 'coopingame.menu', 'coopforce.menu', 'coopskins.menu', 'coopoptions.menu',
       'coopalldown.menu', 'coopdebrief.menu', 'coopvote.menu', 'cooploadout.menu', 'coopwpn.menu', 'menus.txt', 'ingame.txt'}
# fichiers du jeu qu'on ne touche pas (HUD, ecrans de chargement)
SKIP = {'hud.menu', 'loadscreen.menu', 'jahud.txt', 'jampingame.txt', 'jampmenus.txt', 'tier1.txt', 'tier2.txt', 'tier3.txt'}

# JD, 23/09 : toute la police passe en blanc. La hierarchie se joue sur
# l'opacite, plus sur la teinte.
GOLD = '1 1 1'
DARK = '0 0 0'

# (motif, remplacement) sur les valeurs de couleur
COLORS = [
    # boites pleines bleues -> noir translucide
    (r'backcolor(\s+)0 0 \.6 \.5', r'backcolor\g<1>' + DARK + ' .55'),
    (r'backcolor(\s+)0 0 \.5 \.25', r'backcolor\g<1>' + DARK + ' .4'),
    (r'backcolor(\s+)0 0 \.35 \.7', r'backcolor\g<1>' + DARK + ' .6'),
    (r'backcolor(\s+)\.015 \.015 \.229 1', r'backcolor\g<1>.02 .02 .03 1'),
    (r'backcolor(\s+)\.66 \.66 1 \.25', r'backcolor\g<1>.6 .5 .3 .2'),
    # bordures bleues -> or fonce
    (r'bordercolor(\s+)0 0 \.8 1', r'bordercolor\g<1>.6 .45 .15 1'),
    (r'bordercolor(\s+)0 0 \.6 1', r'bordercolor\g<1>.6 .45 .15 1'),
    (r'bordercolor(\s+)\.33 \.33 \.5 1', r'bordercolor\g<1>.45 .38 .25 1'),
    (r'bordercolor(\s+)\.66 \.66 1 1', r'bordercolor\g<1>.8 .65 .3 1'),
    (r'bordercolor(\s+)\.403 \.584 \.741 1', r'bordercolor\g<1>.6 .45 .15 1'),
    # titres bleu clair et textes lavande -> blanc
    (r'forecolor(\s+)\.549 \.854 1 1', r'forecolor\g<1>1 1 1 1'),
    (r'forecolor(\s+)\.615 \.615 \.956 1(\.0)?', r'forecolor\g<1>1 1 1 .82'),
    (r'forecolor(\s+)\.615 \.615 \.956 0\.0', r'forecolor\g<1>1 1 1 0.0'),
    (r'forecolor(\s+)\.631 \.631 \.815 1', r'forecolor\g<1>1 1 1 .82'),
    # les couleurs or/creme des passages precedents (main.menu et les menus du
    # mod sont habilles sur place, ils portent encore l'ancienne palette)
    (r'forecolor(\s+)1 \.682 0 1', r'forecolor\g<1>1 1 1 1'),
    (r'forecolor(\s+)\.95 \.85 \.55 1', r'forecolor\g<1>1 1 1 1'),
    (r'forecolor(\s+)\.85 \.82 \.72 1', r'forecolor\g<1>1 1 1 .82'),
    (r'forecolor(\s+)\.6 \.6 \.7 1', r'forecolor\g<1>1 1 1 .6'),
    (r'descColor(\s+)1 \.682 0 \.8', r'descColor\g<1>1 1 1 .75'),
    (r'forecolor(\s+)0\.65 0\.65 1 1', r'forecolor\g<1>' + GOLD + ' 1'),
    (r'forecolor(\s+)\.65 \.65 1 1', r'forecolor\g<1>' + GOLD + ' 1'),
    (r'forecolor(\s+)0 0 1 1', r'forecolor\g<1>' + GOLD + ' 1'),
    (r'focuscolor(\s+)0 0 1 1', r'focuscolor\g<1>1 1 1 1'),
    # surlignage d'une ligne de liste
    (r'outlinecolor(\s+)\.5 \.5 \.5 \.5', r'outlinecolor\g<1>.6 .5 .3 .5'),
]

# main.menu est un menu-colonne comme COOPERATION : entree survolee en noir sur
# la barre blanche (le moteur prend le focusColor DU MENU, pas le forecolor de
# l'item, voir Item_TextColor dans ui_shared.cpp)
MAIN_ONLY = [
    (r'focusColor(\s+)1 1 1 1', r'focusColor\g<1>0 0 0 1'),
    # idempotent : la plaque est posee meme si le fichier est deja en noir
    (r'focusColor(\s+)0 0 0 1(?!\s*\n\s*focusBackColor)',
     'focusColor\\g<1>0 0 0 1\n\t\tfocusBackColor\t\t\t1 1 1 1'),
    (r'(name\s+button_glow\s*\n\s+group\s+mods\s*\n\s+)style\s+WINDOW_STYLE_SHADER(\s*\n\s+rect\s+0 0 0 0\s*\n\s+)background\s+"gfx/menus/menu_buttonback"',
     r'\g<1>style				WINDOW_STYLE_FILLED\g<2>backcolor			1 1 1 1'),
]

# liste des resolutions du menu video : on ajoute les modes 16/9 et 21/9 (sdl_window.cpp) et le bureau (-2)
MODES = (r'cvarFloatList(\s+)\{\s*@MENUS_640_X_480 3 @MENUS_800_X_600 4  @MENUS_1024_X_768 6 @MENUS_1152_X_864 7  '
         r'@MENUS_1280_X_1024 8  @MENUS_1600_X_1200 9  @MENUS_2048_X_1536 10 @MENUS_2400_X_600 12 \}')
MODES_NEW = (r'cvarFloatList\g<1>{ "Bureau (natif)" -2 @MENUS_640_X_480 3 @MENUS_800_X_600 4 @MENUS_1024_X_768 6 '
             r'@MENUS_1152_X_864 7 @MENUS_1280_X_1024 8 @MENUS_1600_X_1200 9 @MENUS_2048_X_1536 10 '
             r'"1280x720" 13 "1366x768" 14 "1600x900" 15 "1920x1080" 16 "2560x1080" 17 "2560x1440" 18 "3440x1440" 19 "3840x2160" 20 }')

# ecrans difficulte / personnage : en coop (ui_coopMode join/char/charstart/hostnew) RETOUR et Echap
# reviennent au salon ou a la page REJOINDRE (uiScript coopCharBack <menu du jeu sinon>), et les
# boutons lateraux du jeu (nouvelle partie, charger, controles, options, quitter) sont caches
COOP_MODES = '{ "join" "char" "charstart" "hostnew" }'
CHAR_PATCHES = {
    'newgame_first.menu': [
        (r'(onESC\s*\n\s*\{(?:(?!itemDef).)*?play\s+"sound/interface/esc.wav"\s*\n)\s*close\s+newgamefirstMenu\s*\n\s*open\s+mainMenu',
         r'\1\t\t\tuiScript\t\t\tcoopCharBack mainMenu'),
        (r'(name\s+backbutton\b(?:(?!itemDef).)*?action\s*\n\s*\{\s*\n\s*play\s+"sound/interface/[a-z0-9]+\.wav"\s*;?\s*\n)\s*close\s+all\s*;\s*\n\s*open\s+mainMenu',
         r'\1\t\t\t\tuiScript\t\tcoopCharBack mainMenu'),
    ],
    'character.menu': [
        (r'(onESC\s*\n\s*\{\s*\n\s*play\s+"sound/interface/esc.wav"\s*\n)\s*close\s+characterMenu\s*\n\s*open\s+newgameMenu',
         r'\1\t\t\tuiScript\t\t\tcoopCharBack newgameMenu'),
        (r'(name\s+backbutton\b(?:(?!itemDef).)*?action\s*\n\s*\{\s*\n\s*play\s+"sound/interface/[a-z0-9]+\.wav"\s*;?\s*\n)\s*close\s+all\s*;\s*\n\s*open\s+newgameMenu',
         r'\1\t\t\t\tuiScript\t\tcoopCharBack newgameMenu'),
    ],
}
# ecran SABRE (ref-ui/ui/saber.menu) : les trois listes de manches du jeu sont ecrites en
# dur (les neuf "single_N" et les cinq "dual_N"), donc un mod de sabres n y apparait jamais.
# On les remplace par des boutons qui ouvrent le navigateur coopSaberHilts (toutes les
# entrees de ext_data/sabers/*.sab), et on ajoute deux boutons vers coopSaberColor
# (couleur de lame exacte en RVB). Tout le reste de l ecran - les six couleurs du jeu, les
# styles, les apercus 3D - ne bouge pas.
HILT_LIST = re.compile(r'\n\t+cvar\s+"(ui_saber2?)"\n\t+//FIXME: read these from sabers\.cfg \+ \*\.sab\?\n'
                       r'\t+cvarStrList\s*\n\t+\{[^}]*\}\n')
HILT_TEXT = re.compile(r'\n\t+text\s+@MENUS_HILT(\d)\n')
HILT_TYPE = re.compile(r'(name\s+hiltbut(?:2|_staves)?\s*\n(?:(?!itemDef)[\s\S])*?)type\s+ITEM_TYPE_MULTI'
                       r'([\s\S]*?)font\s+4\n(\t+)textscale\s+1\n')
HILT_ACTION = re.compile(r'(play\s+"sound/interface/choose_hilt\.wav"\s*\n\t+)uiScript\s+"saber(2?)_hilt"')
RGB_BUTTONS = '''
		// JACoop : couleur de lame exacte (RVB), en plus des six couleurs du jeu
		itemDef
		{
			name				coopRgbGlow
			group				none
			style				WINDOW_STYLE_SHADER
			rect				28 196 200 22
			background			"gfx/menus/menu_buttonback"
			forecolor			1 1 1 1
			visible				0
			decoration
		}
		itemDef
		{
			name				coopRgbBut
			group				none
			text				"COULEUR EXACTE (RVB)"
			descText			"Choisir la couleur de la lame au point pres."
			type				ITEM_TYPE_BUTTON
			style				WINDOW_STYLE_EMPTY
			rect				32 198 200 18
			font				3
			textscale			0.75
			textalign			ITEM_ALIGN_LEFT
			textalignx			0
			textaligny			0
			forecolor			1 1 1 1
			visible				1
			mouseEnter
			{
				show			coopRgbGlow
			}
			mouseExit
			{
				hide			coopRgbGlow
			}
			action
			{
				play			"sound/interface/button1.wav" ;
				setcvar			ui_coopHiltWhich	"1" ;
				open			coopSaberColor
			}
		}
		itemDef
		{
			name				coopRgbBut2
			group				none
			text				"RVB DU SABRE GAUCHE"
			descText			"Couleur exacte de la seconde lame."
			type				ITEM_TYPE_BUTTON
			style				WINDOW_STYLE_EMPTY
			rect				32 220 200 18
			font				3
			textscale			0.75
			textalign			ITEM_ALIGN_LEFT
			textalignx			0
			textaligny			0
			forecolor			1 1 1 1
			cvarTest			ui_saber_type
			showCvar			{ "dual" }
			visible				1
			action
			{
				play			"sound/interface/button1.wav" ;
				setcvar			ui_coopHiltWhich	"2" ;
				open			coopSaberColor
			}
		}
'''


def saber_patch(text):
    # 1. the hardcoded hilt lists -> the cvar that holds the chosen hilt's proper name
    text, n = HILT_LIST.subn(lambda m: '\n\t\t\tcvar\t\t\t\t"%s_name"\n' % m.group(1), text)
    assert n == 3, n
    # 2. no "text" line, or it would win over the cvar (ui_shared.cpp Item_Text_Paint)
    text, n = HILT_TEXT.subn('\n', text)
    assert n == 3, n
    # 3. a button, in a font that fits a mod's long hilt names
    text, n = HILT_TYPE.subn(lambda m: m.group(1) + 'type\t\t\t\tITEM_TYPE_BUTTON' + m.group(2)
                             + 'font\t\t\t\t3\n' + m.group(3) + 'textscale\t\t\t0.7\n', text)
    assert n == 3, n
    # 4. the click opens the browser for that blade instead of cycling
    text, n = HILT_ACTION.subn(lambda m: m.group(1) + 'setcvar\t\t\tui_coopHiltWhich\t"%s" ;\n\t\t\t\topen\t\t\tcoopSaberHilts'
                               % ('2' if m.group(2) else '1'), text)
    assert n == 3, n
    # 5. the two RVB buttons, in the free column under the saber-type buttons
    marker = '//BLADE COLORS'
    assert text.count(marker) == 1
    text = text.replace(marker, RGB_BUTTONS + '//BLADE COLORS')
    return text


SIDE_BUTTONS = ('newgamebutton', 'loadgamebutton', 'controlsbutton', 'setupbutton', 'exitgamebutton',
                'newbutton_glow', 'loadgamebutton_glow', 'controlsbutton_glow', 'setupbutton_glow', 'exitgamebutton_glow',
                'newgamebutton_undertext', 'loadgamebutton_undertext', 'controlsbutton_undertext', 'setupbutton_undertext')

def coop_patch(name, text):
    if name == 'saber.menu':
        text = saber_patch(text)
    for pat, rep in CHAR_PATCHES.get(name, ()):
        text, n = re.subn(pat, rep, text, count=1, flags=re.S)
        assert n == 1, (name, pat[:60])
    if name in CHAR_PATCHES:
        for item in SIDE_BUTTONS:
            # first 'visible' line of that itemDef gets a cvarTest/hideCvar in front of it
            text, n = re.subn(r'(name\s+%s\s*\n(?:(?!itemDef).)*?)(\n\s*visible\s)' % item,
                              lambda m: m.group(1) + '\n\t\t\tcvarTest\t\t\t"ui_coopMode"\n\t\t\thideCvar\t\t\t' + COOP_MODES + m.group(2),
                              text, count=1, flags=re.S)
    return text

def skin(text, name=''):
    for pat, rep in COLORS:
        text = re.sub(pat, rep, text)
    text = re.sub(MODES, MODES_NEW, text)
    return coop_patch(name, text)

def main():
    n = 0
    for src in sorted(glob.glob(os.path.join(SRC, '*'))):
        name = os.path.basename(src)
        if name in OWN or name in SKIP or not (name.endswith('.menu') or name.endswith('.txt')):
            continue
        t = io.open(src, encoding='latin-1').read()
        out = skin(t, name)
        io.open(os.path.join(DST, name), 'w', encoding='latin-1', newline='\n').write(out)
        n += 1
    for name in OWN:
        p = os.path.join(DST, name)
        if not name.endswith('.menu') or not os.path.exists(p):
            continue
        t = io.open(p, encoding='latin-1').read()
        t = skin(t)
        if name == 'main.menu':
            for pat, sub in MAIN_ONLY:
                t = re.sub(pat, sub, t)
        io.open(p, 'w', encoding='latin-1', newline='\n').write(t)
    print('skinned', n, 'game menus + own menus')
    # puis les textes en deux langues (francais / anglais) : localize_menus.py
    import localize_menus
    localize_menus.localize()

if __name__ == '__main__':
    main()
