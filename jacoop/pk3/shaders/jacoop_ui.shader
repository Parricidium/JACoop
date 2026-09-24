// JACoop : habillage des menus "style Arx" — redefinit les shaders de decor que
// tous les menus du jeu utilisent (le moteur JACoop laisse le pk3 du mod
// l'emporter sur assets1/shaders/ui.shader). Les menus eux-memes ne changent
// pas ; seuls le fond, les cadres bleus et les boites de contenu sont refaits.
//
// - fond plein ecran : gfx/jacoop/menu_bg (le meme que le menu principal)
// - cadres / textes defilants / boites bleues : invisibles
// - boites de contenu : sombres, translucides

// fond plein ecran (dessine au-dessus de main_centerblue et des textes lateraux)
gfx/menus/main_background
{
	nopicmip
	nomipmaps
	{
		map gfx/jacoop/menu_bg
		blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
		rgbGen vertex
		alphaGen const 0.93
	}
}

// ecran du personnage et datapad : leur propre fond devient le notre
gfx/menus/charmenu
{
	nopicmip
	nomipmaps
	{
		map gfx/jacoop/menu_bg
		rgbGen vertex
	}
}
gfx/menus/datapad
{
	nopicmip
	nomipmaps
	{
		map gfx/jacoop/menu_bg
		rgbGen vertex
	}
}
gfx/menus/sabermenu_back
{
	nopicmip
	nomipmaps
	{
		map gfx/jacoop/menu_bg
		rgbGen vertex
	}
}

// decor bleu retire (rien n'est dessine)
gfx/menus/main_centerblue
{
	{
		map $whiteimage
		blendFunc GL_ZERO GL_ONE
	}
}
gfx/menus/menu_side_text
{
	{
		map $whiteimage
		blendFunc GL_ZERO GL_ONE
	}
}
gfx/menus/menu_side_text_right
{
	{
		map $whiteimage
		blendFunc GL_ZERO GL_ONE
	}
}
gfx/menus/menu_boxes_left
{
	{
		map $whiteimage
		blendFunc GL_ZERO GL_ONE
	}
}
gfx/menus/menu_boxes_right
{
	{
		map $whiteimage
		blendFunc GL_ZERO GL_ONE
	}
}
gfx/menus/main_centerwindow
{
	{
		map $whiteimage
		blendFunc GL_ZERO GL_ONE
	}
}
gfx/menus/main_leftwindow
{
	{
		map $whiteimage
		blendFunc GL_ZERO GL_ONE
	}
}
gfx/menus/main_rightwindow
{
	{
		map $whiteimage
		blendFunc GL_ZERO GL_ONE
	}
}
gfx/menus/menu_rotate_ring_b
{
	{
		map $whiteimage
		blendFunc GL_ZERO GL_ONE
	}
}
gfx/menus/videologo
{
	{
		map $whiteimage
		blendFunc GL_ZERO GL_ONE
	}
}
gfx/menus/scanlines
{
	{
		map $whiteimage
		blendFunc GL_ZERO GL_ONE
	}
}
gfx/menus/charmenu_bottom
{
	{
		map $whiteimage
		blendFunc GL_ZERO GL_ONE
	}
}

// boites de contenu : sombres et translucides (a la place du bleu anime)
gfx/menus/menu_blendbox
{
	nopicmip
	nomipmaps
	{
		map $whiteimage
		blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
		rgbGen const ( 0.02 0.02 0.04 )
		alphaGen const 0.55
	}
}
gfx/menus/menu_blendbox2
{
	nopicmip
	nomipmaps
	{
		map $whiteimage
		blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
		rgbGen const ( 0.02 0.02 0.04 )
		alphaGen const 0.55
	}
}

// boites des ecrans sabre / nouvelle partie / force / armes : sombres
gfx/menus/sabermenu_box_top
{
	nopicmip
	{
		map $whiteimage
		blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
		rgbGen const ( 0.02 0.02 0.04 )
		alphaGen const 0.55
	}
}
gfx/menus/sabermenu_box_middle
{
	nopicmip
	{
		map $whiteimage
		blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
		rgbGen const ( 0.02 0.02 0.04 )
		alphaGen const 0.55
	}
}
gfx/menus/sabermenu_box_bottom
{
	nopicmip
	{
		map $whiteimage
		blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
		rgbGen const ( 0.02 0.02 0.04 )
		alphaGen const 0.55
	}
}
gfx/menus/sabermenu_box
{
	nopicmip
	{
		map $whiteimage
		blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
		rgbGen const ( 0.02 0.02 0.04 )
		alphaGen const 0.55
	}
}
gfx/menus/sabermenu_stylebox_left
{
	nopicmip
	{
		map $whiteimage
		blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
		rgbGen const ( 0.02 0.02 0.04 )
		alphaGen const 0.55
	}
}
gfx/menus/sabermenu_stylebox_right
{
	nopicmip
	{
		map $whiteimage
		blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
		rgbGen const ( 0.02 0.02 0.04 )
		alphaGen const 0.55
	}
}
gfx/menus/newgame_boxes
{
	{
		map $whiteimage
		blendFunc GL_ZERO GL_ONE
	}
}
gfx/menus/menu_boxred
{
	nopicmip
	{
		map $whiteimage
		blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
		rgbGen const ( 0.02 0.02 0.04 )
		alphaGen const 0.55
	}
}
gfx/menus/forcemenu_back
{
	nopicmip
	nomipmaps
	{
		map gfx/jacoop/menu_bg
		rgbGen vertex
	}
}
gfx/menus/weaponmenu_back
{
	nopicmip
	nomipmaps
	{
		map gfx/jacoop/menu_bg
		rgbGen vertex
	}
}
gfx/menus/datapad2
{
	nopicmip
	nomipmaps
	{
		map gfx/jacoop/menu_bg
		rgbGen vertex
	}
}

// pages COOPERATION (salon, F6, fin de mission) : meme fond que le menu principal
// par defaut, mais un nom a part pour que le joueur puisse le remplacer tout seul
// (cvar ui_coopPanelBg -> gfx/jacoop/fond_coop, voir UI_CoopSkinShader)
gfx/jacoop/panel_bg
{
	nopicmip
	nomipmaps
	{
		map gfx/jacoop/menu_bg
		blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
		rgbGen vertex
		alphaGen vertex
	}
}

// coop : marqueur "a terre" au-dessus d'un coequipier (cg_coop.cpp)
gfx/jacoop/downed
{
	nopicmip
	nomipmaps
	{
		map gfx/jacoop/downed.tga
		blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
		rgbGen vertex
		alphaGen vertex
	}
}
