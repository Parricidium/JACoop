"""JACoop - generateur des menus coop (dist/pk3/ui/coop*.menu), tous dans le style du menu
principal (fond gfx/jacoop/menu_bg, colonne de boutons or avec halo, descriptions en bas).

    python dist/gen_coop_menus.py

Le fond des pages coop est gfx/jacoop/panel_bg (shader jacoop_ui.shader) : le joueur peut
le remplacer par son image (cvar ui_coopPanelBg, voir UI_CoopSkinShader dans ui_shared.cpp).

Ecrit : coop.menu (coopMenu = sous-menu COOPERATION, coopJoinMenu), coopoptions.menu
(coopOptionsMenu), coopingame.menu (coopMenu de la touche F6), cooplobby.menu (coopLobby),
coopskins.menu (coopSkins), coopsaber.menu (coopSaberHilts : tous les sabres installes),
coopsabercolor.menu (coopSaberColor : couleur de lame exacte en RVB). Les verbes uiScript
vivent dans openjk/code/ui/ui_main.cpp.
Encodage cp1252 (police indexee par octet) : pas d'accents dans les textes (choix : ASCII).
"""
import io, os

DST = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'dist', 'pk3', 'ui')
# tout est blanc (JD, 23/09) : ce qui distingue un titre d'un commentaire est
# l'opacite, plus la teinte
GOLD = '1 1 1 1'        # texte principal, boutons
CREAM = '1 1 1 .82'     # texte courant
GREY = '1 1 1 .6'       # texte secondaire
TITLE = '1 1 1 1'       # titres
SELECT = '0 0 0 1'      # texte de l'entree survolee, sur la barre blanche


def header(name, esc, on_open='', full=1, desc_y=440, focus='1 1 1 1'):
    # une couleur de focus sombre a besoin d'une plaque claire derriere l'item,
    # sinon l'entree selectionnee au clavier est noire sur noir (focusBackColor
    # est a nous, voir Item_Paint dans ui_shared.cpp)
    back = '\n\t\tfocusBackColor			1 1 1 1' if focus == SELECT else ''
    if not on_open:
        on_open = '\t\t\tplay\t\t\t\t"sound/interface/transition.wav"\n'
    return '''	menuDef
	{
		name					"%s"
		fullScreen				%d
		rect					0 0 640 480
		visible					1
		focusColor				%s%s
		descX					320
		descY					%d
		descScale				1
		descColor				1 1 1 .75
		descAlignment			ITEM_ALIGN_CENTER
		onOpen
		{
%s		}
		onESC
		{
			play				"sound/interface/menuroam.wav" ;
%s		}
''' % (name, full, focus, back, desc_y, on_open, esc)


def decor(subtitle=None, logo=True):
    s = '''
		// --- decor ------------------------------------------------------
		itemDef
		{
			name				background
			group				grp_background
			style				WINDOW_STYLE_SHADER
			rect				0 0 640 480
			background			"gfx/jacoop/panel_bg"
			forecolor			1 1 1 1
			visible				1
			decoration
		}
'''
    if logo:
        s += '''		itemDef
		{
			name				starwars
			group				grp_background
			style				WINDOW_STYLE_SHADER
			rect				150 6 340 89
			background			"gfx/menus/jediacademy"
			forecolor			1 1 1 1
			visible				1
			decoration
		}
'''
    if subtitle:
        s += '''		itemDef
		{
			name				subtitle
			style				WINDOW_STYLE_EMPTY
			rect				150 98 340 16
			text				"%s"
			font				3
			textscale			0.5
			textalign			ITEM_ALIGN_CENTER
			textalignx			170
			forecolor			0.6 0.6 0.75 1
			visible				1
			decoration
		}
''' % subtitle
    s += '''		itemDef
		{
			name				button_glow
			group				mods
			style				WINDOW_STYLE_FILLED
			rect				0 0 0 0
			backcolor			1 1 1 1
			forecolor			1 1 1 1
			visible				0
			decoration
		}
'''
    return s


def title(text, y=28):
    return '''		itemDef
		{
			name				title
			style				WINDOW_STYLE_EMPTY
			rect				0 %d 640 30
			text				"%s"
			font				3
			textscale			1.2
			textalign			ITEM_ALIGN_CENTER
			textalignx			320
			forecolor			%s
			visible				1
			decoration
		}
''' % (y, text, TITLE)


def colbutton(name, y, text, action, desc='', cvartest=None, show=None, hide=None, x=50, w=220):
    """bouton de colonne avec halo, comme main.menu"""
    cv = ''
    if cvartest:
        cv = '			cvarTest			"%s"\n' % cvartest
        if show is not None:
            cv += '			showCvar			{ %s }\n' % ' '.join('"%s"' % v for v in show)
        if hide is not None:
            cv += '			hideCvar			{ %s }\n' % ' '.join('"%s"' % v for v in hide)
    return '''		itemDef
		{
			name				%s
			group				column
			style				WINDOW_STYLE_EMPTY
			type				ITEM_TYPE_BUTTON
			rect				%d %d %d 26
			text				"%s"
			descText			"%s"
			font				3
			textscale			1.0
			textaligny			0
			textalign			ITEM_ALIGN_LEFT
			textstyle			1
			textalignx			8
			forecolor			%s
%s			visible				1
			mouseEnter
			{
				show			button_glow
				setitemrect		button_glow	%d %d %d 32
			}
			mouseExit
			{
				hide			button_glow
			}
			action
			{
				play			"sound/interface/button1.wav" ;
%s			}
		}
''' % (name, x, y, w, text, desc, GOLD, cv, x - 20, y - 3, w + 40, action)


def button(name, rect, text, action, desc='', align='LEFT', cvartest=None, show=None, hide=None, scale=0.9, color=GOLD, cvar=None):
    """petit bouton texte (sans halo) ; cvar = le libelle vient d une cvar"""
    cv = ''
    if cvartest:
        cv = '			cvarTest			"%s"\n' % cvartest
        if show is not None:
            cv += '			showCvar			{ %s }\n' % ' '.join('"%s"' % v for v in show)
        if hide is not None:
            cv += '			hideCvar			{ %s }\n' % ' '.join('"%s"' % v for v in hide)
    x, y, w, h = rect
    ax = {'LEFT': 0, 'CENTER': w // 2, 'RIGHT': w}[align]
    txt = '			cvar				"%s"\n' % cvar if cvar else '			text				"%s"\n' % text
    return '''		itemDef
		{
			name				%s
			type				ITEM_TYPE_BUTTON
			style				WINDOW_STYLE_EMPTY
			rect				%d %d %d %d
%s			descText			"%s"
			font				3
			textscale			%s
			textalign			ITEM_ALIGN_%s
			textalignx			%d
			forecolor			%s
%s			visible				1
			action
			{
				play			"sound/interface/button1.wav" ;
%s			}
		}
''' % (name, x, y, w, h, txt, desc, scale, align, ax, color, cv, action)


def label(name, rect, text, scale=0.7, color=CREAM, align='LEFT', font=3, cvartest=None, show=None, hide=None, cvar=None):
    cv = ''
    if cvartest:
        cv = '			cvarTest			"%s"\n' % cvartest
        if show is not None:
            cv += '			showCvar			{ %s }\n' % ' '.join('"%s"' % v for v in show)
        if hide is not None:
            cv += '			hideCvar			{ %s }\n' % ' '.join('"%s"' % v for v in hide)
    x, y, w, h = rect
    ax = {'LEFT': 0, 'CENTER': w // 2, 'RIGHT': w}[align]
    txt = '			cvar				"%s"\n' % cvar if cvar else '			text				"%s"\n' % text
    return '''		itemDef
		{
			name				%s
			style				WINDOW_STYLE_EMPTY
			rect				%d %d %d %d
%s			font				%d
			textscale			%s
			textalign			ITEM_ALIGN_%s
			textalignx			%d
			forecolor			%s
%s			visible				1
			decoration
		}
''' % (name, x, y, w, h, txt, font, scale, align, ax, color, cv)


def box(name, rect, alpha=0.55):
    x, y, w, h = rect
    return '''		itemDef
		{
			name				%s
			style				WINDOW_STYLE_FILLED
			rect				%d %d %d %d
			backcolor			0 0 0 %s
			border				1
			bordercolor			.45 .38 .25 1
			visible				1
			decoration
		}
''' % (name, x, y, w, h, alpha)


def listbox(name, rect, feeder, elem_h=16, scale=0.6, action='', cvartest=None, show=None, hide=None):
    x, y, w, h = rect
    cv = ''
    if cvartest:
        cv = '			cvarTest			"%s"\n' % cvartest
        if show is not None:
            cv += '			showCvar			{ %s }\n' % ' '.join('"%s"' % v for v in show)
        if hide is not None:
            cv += '			hideCvar			{ %s }\n' % ' '.join('"%s"' % v for v in hide)
    act = ''
    if action:
        act = '			doubleClick\n			{\n%s			}\n' % action
    return '''		itemDef
		{
			name				%s
			type				ITEM_TYPE_LISTBOX
			style				WINDOW_STYLE_FILLED
			rect				%d %d %d %d
			feeder				%s
			elementwidth		%d
			elementheight		%d
			elementtype			LISTBOX_TEXT
			textscale			%s
			forecolor			%s
			backcolor			0 0 0 .45
			outlinecolor		.6 .5 .3 .5
			border				1
			bordercolor			.45 .38 .25 1
%s			visible				1
%s		}
''' % (name, x, y, w, h, feeder, w, elem_h, scale, CREAM, cv, act)


def editfield(name, rect, text, cvar, maxchars, desc='', maxpaint=None):
    x, y, w, h = rect
    mp = maxpaint or maxchars
    return '''		itemDef
		{
			name				%s
			type				ITEM_TYPE_EDITFIELD
			style				WINDOW_STYLE_EMPTY
			rect				%d %d %d %d
			text				"%s"
			descText			"%s"
			cvar				"%s"
			maxChars			%d
			maxPaintChars		%d
			font				3
			textscale			0.75
			textalign			ITEM_ALIGN_LEFT
			textalignx			0
			forecolor			%s
			visible				1
		}
''' % (name, x, y, w, h, text, desc, cvar, maxchars, mp, CREAM)


def multi(name, rect, text, cvar, choices, desc='', cvartest=None, show=None, hide=None):
    """choix cyclique : choices = [(libelle, valeur), ...] (valeurs numeriques -> cvarFloatList)"""
    x, y, w, h = rect
    cv = ''
    if cvartest:
        cv = '			cvarTest			"%s"\n' % cvartest
        if show is not None:
            cv += '			showCvar			{ %s }\n' % ' '.join('"%s"' % v for v in show)
        if hide is not None:
            cv += '			hideCvar			{ %s }\n' % ' '.join('"%s"' % v for v in hide)
    lst = ' '.join('"%s" %s' % (l, v) for l, v in choices)
    return '''		itemDef
		{
			name				%s
			type				ITEM_TYPE_MULTI
			style				WINDOW_STYLE_EMPTY
			rect				%d %d %d %d
			text				"%s"
			descText			"%s"
			cvar				"%s"
			cvarFloatList		{ %s }
			font				3
			textscale			0.75
			textalign			ITEM_ALIGN_LEFT
			textalignx			0
			forecolor			%s
%s			visible				1
			action
			{
				play			"sound/interface/button1.wav"
			}
		}
''' % (name, x, y, w, h, text, desc, cvar, lst, CREAM, cv)


def multistr(name, rect, text, cvar, choices, desc=''):
    """choix cyclique sur une cvar texte (cvarStrList)"""
    x, y, w, h = rect
    lst = ' '.join('"%s" "%s"' % (l, v) for l, v in choices)
    return '''		itemDef
		{
			name				%s
			type				ITEM_TYPE_MULTI
			style				WINDOW_STYLE_EMPTY
			rect				%d %d %d %d
			text				"%s"
			descText			"%s"
			cvar				"%s"
			cvarStrList			{ %s }
			font				3
			textscale			0.75
			textalign			ITEM_ALIGN_LEFT
			textalignx			0
			forecolor			%s
			visible				1
			action
			{
				play			"sound/interface/button1.wav"
			}
		}
''' % (name, x, y, w, h, text, desc, cvar, lst, CREAM)


def slider(name, rect, text, cvar, lo, hi, default, desc=''):
    """curseur 0-255 (ITEM_TYPE_SLIDER ecrit directement le cvar pendant le glissement)"""
    x, y, w, h = rect
    return '''		itemDef
		{
			name				%s
			type				ITEM_TYPE_SLIDER
			style				WINDOW_STYLE_EMPTY
			rect				%d %d %d %d
			text				"%s"
			descText			"%s"
			cvarfloat			"%s" %s %s %s
			font				3
			textscale			0.75
			textalign			ITEM_ALIGN_RIGHT
			textalignx			90
			textaligny			0
			forecolor			%s
			visible				1
		}
''' % (name, x, y, w, h, text, desc, cvar, default, lo, hi, CREAM)


def ownerdraw(name, rect, num, comment=''):
    x, y, w, h = rect
    return '''		itemDef
		{
			name				%s
			style				WINDOW_STYLE_EMPTY
			rect				%d %d %d %d
			ownerdraw			%s						// %s
			font				3
			textscale			0.6
			forecolor			%s
			visible				1
			decoration
		}
''' % (name, x, y, w, h, num, comment, CREAM)


def saber_preview(rect, second=False, name=None, cvar=None, hide=None, show=None):
    """apercu 3D du sabre : memes reglages que l ecran du jeu (ref-ui/ui/saber.menu),
    les lames se dessinent a partir de ui_saber / ui_saber_color (drapeau isSaber).
    L ecran du jeu lui donne 615x615 : une fenetre plus petite montre la meme scene
    en plus petit (meme champ de vision), ce qui remet la lame dans son coin.
    C est le drapeau isSaber / isSaber2 qui decide de la lame dessinee, pas le nom
    de l item : on peut donc en poser plusieurs (voir l ecran SABRE LASER, qui
    montre les deux manches cote a cote en mode deux sabres)."""
    x, y, w, h = rect
    if cvar is None:
        # par defaut : le navigateur ne montre que le manche qu il fait choisir
        cvar, hide = 'ui_coopHiltWhich', '1' if second else '2'
    gate = '\t\t\tcvarTest\t\t\t%s\n' % cvar
    gate += '\t\t\t%sCvar\t\t\t{ "%s" }\n' % ('hide', hide) if hide else '\t\t\tshowCvar\t\t\t{ "%s" }\n' % show
    return '''		itemDef
		{
			name				%s
			group				models
			type				ITEM_TYPE_MODEL
			rect				%d %d %d %d
			asset_model			"models/weapons2/saber_1/saber_1.glm"
			%s			1
			model_angle			180
			model_rotation		20
			model_g2mins		0 0 0
			model_g2maxs		20 20 20
			model_fovx			75
			model_fovy			75
%s			visible				1
			decoration
		}
''' % (name or ('saber2' if second else 'saber'), x, y, w, h,
       'isSaber2' if second else 'isSaber ', gate)


def bind(name, rect, text, cmd, desc=''):
    x, y, w, h = rect
    return '''		itemDef
		{
			name				%s
			type				ITEM_TYPE_BIND
			style				WINDOW_STYLE_EMPTY
			rect				%d %d %d %d
			text				"%s"
			descText			"%s"
			cvar				"%s"
			font				3
			textscale			0.75
			textalign			ITEM_ALIGN_LEFT
			textalignx			0
			forecolor			%s
			visible				1
		}
''' % (name, x, y, w, h, text, desc, cmd, GOLD)


def write(name, text):
    p = os.path.join(DST, name)
    io.open(p, 'w', encoding='cp1252', newline='\n').write(text)
    print('wrote', name)


# ======================================================================= coop.menu
coop_menu = '''// JACoop - COOPERATION (sous-menu du menu principal) et REJOINDRE UNE PARTIE.
// Genere par dist/gen_coop_menus.py : ne pas editer a la main.
{
''' + header('coopMenu', '			close				all ;\n			open				mainMenu\n',
             '			setfocus			hostButton\n', focus=SELECT) + decor() \
    + colbutton('hostButton', 160, 'HEBERGER', '				uiScript		coopCreate\n',
                'Ouvre le salon : tes amis te rejoignent, puis NOUVELLE PARTIE ou CONTINUER.') \
    + colbutton('joinButton', 200, 'REJOINDRE', '				close			all ;\n				open			coopJoinMenu\n',
                'Rejoindre la partie d un ami (reseau local ou adresse Internet).') \
    + colbutton('optionsButton', 240, 'OPTIONS COOPERATION', '				setcvar			ui_coopOptionsFrom main ;\n				close			all ;\n				open			coopOptionsMenu\n',
                'Pseudo, joueurs max, touche pour relever un coequipier, joueurs a terre...') \
    + colbutton('backButton', 296, 'RETOUR', '				close			all ;\n				open			mainMenu\n', 'Retour au menu principal.') \
    + '''	}

''' + header('coopJoinMenu', '			close				all ;\n			open				coopMenu\n',
             '			uiScript			coopRefresh ;\n			setfocus			serverList\n') + decor(logo=False) \
    + title('REJOINDRE UNE PARTIE') \
    + label('lanLabel', (60, 80, 400, 20), 'PARTIES SUR LE RESEAU LOCAL', 0.8, GOLD) \
    + listbox('serverList', (60, 104, 400, 150), '0x18', 18, 0.65, '				uiScript		coopJoin\n') \
    + button('refreshButton', (480, 104, 130, 24), 'ACTUALISER', '				uiScript		coopRefresh\n', 'Rechercher a nouveau les parties du reseau local.') \
    + button('joinButton', (480, 134, 130, 24), 'REJOINDRE', '				uiScript		coopJoin\n', 'Rejoindre la partie selectionnee.') \
    + label('hint', (60, 258, 400, 16), 'Double-clic sur une partie pour la rejoindre.', 0.55, GREY) \
    + label('directLabel', (60, 300, 400, 20), 'ADRESSE DIRECTE (IP:PORT)', 0.8, GOLD) \
    + editfield('addressField', (60, 324, 320, 20), 'Adresse :', 'ui_coopAddress', 48, 'L adresse de l hote, par exemple 192.168.1.10:29070.', 30) \
    + button('connectButton', (400, 322, 150, 24), 'CONNEXION', '				uiScript		coopConnect\n', 'Se connecter a cette adresse.') \
    + label('hint2', (60, 350, 520, 16), 'Exemple : 192.168.1.10:29070 - le port par defaut est 29070.', 0.55, GREY) \
    + label('hint3', (60, 380, 520, 16), 'Tu arrives dans le salon de l hote ; ton personnage se cree quand il lance la partie.', 0.55, GREY) \
    + button('backButton', (480, 440, 130, 24), 'RETOUR', '				close			all ;\n				open			coopMenu\n', 'Retour.', 'RIGHT') \
    + '''	}
}
'''
write('coop.menu', coop_menu)

# ======================================================================= coopoptions.menu
# OPTIONS COOPERATION dans la presentation de l'ecran Config officiel (setup.menu) :
# une barre de titre, des onglets a gauche (halo au survol, comme les siens), et un
# cadre a droite avec une ligne par option, surlignee au survol. L'onglet ouvert est la
# cvar ui_coopOptionsTab (archivee : retenu d'une fois sur l'autre) ; chaque ligne du
# cadre n'est peinte que pour le sien (cvarTest), le halo de l'onglet actif aussi.
# Le RENDU MODERNE est un onglet comme les autres. JD, 24/09.
PX, PY, PW, PH = 260, 130, 340, 294     # le cadre de droite
PROWS = 21                              # lignes de 14 px, comme l'original
TX, TY0, TH = 80, 130, 24               # les onglets
MOD = 'r_modern'

TABS = [('tabPlayer', 'JOUEUR', 'player', 'Pseudo, touche pour relever, camera, champ de vision.'),
        ('tabHost', 'HOTE', 'host', 'Reglages de la partie : ne comptent que pour celui qui heberge.'),
        ('tabImages', 'IMAGES DE MENU', 'images', 'Tes propres images de fond et de logo.'),
        ('tabRender', 'RENDU MODERNE', 'render', 'Ombres du soleil, occlusion ambiante, rayons. Tout est eteint par defaut.'),
        ('tabRT', 'RAY TRACING', 'rt', 'Ombres, occlusion, lumieres et reflets calcules par rayons (GL 4.3).')]
GROUPS = [g for _, _, g, _ in TABS]
HIDDEN_TABS = ('images',)	# garde son numero, n'est plus montre


def _cvshow(cvartest, show=None, hide=None):
    if not cvartest:
        return ''
    cv = '\t\t\tcvarTest\t\t\t"%s"\n' % cvartest
    if show is not None:
        cv += '\t\t\tshowCvar\t\t\t{ %s }\n' % ' '.join('"%s"' % v for v in show)
    if hide is not None:
        cv += '\t\t\thideCvar\t\t\t{ %s }\n' % ' '.join('"%s"' % v for v in hide)
    return cv


def prow_y(row):
    return PY + 3 + 14 * row


TABCVAR = 'ui_coopOptionsTab'


def _prow_head(name, group, row, itype, text, desc, color=CREAM, cvartest=None, show=None, hide=None, align='RIGHT', ax=190):
    '''le debut commun d'une ligne du cadre : police 4, libelle aligne a droite sur la colonne
    des deux-points. La ligne n'est peinte que si son onglet est celui de ui_coopOptionsTab.'''
    cvartest, show, hide = TABCVAR, [str(GROUPS.index(group))], None
    return ('\t\titemDef\n\t\t{\n'
            + '\t\t\tname\t\t\t\t%s\n\t\t\tgroup\t\t\t\t%s\n' % (name, group)
            + ('\t\t\ttype\t\t\t\t%s\n' % itype if itype else '')
            + '\t\t\tstyle\t\t\t\tWINDOW_STYLE_EMPTY\n'
            + '\t\t\trect\t\t\t\t%d %d %d 14\n' % (PX, prow_y(row), PW)
            + '\t\t\ttext\t\t\t\t"%s"\n' % text
            + ('\t\t\tdescText\t\t\t"%s"\n' % desc if desc else '')
            + '\t\t\tfont\t\t\t\t4\n\t\t\ttextscale\t\t\t1\n'
            + '\t\t\ttextalign\t\t\tITEM_ALIGN_%s\n\t\t\ttextalignx\t\t\t%d\n\t\t\ttextaligny\t\t\t0\n' % (align, ax)
            + '\t\t\tforecolor\t\t\t%s\n' % color
            + _cvshow(cvartest, show, hide)
            + '\t\t\tvisible\t\t\t\t1\n')


def _prow_hover(row):
    return ('\t\t\tmouseEnter\n\t\t\t{\n\t\t\t\tshow\t\t\thighlight%d\n\t\t\t}\n'
            '\t\t\tmouseExit\n\t\t\t{\n\t\t\t\thide\t\t\thighlight%d\n\t\t\t}\n' % (row, row))


def prow_multi(name, group, row, text, cvar, choices, desc='', cvartest=None, show=None, hide=None):
    lst = ' '.join('"%s" %s' % (l, v) for l, v in choices)
    return (_prow_head(name, group, row, 'ITEM_TYPE_MULTI', text, desc, CREAM, cvartest, show, hide)
            + '\t\t\tcvar\t\t\t\t"%s"\n\t\t\tcvarFloatList\t\t{ %s }\n' % (cvar, lst)
            + _prow_hover(row)
            + '\t\t\taction\n\t\t\t{\n\t\t\t\tplay\t\t\t"sound/interface/button1.wav"\n\t\t\t}\n\t\t}\n')


def prow_bind(name, group, row, text, cmd, desc=''):
    return (_prow_head(name, group, row, 'ITEM_TYPE_BIND', text, desc, CREAM)
            + '\t\t\tcvar\t\t\t\t"%s"\n' % cmd + _prow_hover(row) + '\t\t}\n')


def prow_edit(name, group, row, text, cvar, maxchars, desc=''):
    return (_prow_head(name, group, row, 'ITEM_TYPE_EDITFIELD', text, desc, CREAM)
            + '\t\t\tcvar\t\t\t\t"%s"\n\t\t\tmaxChars\t\t\t%d\n\t\t\tmaxPaintChars\t\t%d\n' % (cvar, maxchars, maxchars)
            + _prow_hover(row) + '\t\t}\n')


def prow_label(name, group, row, text, color=GOLD, align='RIGHT', ax=190, cvartest=None, show=None):
    return _prow_head(name, group, row, '', text, '', color, cvartest, show, None, align, ax) + '\t\t\tdecoration\n\t\t}\n'


def prow_info(name, group, row, text):
    '''une ligne d'explication, en gris et a gauche (34 caracteres au plus)'''
    return prow_label(name, group, row, text, GREY, 'LEFT', 10)


def prow_button(name, group, row, text, action, desc=''):
    return (_prow_head(name, group, row, 'ITEM_TYPE_BUTTON', text, desc, GOLD, None, None, None, 'CENTER', PW // 2)
            + _prow_hover(row)
            + '\t\t\taction\n\t\t\t{\n\t\t\t\tplay\t\t\t"sound/interface/button1.wav" ;\n%s\t\t\t}\n\t\t}\n' % action)


def tab(idx, name, text, group, desc, slot=None):
    '''un onglet : montre son groupe, cache les autres ; le halo suit la souris, l'onglet actif est en blanc plein'''
    y = TY0 + TH * (idx if slot is None else slot)	# slot : sa place a l'ecran quand un onglet est cache
    acts = '\t\t\t\tsetcvar\t\t\t%s %d\n' % (TABCVAR, idx)
    cols = ''
    return '''		itemDef
		{
			name				%sActive
			style				WINDOW_STYLE_SHADER
			rect				%d %d 220 26
			background			"gfx/menus/menu_blendbox2"
			forecolor			1 1 1 1
			cvarTest			"%s"
			showCvar			{ "%d" }
			visible				1
			decoration
		}
		itemDef
		{
			name				%s
			group				tabs
			text				"%s"
			type				ITEM_TYPE_BUTTON
			style				WINDOW_STYLE_EMPTY
			rect				%d %d 170 24
			font				3
			textscale			0.9
			textalignx			170
			textaligny			2
			textstyle			1
			textalign			ITEM_ALIGN_RIGHT
			forecolor			1 1 1 1
			descText			"%s"
			visible				1
			mouseEnter
			{
				show			sidebutton_glow
				setitemrect		sidebutton_glow	%d %d 220 26
			}
			mouseExit
			{
				hide			sidebutton_glow
			}
			action
			{
				play			"sound/interface/sub_select" ;
				hide			highlights ;
%s%s			}
		}
''' % (name, TX - 40, y - 1, TABCVAR, idx, name, text, TX, y, desc, TX - 40, y - 1, acts, cols)


def tabs_frame(title_text, title_y):
    '''la barre de titre, le halo des onglets, le cadre de droite et ses lignes de surlignage'''
    s = '''		itemDef
		{
			name				optTitle
			style				WINDOW_STYLE_SHADER
			background			"gfx/menus/menu_blendbox"
			text				"%s"
			rect				100 %d 440 16
			font				3
			textscale			0.7
			textalign			ITEM_ALIGN_CENTER
			textalignx			225
			textaligny			-1
			forecolor			1 1 1 1
			visible				1
			decoration
		}
		itemDef
		{
			name				sidebutton_glow
			group				mods
			style				WINDOW_STYLE_SHADER
			rect				%d %d 220 26
			background			"gfx/menus/menu_blendbox2"
			forecolor			1 1 1 1
			visible				0
			decoration
		}
		itemDef
		{
			name				optPanel
			style				WINDOW_STYLE_FILLED
			rect				%d %d %d %d
			backcolor			0 0 0 .55
			forecolor			1 1 1 1
			border				1
			bordercolor			.6 .45 .15 1
			visible				1
			decoration
		}
''' % (title_text, title_y, TX - 40, TY0 - 1, PX, PY, PW, PH)
    for row in range(PROWS):
        s += '''		itemDef
		{
			name				highlight%d
			group				highlights
			style				WINDOW_STYLE_SHADER
			rect				%d %d %d 14
			background			"gfx/menus/menu_blendbox"
			forecolor			1 1 1 1
			visible				0
			decoration
		}
''' % (row, PX, prow_y(row) + 2, PW)
    return s


opt_open = '\t\t\tplay\t\t\t\t"sound/interface/transition.wav" ;\n\t\t\thide\t\t\t\thighlights ;\n\t\t\thide\t\t\t\tsidebutton_glow ;\n'

opt = '''// JACoop - OPTIONS COOPERATION (charge au menu principal et en jeu : menus.txt + ingame.txt).
// Genere par dist/gen_coop_menus.py : ne pas editer a la main.
// Presentation de l'ecran Config officiel : onglets a gauche, options dans le cadre a droite.
// ui_coopOptionsFrom (main / lobby / game) dit ou revenir (uiScript coopOptionsBack).
{
''' + header('coopOptionsMenu', '			uiScript			coopOptionsBack\n', opt_open, 1, 440) + decor(logo=True) \
    + tabs_frame('OPTIONS COOPERATION', 104) \
    + ''.join(tab(i, n, t, g, d, k) for k, (i, (n, t, g, d)) in enumerate(
        [(i, x) for i, x in enumerate(TABS) if x[2] not in HIDDEN_TABS]))
# --- JOUEUR
opt += prow_edit('nameField', 'player', 0, 'Pseudo :', 'name', 15, 'Ton nom, vu par les autres joueurs (salon, messages).') \
    + prow_bind('reviveBind', 'player', 1, 'Relever (touche) :', '+coop_revive', 'Touche a maintenir pres d un coequipier a terre pour le relever (G par defaut).') \
    + prow_multi('cameraLag', 'player', 2, 'Lissage camera :', 'cg_coopCameraLag',
                 [('Aucun', 0), ('Leger', 50), ('Normal', 100), ('Fort', 200)], 'Retard de la camera des cinematiques chez les invites (contre les a-coups reseau).') \
    + prow_multi('fov', 'player', 3, 'Champ de vision :', 'cg_fov',
                 [('80 (normal)', 80), ('90', 90), ('100', 100), ('110', 110), ('120', 120)],
                 'Valeur de reference en 4/3 : sur un ecran large elle est elargie toute seule, tu n as rien a compenser.')
# --- HOTE
opt += prow_multi('maxPlayers', 'host', 0, 'Joueurs max :', 'ui_coopMaxPlayers', [('2', 2), ('3', 3), ('4', 4)], 'Nombre de places dans la partie.') \
    + prow_multi('requireReady', 'host', 1, 'Lancement :', 'g_coopRequireReady',
                 [('Persos valides', 1), ('Immediat', 0)], 'NOUVELLE PARTIE attend que chaque invite ait valide son personnage.') \
    + prow_multi('downed', 'host', 2, 'Joueurs a terre :', 'g_coopDowned', [('Oui (relever)', 1), ('Non', 0)],
                 'Un coup mortel met a terre (un coequipier releve) au lieu de tuer. Non : reapparition directe.') \
    + prow_multi('bleedOut', 'host', 3, 'Temps a terre :', 'g_coopBleedOut',
                 [('30 s', 30), ('60 s', 60), ('90 s', 90), ('120 s', 120)], 'Delai pour venir relever un coequipier avant qu il meure.') \
    + prow_multi('reviveTime', 'host', 4, 'Duree pour relever :', 'g_coopReviveTime', [('2 s', 2), ('3 s', 3), ('5 s', 5)], 'Temps de maintien de la touche.') \
    + prow_multi('respawnDelay', 'host', 5, 'Reapparition :', 'g_coopRespawnDelay',
                 [('5 s', 5), ('10 s', 10), ('20 s', 20)], 'Delai avant de revenir a cote d un coequipier debout.') \
    + prow_multi('allDownAuto', 'host', 6, 'Rechargement auto :', 'g_coopAllDownAuto',
                 [('Jamais', 0), ('20 s', 20), ('40 s', 40)], 'Tous a terre : le dernier point de controle est recharge tout seul apres ce delai.') \
    + prow_multi('friendlyFire', 'host', 7, 'Tirs entre joueurs :', 'g_coopFriendlyFire',
                 [('Non', 0), ('Oui', 1), ('Moitie', 2)], 'Les joueurs peuvent se blesser et se tuer entre eux.') \
    + prow_multi('transferRate', 'host', 8, 'Debit d envoi :', 'sv_coopTransferRate',
                 [('256 Ko/s', 256), ('512 Ko/s', 512), ('1 Mo/s', 1024), ('2 Mo/s', 2048), ('5 Mo/s', 5120),
                  ('10 Mo/s', 10240), ('25 Mo/s', 25600), ('50 Mo/s', 51200), ('100 Mo/s', 102400), ('Illimite', 0)],
                 'Vitesse d envoi de tes pk3 aux invites. Au-dela de quelques Mo/s le reseau du jeu plafonne de lui-meme.') \
    + prow_multi('ragdoll', 'host', 9, 'Ragdoll des corps :', 'g_coopRagdoll', [('Non', 0), ('Oui', 1), ('Des la mort', 2)],
                 'Les PNJ morts tombent en physique quand on les pousse, tire ou qu ils volent ; Des la mort : a chaque mort.') \
    + prow_multi('ragdollSync', 'host', 10, 'Sync ragdoll :', 'g_coopRagdollSync', [('Non', 0), ('Oui', 1)],
                 'Envoie la pose des corps aux invites (20 fois par seconde, ~250 octets par corps) : la meme chez tous.')
# --- IMAGES DE MENU : onglet cache (JD, 24/09) ; ui_coopCustomSkin reste a 1, les images
# posees dans base/gfx/jacoop/ passent toujours devant celles du mod.
# --- RENDU MODERNE : une couche AJOUTEE ; eteinte, le jeu dessine exactement comme
# avant. Chaque effet garde son interrupteur, pour juger l'un sans l'autre.
opt += prow_multi('modern', 'render', 0, 'Rendu moderne :', 'r_modern', [('Origine', 0), ('Active', 1)],
                  'Eteint, le jeu dessine exactement comme avant : aucun tampon cree, aucun shader compile. Purement local.') \
    + prow_label('aoLabel', 'render', 2, 'OCCLUSION AMBIANTE', GOLD, cvartest=MOD, show=['1']) \
    + prow_multi('ao', 'render', 3, 'Occlusion ambiante :', 'r_modernAO', [('Non', 0), ('Oui', 1)],
                 'Assombrit les creux et les recoins. Se voit surtout en interieur.', cvartest=MOD, show=['1']) \
    + prow_multi('aoInt', 'render', 4, 'Intensite :', 'r_modernAOIntensity',
                 [('Discrete', 0.4), ('Normale', 0.8), ('Forte', 1.2), ('Tres forte', 1.6)],
                 'Force de l assombrissement.', cvartest=MOD, show=['1']) \
    + prow_multi('aoRad', 'render', 5, 'Rayon :', 'r_modernAORadius',
                 [('Serre (16)', 16), ('Normal (28)', 28), ('Large (48)', 48), ('Tres large (96)', 96)],
                 'Distance de recherche, en unites du jeu (un joueur en fait 64 de haut).', cvartest=MOD, show=['1']) \
    + prow_label('sunLabel', 'render', 7, 'OMBRES DU SOLEIL', GOLD, cvartest=MOD, show=['1']) \
    + prow_multi('sun', 'render', 8, 'Ombres du soleil :', 'r_modernSun', [('Non', 0), ('Oui', 1)],
                 'Le decor projette son ombre. Aucune carte du jeu ne declare de soleil : celui du moteur sert.',
                 cvartest=MOD, show=['1']) \
    + prow_multi('sunMap', 'render', 9, 'Ombres hors champ :', 'r_modernSunMap', [('Oui', 1), ('Non', 0)],
                 'Le decor est dessine depuis le soleil : un mur hors du cadre projette aussi, l ombre ne bouge plus avec le regard.',
                 cvartest=MOD, show=['1']) \
    + prow_multi('sunStr', 'render', 10, 'Force des ombres :', 'r_modernSunStrength',
                 [('Legere', 0.25), ('Normale', 0.45), ('Marquee', 0.7)], 'A quel point une ombre assombrit.',
                 cvartest=MOD, show=['1']) \
    + prow_multi('sunLen', 'render', 11, 'Portee hors carte :', 'r_modernSunLength',
                 [('Courte (160)', 160), ('Normale (320)', 320), ('Longue (640)', 640)],
                 'Au-dela de la carte d ombre : jusqu ou une ombre en espace ecran peut s etendre.', cvartest=MOD, show=['1']) \
    + prow_label('rayLabel', 'render', 13, 'RAYONS CREPUSCULAIRES', GOLD, cvartest=MOD, show=['1']) \
    + prow_multi('rays', 'render', 14, 'Rayons du soleil :', 'r_modernRays', [('Non', 0), ('Oui', 1)],
                 'Rais de lumiere. Il faut que le soleil soit dans le cadre, donc surtout en levant les yeux.',
                 cvartest=MOD, show=['1']) \
    + prow_multi('raysStr', 'render', 15, 'Force des rayons :', 'r_modernRaysStrength',
                 [('Discrete', 0.2), ('Normale', 0.35), ('Forte', 0.6)], 'Intensite des rais.',
                 cvartest=MOD, show=['1']) \
    + prow_label('dbgLabel', 'render', 17, 'APERCU', GOLD, cvartest=MOD, show=['1']) \
    + prow_multi('debug', 'render', 18, 'Afficher :', 'r_modernDebug',
                 [('Le jeu', 0), ('Profondeur', 1), ('Occlusion seule', 3), ('Ombre seule', 4), ('Rayons seuls', 5)],
                 'Pour juger un effet isolement. A remettre sur Le jeu pour jouer.', cvartest=MOD, show=['1'])
# --- RAY TRACING : des rayons par pixel dans les shaders. Remplace la carte
# d'ombre et l'occlusion en espace ecran ; les lames de sabre eclairent avec
# leur couleur ; l'ombre volumetrique du jeu est coupee (le trace s'en charge).
opt += prow_multi('rt', 'rt', 0, 'Ray tracing :', 'r_modernRT', [('Non', 0), ('Oui', 1)],
                  'Rayons par pixel : ombres douces exactes, occlusion vraie, lumieres avec ombre, reflets, matieres. Il faut le rendu moderne actif.') \
    + prow_multi('rtScale', 'rt', 1, 'Finesse :', 'r_modernRTScale', [('Quart', 0.5), ('Moitie', 0.7), ('Pleine', 1)],
                 'Resolution de la passe tracee. Quart = 4 fois moins de rayons (image lissee ensuite), Pleine = 4 fois plus lourd.') \
    + prow_label('rtSunLabel', 'rt', 2, 'OMBRES DU SOLEIL', GOLD) \
    + prow_multi('rtSunRays', 'rt', 3, 'Rayons :', 'r_modernRTSunRays', [('2', 2), ('4', 4), ('8', 8)],
                 'Rayons dans la penombre seulement (2 partout ailleurs). Le soleil se coupe dans RENDU MODERNE.') \
    + prow_multi('rtSoft', 'rt', 4, 'Douceur :', 'r_modernRTSoft', [('Nette', 0), ('Douce', 1), ('Tres douce', 2.5)],
                 'Largeur de la penombre.') \
    + prow_label('rtAoLabel', 'rt', 5, 'OCCLUSION TRACEE', GOLD) \
    + prow_multi('rtAoRays', 'rt', 6, 'Rayons :', 'r_modernRTAORays', [('1', 1), ('2', 2), ('4', 4), ('8', 8)],
                 'Chaque rayon coute autant que le soleil : 2 suffisent (l image est lissee). 8 en pleine finesse divise les images par seconde par deux.') \
    + prow_multi('rtAoRange', 'rt', 7, 'Portee :', 'r_modernRTAORange', [('Courte (48)', 48), ('Normale (96)', 96), ('Longue (192)', 192)],
                 'Jusqu ou un obstacle assombrit, en unites du jeu.') \
    + prow_label('rtLightLabel', 'rt', 8, 'LUMIERES ET REFLETS', GOLD) \
    + prow_multi('rtLights', 'rt', 9, 'Lumieres tracees :', 'r_modernRTLights', [('Non', 0), ('Oui', 1)],
                 'Lames de sabre (avec leur couleur), tirs, explosions, nappes de lave : eclairage par pixel avec ombre portee.') \
    + prow_multi('rtLightScale', 'rt', 10, 'Intensite :', 'r_modernRTLightScale', [('Discrete', 0.5), ('Normale', 1), ('Forte', 2)],
                 'Force des lumieres tracees.') \
    + prow_multi('rtReflect', 'rt', 11, 'Reflets :', 'r_modernRTReflect', [('Non', 0), ('Oui', 1)],
                 'Sur les surfaces brillantes du decor (metal, sols polis).') \
    + prow_multi('rtReflStr', 'rt', 12, 'Force des reflets :', 'r_modernRTReflectStrength', [('Discrete', 0.6), ('Normale', 1), ('Forte', 1.6)],
                 'A quel point le reflet se voit (decor, verre, eau).') \
    + prow_multi('rtDynamic', 'rt', 13, 'Persos et portes :', 'r_modernRTDynamic', [('Non', 0), ('Oui', 1)],
                 'Les personnages, portes et ascenseurs projettent et recoivent les ombres.') \
    + prow_label('rtMatLabel', 'rt', 14, 'MATIERES DU DECOR', GOLD) \
    + prow_multi('rtNormals', 'rt', 15, 'Relief des textures :', 'r_modernRTNormals', [('Non', 0), ('Oui', 1)],
                 'Normal maps tirees des textures du decor (pas les personnages) : le relief accroche le soleil et les lumieres.') \
    + prow_multi('rtNormalStr', 'rt', 16, 'Force du relief :', 'r_modernRTNormalStrength', [('Discrete', 0.5), ('Normale', 1), ('Forte', 1.8)],
                 'Profondeur du relief.') \
    + prow_multi('rtWater', 'rt', 17, 'Eau :', 'r_modernRTWater', [('Non', 0), ('Oui', 1)],
                 'Ondulations, refraction de ce qui est dessous, reflet trace dessus, eclat du soleil.') \
    + prow_multi('rtGlass', 'rt', 18, 'Verre :', 'r_modernRTGlass', [('Non', 0), ('Oui', 1)],
                 'Un reflet trace sur les vitres et les surfaces de verre.') \
    + prow_multi('rtLava', 'rt', 19, 'Lave :', 'r_modernRTLava', [('Non', 0), ('Oui', 1)],
                 'La lave ondule, pulse et eclaire alentour (lumieres tracees).')
opt += button('backButton', (480, 456, 130, 24), 'RETOUR', '\t\t\t\tuiScript\t\tcoopOptionsBack\n', 'Retour.', 'RIGHT') \
    + '''	}
}
'''
write('coopoptions.menu', opt)


# ======================================================================= coopingame.menu (F6)
ingame = '''// JACoop - menu COOPERATION en jeu (touche F6 : bind F6 "uimenu coopMenu"), colonne comme le
// menu principal. sv_running 1 = hote (ou solo), 0 = invite ; ui_coopCanHost = solo pas encore ouvert.
// Genere par dist/gen_coop_menus.py : ne pas editer a la main.
{
''' + header('coopMenu', '			uiScript			closeingame\n', '			uiScript			coopLobbyOpen ;\n			setfocus			lobbyButton\n', focus=SELECT) \
    + decor(logo=False) + title('COOPERATION') \
    + colbutton('lobbyButton', 90, 'SALON / JOUEURS', '				close			all ;\n				open			coopLobby\n',
                'Qui est la, qui est pret ; l hote y lance ou charge la partie.') \
    + colbutton('saberButton', 162, 'SABRE LASER', '				setcvar			ui_coopForceFrom game ;\n				close			all ;\n				open			coopSaberPick\n',
                'Type de sabre, manche et couleur de lame - a tout moment.') \
    + colbutton('skinsButton', 126, 'PERSONNAGE', '				setcvar			ui_coopForceFrom game ;\n				close			all ;\n				open			coopSkins\n',
                'Changer de modele ou de skin, tout de suite, meme en jeu.') \
    + colbutton('forceButton', 198, 'POINTS DE FORCE', '				setcvar			ui_coopForceFrom game ;\n				close			all ;\n				open			coopForceSelect\n',
                'Repartir tes points de Force (invites).', 'sv_running', hide=['1']) \
    + colbutton('tpButton', 234, 'REJOINDRE L HOTE', '				uiScript		coopTeleport\n',
                'Etre teleporte a cote de l hote (bloque derriere une porte...).', 'sv_running', hide=['1']) \
    + colbutton('gatherButton', 198, 'RAMENER LES INVITES', '				uiScript		coopGather\n',
                'Teleporter tous les invites a cote de toi.', 'sv_running', show=['1']) \
    + colbutton('hostTpButton', 234, 'ALLER VERS L INVITE', '				uiScript		coopTeleport\n',
                'Etre teleporte a cote de l invite le plus proche.', 'sv_running', show=['1']) \
    + colbutton('hostButton', 270, 'OUVRIR AUX AMIS', '				uiScript		coopHost\n',
                'Ouvrir cette partie solo aux amis : ils apparaissent a cote de toi.', 'ui_coopCanHost', show=['1']) \
    + colbutton('optionsButton', 306, 'OPTIONS COOPERATION', '				setcvar			ui_coopOptionsFrom game ;\n				close			all ;\n				open			coopOptionsMenu\n',
                'Pseudo, touche pour relever, joueurs a terre, joueurs max...') \
    + colbutton('npcButton', 342, 'PNJ', '				setcvar			ui_coopForceFrom game ;\n				close			all ;\n				open			coopNpcMenu\n',
                'Faire apparaitre un personnage devant toi (hote).', cvartest='sv_running', show=['1']) \
    + colbutton('resumeButton', 378, 'RETOUR AU JEU', '				uiScript		closeingame\n', 'Fermer ce menu (F6 le rouvre).') \
    + label('playersLabel', (320, 90, 290, 16), 'JOUEURS', 0.7, GOLD) \
    + listbox('playerList', (320, 110, 290, 110), '0x19', 18, 0.6) \
    + label('stateHost', (320, 228, 290, 16), 'Tu heberges cette partie.', 0.55, GREY, cvartest='sv_running', show=['1']) \
    + label('stateJoin', (320, 228, 290, 16), 'Tu es connecte a l hote.', 0.55, GREY, cvartest='sv_running', hide=['1']) \
    + label('hint1', (320, 300, 290, 16), 'Echap : menu du jeu (sauvegarder, charger,', 0.55, GREY) \
    + label('hint2', (320, 316, 290, 16), 'options, quitter). Un coequipier a terre :', 0.55, GREY) \
    + label('hint3', (320, 332, 290, 16), 'approche-toi et maintiens la touche G.', 0.55, GREY) \
    + '''	}
}
'''
write('coopingame.menu', ingame)

# ======================================================================= cooplobby.menu
ST = 'ui_coopLobbyState'
lobby = '''// JACoop - SALON COOPERATIF (hote et invites). Liste des joueurs : feeder 0x19 (CS_COOP_LOBBY).
// ui_coopLobbyState : host / hoststart (hote), 0 / 1 (invite pas pret / pret), char / chardone
// (invite pendant le lancement : personnage a creer / valide). Genere par dist/gen_coop_menus.py.
{
''' + header('coopLobby', '			uiScript			closeingame\n', '			uiScript			coopLobbyOpen\n') + decor(logo=False) \
    + title('SALON COOPERATIF') \
    + label('playersLabel', (60, 66, 300, 20), 'JOUEURS', 0.8, GOLD) \
    + listbox('playerList', (60, 90, 520, 96), '0x19', 18, 0.7) \
    + label('stHost', (60, 196, 520, 20), 'Tu heberges. Attends tes amis, puis lance ou reprends la partie :', 0.6, CREAM, cvartest=ST, show=['host']) \
    + label('stHostStart', (60, 196, 520, 20), 'Les invites creent leur personnage... la partie demarre quand tous ont valide.', 0.6, CREAM, cvartest=ST, show=['hoststart']) \
    + label('stHostWait', (60, 196, 520, 20), 'Transfert des mods en cours (tes skins vers eux, les leurs vers toi)...', 0.6, '.4 1 .4 1', cvartest=ST, show=['hostwait']) \
    + label('stHostWait2', (60, 226, 520, 20), 'NOUVELLE PARTIE et CONTINUER reviennent quand chacun a fini de telecharger.', 0.6, CREAM, cvartest=ST, show=['hostwait']) \
    + label('stJoinDl', (60, 196, 520, 20), 'Les skins de l hote arrivent : tu seras marque pret automatiquement.', 0.6, CREAM, cvartest=ST, show=['dl']) \
    + label('stJoin', (60, 196, 520, 20), 'L hote lance la partie. Tu peux te declarer pret, ou preparer ton personnage :', 0.6, CREAM, cvartest=ST, show=['0', '1']) \
    + label('stChar', (60, 196, 520, 20), 'L hote a lance la partie : cree ton personnage.', 0.6, CREAM, cvartest=ST, show=['char']) \
    + label('stCharDone', (60, 196, 520, 20), 'Personnage valide - en attente des autres joueurs et de l hote.', 0.6, CREAM, cvartest=ST, show=['chardone']) \
    + button('newGameButton', (60, 226, 220, 24), 'NOUVELLE PARTIE', '				uiScript		coopNewGame\n', 'Difficulte et personnage, puis les invites creent le leur.', cvartest=ST, show=['host']) \
    + button('continueButton', (300, 226, 260, 24), 'CONTINUER (CHARGER)', '				uiScript		coopContinue\n', 'Reprendre une sauvegarde (chacun retrouve son personnage).', cvartest=ST, show=['host']) \
    + button('saberButtonH', (60, 280, 220, 24), 'SABRE LASER', '				setcvar			ui_coopForceFrom lobby ;\n				close			all ;\n				open			coopSaberPick\n', 'Type de sabre, manche et couleur de lame.', cvartest=ST, show=['host', 'hostwait']) \
    + button('skinsButtonH', (60, 256, 220, 24), 'PERSONNAGE', '				setcvar			ui_coopForceFrom lobby ;\n				close			all ;\n				open			coopSkins\n', 'Choisir un modele ou un skin installe.', cvartest=ST, show=['host', 'hostwait']) \
    + button('optionsButtonH', (300, 256, 260, 24), 'OPTIONS COOPERATION', '				setcvar			ui_coopOptionsFrom lobby ;\n				close			all ;\n				open			coopOptionsMenu\n', 'Joueurs a terre, joueurs max, pseudo...', cvartest=ST, show=['host', 'hostwait']) \
    + button('goButton', (60, 226, 220, 24), 'COMMENCER', '				uiScript		coopGo\n', 'Demarrer sans attendre les personnages restants.', cvartest=ST, show=['hoststart']) \
    + button('cancelButton', (300, 226, 260, 24), 'ANNULER', '				uiScript		coopCancelStart\n', 'Revenir au salon.', cvartest=ST, show=['hoststart']) \
    + button('readyButton', (60, 226, 220, 24), 'JE SUIS PRET', '				uiScript		coopReadyToggle\n', 'Dire a l hote que tu es pret.', cvartest=ST, show=['0']) \
    + button('notReadyButton', (60, 226, 220, 24), 'PRET  (annuler)', '				uiScript		coopReadyToggle\n', 'Tu es marque pret ; clique pour annuler.', cvartest=ST, show=['1']) \
    + button('forceButton', (300, 226, 260, 24), 'POINTS DE FORCE', '				setcvar			ui_coopForceFrom lobby ;\n				close			all ;\n				open			coopForceSelect\n', 'Repartir tes points de Force.', cvartest=ST, show=['0', '1', 'chardone', 'dl']) \
    + button('characterButton', (60, 256, 220, 24), 'PERSONNAGE', '				uiScript		coopCharacter\n', 'Choisir ton espece, ton apparence et ton sabre.', cvartest=ST, show=['0', '1', 'dl']) \
    + button('charButton', (60, 226, 220, 24), 'CREER MON PERSONNAGE', '				uiScript		coopCharacter\n', 'Espece, apparence, sabre : la partie demarre quand tous ont valide.', cvartest=ST, show=['char']) \
    + button('charAgainButton', (60, 226, 220, 24), 'PERSONNAGE (modifier)', '				uiScript		coopCharacter\n', 'Revoir ton personnage avant le depart.', cvartest=ST, show=['chardone']) \
    + button('saberButtonJ', (300, 280, 260, 24), 'SABRE LASER', '				setcvar			ui_coopForceFrom lobby ;\n				close			all ;\n				open			coopSaberPick\n', 'Type de sabre, manche et couleur de lame.', cvartest=ST, show=['0', '1', 'chardone', 'dl']) \
    + button('skinsButtonJ', (300, 256, 260, 24), 'PERSONNAGE', '				setcvar			ui_coopForceFrom lobby ;\n				close			all ;\n				open			coopSkins\n', 'Choisir un modele ou un skin installe.', cvartest=ST, show=['0', '1', 'chardone', 'dl']) \
    + label('hint', (60, 300, 520, 16), 'Echap ferme ce salon, F6 le rouvre.', 0.55, GREY) \
    + label('hint2', (60, 316, 520, 16), 'Chaque joueur garde sa progression avec la sauvegarde de l hote.', 0.55, GREY) \
    + label('hint3', (60, 332, 520, 16), 'Un coup mortel met a terre : un coequipier releve avec la touche G (options).', 0.55, GREY) \
    + label('dlLabel', (60, 352, 520, 16), '', 0.55, '.4 1 .4 1', cvartest=ST, hide=['host', 'hostwait', 'hoststart'], cvar='cl_coopTransferStatus') \
    + label('msgLabel', (60, 400, 520, 20), '', 0.6, '1 .4 .4 1', cvar='ui_coopLobbyMsg') \
    + button('leaveButton', (60, 440, 160, 24), 'QUITTER', '				uiScript		coopLeave\n', 'Quitter cette partie (retour au menu principal).') \
    + button('closeButton', (480, 440, 130, 24), 'FERMER', '				uiScript		closeingame\n', 'Fermer le salon (F6 le rouvre).', 'RIGHT') \
    + '''	}
}
'''
write('cooplobby.menu', lobby)

# ======================================================================= coopskins.menu
skins = '''// JACoop - PERSONNAGE / SKIN : tous les modeles de joueur installes (models/players/*, pk3 de
// skins compris) avec leurs variantes, le portrait et un apercu 3D. Ouvert depuis le salon ou le
// menu F6 ; ui_coopForceFrom (lobby/game) dit ou revenir. Genere par dist/gen_coop_menus.py.
{
''' + header('coopSkins', '			uiScript			coopForceBack\n', '			uiScript			coopModelsInit\n', 1, 424) + decor(logo=False) \
    + title('PERSONNAGE / SKIN') \
    + label('modelLabel', (30, 66, 210, 16), 'MODELE', 0.7, GOLD) \
    + listbox('modelList', (30, 86, 210, 324), '0x1a', 16, 0.6) \
    + label('skinLabel', (250, 66, 140, 16), 'VARIANTE', 0.7, GOLD) \
    + listbox('skinList', (250, 86, 140, 100), '0x1b', 16, 0.6) \
    + label('portraitLabel', (250, 196, 140, 16), 'PORTRAIT', 0.7, GOLD) \
    + box('portraitFrame', (250, 216, 140, 164), 0.55) \
    + '''		itemDef
		{
			name				portrait
			style				WINDOW_STYLE_EMPTY
			rect				256 222 128 150
			ownerdraw			257						// UI_COOP_MODEL_ICON
			font				3
			textscale			0.55
			forecolor			%s
			visible				1
			decoration
		}
		itemDef
		{
			name				character
			group				models
			type				ITEM_TYPE_MODEL
			rect				400 50 230 380
			model_g2anim		"BOTH_STAND1"
			asset_model			"ui_char_model"
			model_angle			180
			model_g2mins		-20 -15 -10
			model_g2maxs		20 15 45
			model_fovx			40
			model_fovy			40
			isCharacter			1
			visible				1
			decoration
		}
''' % CREAM \
    + label('hint', (30, 412, 600, 16), 'Les autres joueurs voient ce modele s ils ont le meme pk3 (l hote envoie les siens dans le salon).', 0.5, GREY) \
    + button('backButton', (30, 440, 160, 24), 'RETOUR', '				uiScript		coopForceBack\n', 'Revenir sans rien changer.') \
    + button('applyButton', (450, 440, 160, 24), 'APPLIQUER', '				uiScript		coopApplyModel ;\n				uiScript		coopForceBack\n', 'Prendre ce modele (tout de suite, meme en jeu).', 'RIGHT') \
    + '''	}
}
'''
write('coopskins.menu', skins)

# =================================================================== coopsaberpick.menu
# SABRE LASER : le pendant de PERSONNAGE / SKIN, ouvrable a tout moment (salon et F6).
# Uniquement des boutons a texte fixe et des labels : c'est ce que le reste du mod
# emploie, et une premiere version a base de "cvar" sur un bouton et de cvarStrList
# ne repondait pas a la souris. Les navigateurs de manche et de couleur sont des
# menus plein ecran : on ferme celui-ci avant de les ouvrir, sinon celui du dessus
# prend le focus sans forcement se voir (meme piege que l'ecran de Force en vague 4).
def saber_open(which, target):
    return ('\t\t\t\tsetcvar\t\t\tui_coopHiltFrom pick ;\n'
            '\t\t\t\tsetcvar\t\t\tui_coopHiltWhich %i ;\n'
            '\t\t\t\tclose\t\t\tcoopSaberPick ;\n'
            '\t\t\t\topen\t\t\t%s\n') % (which, target)

DUAL = 'ui_saber_type'
pick = '''// JACoop - SABRE LASER : type, manches et couleurs, a tout moment. Ouvert depuis le
// salon ou le menu F6 ; ui_coopForceFrom (lobby/game) dit ou revenir.
// Genere par dist/gen_coop_menus.py : ne pas editer a la main.
{
''' + header('coopSaberPick', '\t\t\tuiScript\t\t\tcoopForceBack\n',
                '\t\t\tuiScript\t\t\tcoopSaberInit ;\n\t\t\tsetfocus\t\t\ttypeSingle\n', 1, 424) + decor(logo=False) \
    + title('SABRE LASER') \
    + label('typeLabel', (30, 66, 300, 16), 'TYPE DE SABRE', 0.7, GOLD) \
    + button('typeSingle', (30, 88, 300, 22), 'SABRE SIMPLE', '\t\t\t\tuiScript\t\tcoopSaberType single\n', 'Une seule lame.') \
    + button('typeStaff', (30, 112, 300, 22), 'DOUBLE LAME', '\t\t\t\tuiScript\t\tcoopSaberType staff\n', 'Un seul manche, une lame a chaque bout.') \
    + button('typeDual', (30, 136, 300, 22), 'DEUX SABRES', '\t\t\t\tuiScript\t\tcoopSaberType dual\n', 'Un sabre dans chaque main.') \
    + label('typeNow', (30, 160, 300, 16), '', 0.6, GREY, cvar='ui_saber_type') \
    + label('hiltLabel', (30, 196, 300, 16), 'MANCHE', 0.7, GOLD) \
    + label('hiltNow', (30, 216, 300, 18), '', 0.7, CREAM, cvar='ui_saber_name') \
    + button('hiltButton', (30, 238, 300, 22), 'CHOISIR LE MANCHE', saber_open(1, 'coopSaberHilts'),
             'Tous les manches installes, mods compris.') \
    + label('hilt2Now', (30, 264, 300, 18), '', 0.7, CREAM, cvar='ui_saber2_name', cvartest=DUAL, show=['dual']) \
    + button('hilt2Button', (30, 286, 300, 22), 'MANCHE DE LA MAIN GAUCHE', saber_open(2, 'coopSaberHilts'),
             'Le manche de la seconde lame.', 'LEFT', cvartest=DUAL, show=['dual']) \
    + label('colorLabel', (30, 320, 300, 16), 'COULEUR DE LAME', 0.7, GOLD) \
    + button('colorButton', (30, 342, 300, 22), 'COULEUR EXACTE (RVB)', saber_open(1, 'coopSaberColor'),
             'Couleur exacte de la lame, en rouge / vert / bleu.') \
    + button('color2Button', (30, 366, 300, 22), 'COULEUR DE LA LAME GAUCHE', saber_open(2, 'coopSaberColor'),
             'Couleur exacte de la seconde lame.', 'LEFT', cvartest=DUAL, show=['dual']) \
    + saber_preview((330, 90, 300, 300), False, cvar=DUAL, hide='dual') \
    + saber_preview((330, 56, 300, 210), False, name='saberR', cvar=DUAL, show='dual') \
    + saber_preview((330, 240, 300, 210), True, name='saberL', cvar=DUAL, show='dual') \
    + label('previewR', (340, 172, 300, 14), 'MAIN DROITE', 0.55, GREY, cvartest=DUAL, show=['dual']) \
    + label('previewL', (340, 355, 300, 14), 'MAIN GAUCHE', 0.55, GREY, cvartest=DUAL, show=['dual']) \
    + label('hint', (30, 412, 600, 16), 'Les autres joueurs voient ton sabre s ils ont le meme pk3 (il leur est envoye dans le salon).', 0.5, GREY) \
    + button('backButton', (30, 440, 160, 24), 'RETOUR', '\t\t\t\tuiScript\t\tcoopForceBack\n', 'Revenir sans rien changer.') \
    + button('applyButton', (450, 440, 160, 24), 'APPLIQUER', '\t\t\t\tuiScript\t\tupdatesabercvars ;\n\t\t\t\texec\t\t\t"cmd coop_rebuild" ;\n\t\t\t\tuiScript\t\tcoopForceBack\n',
             'Prendre ce sabre (tout de suite, meme en jeu).', 'RIGHT') \
    + '''\t}
}
'''
write('coopsaberpick.menu', pick)

# ===================================================================== coopnpc.menu
# PNJ (hote) : la liste vient de l interface elle-meme (UI_CoopScanNpcs lit
# ext_data/npcs comme le fait le module de jeu de son cote), l apercu est celui
# de l ecran des skins, et APPARAITRE lance "npc spawn <nom>" devant l hote.
# La triche est la meme cvar que la console (helpUsObi), publiee a tout le monde.
npc = '''// JACoop - PNJ : faire apparaitre un personnage devant soi (hote). Ouvert depuis le
// menu F6. Genere par dist/gen_coop_menus.py : ne pas editer a la main.
{
''' + header('coopNpcMenu', '			uiScript			coopForceBack\n', '			uiScript			coopNpcsInit\n', 1, 424) + decor(logo=False) \
    + title('PNJ') \
    + label('countLabel', (30, 62, 300, 16), '', 0.6, GREY, cvar='ui_coopNpcCount') \
    + listbox('npcList', (30, 82, 300, 300), '0x1e', 16, 0.65) \
    + label('nameLabel', (350, 62, 260, 20), '', 0.8, GOLD, cvar='ui_coopNpcName') \
    + label('infoLabel', (350, 84, 260, 16), '', 0.55, GREY, cvar='ui_coopNpcInfo') \
    + '''		itemDef
		{
			name				npcModel
			group				models
			type				ITEM_TYPE_MODEL
			rect				380 100 230 300
			model_g2anim		"BOTH_STAND1"
			asset_model			"models/players/stormtrooper/model.glm"
			model_angle			180
			model_g2mins		-20 -15 -10
			model_g2maxs		20 15 45
			model_fovx			40
			model_fovy			40
			isCharacter			1
			visible				1
			decoration
		}
''' \
    + multi('cheats', (30, 390, 300, 20), 'Triche (console) :', 'helpUsObi',
            [('Non', 0), ('Oui', 1)],
            'Autorise les commandes de triche pour tout le monde. APPARAITRE l active tout seul.') \
    + label('hint', (30, 412, 600, 16), 'Le PNJ apparait devant l hote. Les invites le voient comme n importe quel personnage du niveau.', 0.5, GREY) \
    + button('backButton', (30, 440, 160, 24), 'RETOUR', '				uiScript		coopForceBack\n', 'Revenir au menu.') \
    + button('spawnButton', (430, 440, 180, 24), 'FAIRE APPARAITRE', '				uiScript		coopNpcSpawn\n',
             'Faire apparaitre ce PNJ devant toi.', 'RIGHT') \
    + '''	}
}
'''
write('coopnpc.menu', npc)

# ======================================================================= coopsaber.menu
# Les listes de sabres de l ecran du jeu etaient ecrites en dur (les neuf manches
# d origine) : un mod de sabres n y apparaissait jamais. Ce navigateur liste tout ce que
# le moteur trouve dans ext_data/sabers/*.sab (feeder 0x1d, ui/ui_saber.cpp les lit dans
# SaberParms, la meme source que l apercu 3D). ui_coopHiltWhich dit quelle lame on change
# (1 = sabre droit / ui_saber, 2 = sabre gauche / ui_saber2). Le cvar est ecrit tout de
# suite pour que l apercu suive ; RETOUR le remet comme il etait.
BACK_HILT = '''			uiScript			coopHiltCancel ;
			uiScript			coopHiltDone ;
			uiScript			coopHiltReturn
'''
hilts = '''// JACoop - CHOIX DU MANCHE : tous les sabres installes (ext_data/sabers/*.sab, mods
// compris) avec leur nom, filtres par le type de sabre de l ecran, et l apercu 3D du jeu.
// Ouvert depuis l ecran SABRE. Genere par dist/gen_coop_menus.py.
{
''' + header('coopSaberHilts', BACK_HILT, '			uiScript			coopHiltsInit\n', 1, 424) + decor(logo=False) \
    + label('title', (0, 24, 640, 30), '', 1.2, TITLE, 'CENTER', cvar='ui_coopHiltTitle') \
    + label('countLabel', (320, 60, 300, 16), '', 0.6, GREY, cvar='ui_coopHiltCount') \
    + listbox('hiltList', (320, 80, 300, 330), '0x1d', 16, 0.65) \
    + saber_preview((10, 120, 300, 300), False) + saber_preview((10, 120, 300, 300), True) \
    + label('hint', (30, 412, 600, 16), 'Les autres joueurs voient ce manche s ils ont le meme pk3 que toi.', 0.5, GREY) \
    + button('backButton', (30, 440, 160, 24), 'RETOUR', BACK_HILT.replace('\t\t\t', '\t\t\t\t'), 'Revenir sans changer de manche.') \
    + button('applyButton', (450, 440, 160, 24), 'VALIDER', '				uiScript		coopHiltDone ;\n				uiScript		coopHiltReturn\n', 'Garder ce manche.', 'RIGHT') \
    + '''	}
}
'''
write('coopsaber.menu', hilts)

# ================================================================== coopsabercolor.menu
# Couleur de lame exacte : les trois curseurs ecrivent ui_saber_rgb_r/g/b, et le moteur
# recompose g_saber_color / ui_saber_color sous la forme "#rrggbb" (ownerdraw 258, qui
# dessine aussi la pastille) - d ou l apercu 3D qui suit le curseur. Les six couleurs du
# jeu restent disponibles en raccourci et continuent de s ecrire par leur nom.
BACK_COLOR = '''			uiScript			coopColorCancel ;
			uiScript			coopHiltReturn
'''
PRESETS = [('ROUGE', 'red'), ('ORANGE', 'orange'), ('JAUNE', 'yellow'),
           ('VERT', 'green'), ('BLEU', 'blue'), ('VIOLET', 'purple')]
color = '''// JACoop - COULEUR DE LAME (RVB) : trois curseurs 0-255, une pastille et l apercu 3D
// de la lame. Ouvert depuis l ecran SABRE. Genere par dist/gen_coop_menus.py.
{
''' + header('coopSaberColor', BACK_COLOR, '			uiScript			coopColorInit\n', 1, 424) + decor(logo=False) \
    + label('title', (0, 24, 640, 30), '', 1.2, TITLE, 'CENTER', cvar='ui_coopHiltTitle') \
    + box('sliderBox', (30, 70, 300, 120), 0.55) \
    + slider('sliderR', (40, 82, 280, 16), 'ROUGE', 'ui_saber_rgb_r', 0, 255, 255, 'Composante rouge de la lame (0-255).') \
    + slider('sliderG', (40, 112, 280, 16), 'VERT', 'ui_saber_rgb_g', 0, 255, 255, 'Composante verte de la lame (0-255).') \
    + slider('sliderB', (40, 142, 280, 16), 'BLEU', 'ui_saber_rgb_b', 0, 255, 255, 'Composante bleue de la lame (0-255).') \
    + ownerdraw('swatch', (40, 200, 120, 28), '258', 'UI_COOP_SABER_SWATCH') \
    + label('hexLabel', (170, 206, 200, 16), '', 0.65, CREAM, cvar='ui_coopColorText') \
    + label('presetLabel', (30, 246, 300, 16), 'COULEURS DU JEU', 0.7, GOLD) \
    + saber_preview((330, 100, 300, 300), False) + saber_preview((330, 100, 300, 300), True) \
    + ''.join(button('preset%d' % i, (30 + (i % 3) * 100, 266 + (i // 3) * 26, 95, 22), lbl,
                      '				uiScript		coopColorPreset %s\n' % val,
                      'Reprendre la couleur %s du jeu.' % lbl.lower())
              for i, (lbl, val) in enumerate(PRESETS)) \
    + label('hint', (30, 380, 600, 16), 'La couleur voyage avec ton personnage : les autres joueurs voient la meme.', 0.5, GREY) \
    + label('hint2', (30, 396, 600, 16), 'Elle est gardee dans les sauvegardes comme les six couleurs du jeu.', 0.5, GREY) \
    + button('backButton', (30, 440, 160, 24), 'RETOUR', BACK_COLOR.replace('\t\t\t', '\t\t\t\t'), 'Revenir sans changer la couleur.') \
    + button('applyButton', (450, 440, 160, 24), 'VALIDER', '				uiScript		coopHiltReturn\n', 'Garder cette couleur.', 'RIGHT') \
    + '''	}
}
'''
write('coopsabercolor.menu', color)


# ================================================================== fin de mission (coop)
# Le module de jeu de l'hote pilote ces trois panneaux avec la commande serveur
# "coopmenu <menu>" et publie tout ce qu'ils affichent dans CS_COOP_ENDLEVEL
# (game/g_coop_endlevel.cpp) : ui_coopEndPhase (D debriefing, V vote, L equipement,
# W l'hote est sur les ecrans du jeu), ui_coopEndTitle, ui_coopEndStats,
# ui_coopEndInfo, ui_coopEndHost (1 = c'est moi qui heberge).
EP = 'ui_coopEndPhase'
NOOP = '\t\t\tshow\t\t\t\ttitle\n'   # Echap ne doit pas fermer ces panneaux

debrief = '''// JACoop - FIN DE MISSION : le debriefing, vu par tout le monde en meme temps.
// Genere par dist/gen_coop_menus.py : ne pas editer a la main.
{
''' + header('coopDebrief', NOOP, '\t\t\tsetfocus\t\t\tnextButton\n', 1, 410) + decor(logo=False) \
    + title('MISSION TERMINEE', 34) \
    + label('missionName', (0, 80, 640, 26), '', 1.0, TITLE, 'CENTER', cvar='ui_coopEndTitle') \
    + box('statsBox', (90, 128, 460, 108), 0.5) \
    + label('statsLine', (90, 152, 460, 20), '', 0.75, CREAM, 'CENTER', cvar='ui_coopEndStats') \
    + label('statsHint', (90, 190, 460, 16), 'Chacun garde ses armes, ses pouvoirs et sa progression.', 0.55, GREY, 'CENTER') \
    + label('playersLabel', (170, 252, 300, 16), 'JOUEURS', 0.65, GOLD, 'CENTER') \
    + listbox('playerList', (170, 272, 300, 84), '0x19', 18, 0.6) \
    + label('infoLine', (0, 368, 640, 18), '', 0.65, CREAM, 'CENTER', cvar='ui_coopEndInfo') \
    + label('hintD', (0, 396, 640, 16), 'SUIVANT : la carte des missions s ouvre quand tout le monde a continue.', 0.55, GREY, 'CENTER', cvartest=EP, show=['D']) \
    + label('hintW', (0, 396, 640, 16), 'Cette etape de la campagne n offre pas de choix : attends l hote.', 0.55, GREY, 'CENTER', cvartest=EP, show=['W']) \
    + button('nextButton', (240, 440, 160, 24), 'SUIVANT', '\t\t\t\tuiScript\t\tcoopEndNext\n', 'J ai lu le debriefing.', 'CENTER', cvartest=EP, show=['D']) \
    + button('goButton', (470, 440, 150, 24), 'PASSER (HOTE)', '\t\t\t\tuiScript\t\tcoopEndGo\n', 'Hote : passer a la suite sans attendre les autres.', 'RIGHT', cvartest='ui_coopEndHost', show=['1']) \
    + '''\t}
}
'''
write('coopdebrief.menu', debrief)

vote = '''// JACoop - FIN DE MISSION : vote sur la prochaine mission (feeder 0x1c = CS_COOP_ENDLEVEL).
// Genere par dist/gen_coop_menus.py : ne pas editer a la main.
{
''' + header('coopVote', NOOP, '\t\t\tuiScript\t\t\tcoopVoteOpen ;\n\t\t\tsetfocus\t\t\tmissionList\n') + decor(logo=False) \
    + title('PROCHAINE MISSION', 28) \
    + label('rule', (0, 64, 640, 16), 'Chacun vote : la majorite l emporte, l hote tranche en cas d egalite.', 0.55, GREY, 'CENTER') \
    + label('missionsLabel', (60, 90, 520, 16), 'MISSIONS DISPONIBLES', 0.7, GOLD) \
    + listbox('missionList', (60, 110, 520, 112), '0x1c', 18, 0.65, '\t\t\t\tuiScript\t\tcoopEndVote\n') \
    + button('voteButton', (60, 232, 200, 24), 'VOTER', '\t\t\t\tuiScript\t\tcoopEndVote\n', 'Voter pour la mission selectionnee (double-clic aussi).') \
    + button('goButton', (380, 232, 200, 24), 'TRANCHER (HOTE)', '\t\t\t\tuiScript\t\tcoopEndGo\n', 'Hote : compter les voix tout de suite.', 'LEFT', cvartest='ui_coopEndHost', show=['1']) \
    + label('playersLabel', (60, 272, 520, 16), 'JOUEURS', 0.7, GOLD) \
    + listbox('playerList', (60, 292, 520, 84), '0x19', 18, 0.6) \
    + label('infoLine', (0, 392, 640, 18), '', 0.65, CREAM, 'CENTER', cvar='ui_coopEndInfo') \
    + label('hint', (0, 420, 640, 16), 'Une mission deja jouee n apparait pas dans la liste.', 0.55, GREY, 'CENTER') \
    + '''\t}
}
'''
write('coopvote.menu', vote)

loadout = '''// JACoop - FIN DE MISSION : equipement de chacun avant le depart, puis PRET.
// Les ecrans de Force et d armes sont les copies coop (coopForceSelect, coopWpnSelect) :
// un invite n a pas de serveur, ses choix repartent vers l hote (coopforce / coop_endwpn).
// Genere par dist/gen_coop_menus.py : ne pas editer a la main.
{
''' + header('coopLoadout', NOOP, '\t\t\tsetfocus\t\t\treadyButton\n') + decor(logo=False) \
    + title('PREPARATION', 28) \
    + label('missionLabel', (0, 66, 640, 16), 'PROCHAINE MISSION', 0.6, GOLD, 'CENTER') \
    + label('missionName', (0, 84, 640, 26), '', 0.95, TITLE, 'CENTER', cvar='ui_coopEndTitle') \
    + colbutton('forceButton', 150, 'POUVOIRS DE FORCE', '\t\t\t\tsetcvar\t\t\tui_coopForceFrom endlevel ;\n\t\t\t\tclose\t\t\tall ;\n\t\t\t\topen\t\t\tcoopForceSelect\n',
                'Depenser ton point de Force avant de partir.') \
    + colbutton('wpnButton', 190, 'ARMES', '\t\t\t\tclose\t\t\tall ;\n\t\t\t\topen\t\t\tcoopWpnSelect\n',
                'Choisir deux armes et un explosif pour la mission.') \
    + colbutton('readyButton', 244, 'PRET / PAS PRET', '\t\t\t\tuiScript\t\tcoopEndReady\n',
                'La mission demarre quand tout le monde est pret.') \
    + label('playersLabel', (320, 150, 290, 16), 'JOUEURS', 0.7, GOLD) \
    + listbox('playerList', (320, 170, 290, 100), '0x19', 18, 0.6) \
    + label('infoLine', (0, 330, 640, 18), '', 0.65, CREAM, 'CENTER', cvar='ui_coopEndInfo') \
    + label('hint1', (0, 360, 640, 16), 'Tes armes et tes pouvoirs te suivent d une mission a l autre.', 0.55, GREY, 'CENTER') \
    + label('hint2', (0, 376, 640, 16), 'L hote peut lancer la mission sans attendre les retardataires.', 0.55, GREY, 'CENTER') \
    + button('goButton', (470, 440, 150, 24), 'COMMENCER (HOTE)', '\t\t\t\tuiScript\t\tcoopEndGo\n', 'Hote : lancer la mission tout de suite.', 'RIGHT', cvartest='ui_coopEndHost', show=['1']) \
    + '''\t}
}
'''
write('cooploadout.menu', loadout)

# ================================================== coopwpn.menu (copie de l'ecran d'armes)
# Meme ecran que le jeu (ref-ui/ui/ingamewpnselect.menu, fichier Raven) : seuls le nom du
# menu et la sortie changent. COMMENCER LA MISSION ne lance plus la carte, il renvoie au
# panneau de preparation (uiScript coopWeaponsDone) ; c est l hote qui lance.
import re

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(DST)))
REF = os.path.join(ROOT, 'ref-ui', 'ui', 'ingamewpnselect.menu')
wpn = io.open(os.path.normpath(REF), encoding='latin-1').read()
assert wpn.count('"ingameWpnSelect"') == 1
wpn = wpn.replace('"ingameWpnSelect"', '"coopWpnSelect"')
wpn, n = re.subn(r'exec\s+"(?:vstr tier_mapname|maptransition [a-z0-9_]+)"', 'uiScript\t\tcoopWeaponsDone', wpn)
assert n == 4, n
wpn, n = re.subn(r'(onESC\s*\n\s*\{\s*\n\s*play\s+"sound/interface/menuroam\.wav"\s*\n)',
                 r'\1\t\t\tuiScript\t\t\tcoopWeaponsDone\n', wpn, count=1)
assert n == 1
wpn = ('// JACoop - choix des armes entre deux missions (ecran du jeu, fichier Raven) : le bouton\n'
       '// COMMENCER renvoie au panneau de preparation au lieu de lancer la carte.\n'
       '// Genere par dist/gen_coop_menus.py depuis ref-ui/ui/ingamewpnselect.menu.\n') + wpn
io.open(os.path.join(DST, 'coopwpn.menu'), 'w', encoding='latin-1', newline='\n').write(wpn)
print('wrote coopwpn.menu')
