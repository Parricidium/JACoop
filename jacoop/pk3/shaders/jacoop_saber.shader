// JACoop : lame neutre pour les couleurs de sabre exactes (RVB).
// Meme construction que gfx/effects/sabers/<couleur>_glow et _line du jeu
// (additif, "glow" pour le halo, rgbGen vertex) mais sur une image blanche :
// le moteur y applique la couleur choisie par le joueur. Genere par
// dist/gen_saber_fx.py - ne pas editer a la main.

gfx/jacoop/rgb_glow
{
	cull	twosided
    {
        map gfx/jacoop/rgb_glow
        blendFunc GL_ONE GL_ONE
        glow
        rgbGen vertex
    }
}

gfx/jacoop/rgb_line
{
	cull	twosided
    {
        map gfx/jacoop/rgb_line
        blendFunc GL_ONE GL_ONE
        rgbGen vertex
    }
}
