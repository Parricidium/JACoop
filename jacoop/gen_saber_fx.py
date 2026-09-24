"""JACoop - art de lame neutre pour les couleurs de sabre exactes (RVB).

Le jeu n'a que six lames, chacune avec ses propres images deja teintees
(gfx/effects/sabers/<couleur>_glow2.jpg et <couleur>_line.jpg) : impossible
d'en tirer une couleur libre, une texture rouge multipliee par du bleu donne
du noir. On fabrique donc ici deux images BLANCHES de meme forme, que le
moteur teinte par lame (refEntity shaderRGBA, les shaders sont en
"rgbGen vertex") : cgame/cg_players.cpp et ui/ui_saber.cpp les utilisent des
qu'une lame porte une couleur exacte, et les six couleurs du jeu continuent
d'utiliser les images d'origine (rendu inchange).

Les profils sont analytiques (aucune image du jeu n'est copiee) mais calibres
sur les images d'origine, mesurees canal par canal : halo gaussien de pic 140
(chute a 38 % a mi-rayon), coeur a plateau sature sur le tiers central puis
extinction en puissance 1.45 - pour qu'une lame RVB brille comme une lame du
jeu. Le blanc incandescent du centre, lui, vient d'une seconde passe plus
fine dessinee en blanc par le moteur (une seule passe teintee ne peut pas
etre a la fois blanche au centre et coloree sur les bords).

    python dist/gen_saber_fx.py

Ecrit : dist/pk3/gfx/jacoop/rgb_glow.png, rgb_line.png
        dist/pk3/shaders/jacoop_saber.shader
"""
import io, math, os

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GFX = os.path.join(ROOT, 'dist', 'pk3', 'gfx', 'jacoop')
SHADERS = os.path.join(ROOT, 'dist', 'pk3', 'shaders')

GLOW_SIZE = 128
GLOW_PEAK = 140.0       # le pic des *_glow2.jpg du jeu
GLOW_SIGMA = 0.508      # chute mesuree sur ces memes images (38 % a mi-rayon)

LINE_W, LINE_H = 64, 256
LINE_PEAK = 255.0
LINE_FLAT = 0.35        # |u| ou le coeur est encore sature, mesure sur *_line.jpg
LINE_FALL = 1.45        # exposant de l'extinction au-dela


def smoothstep(a, b, x):
    if b <= a:
        return 0.0 if x < a else 1.0
    t = min(1.0, max(0.0, (x - a) / (b - a)))
    return t * t * (3.0 - 2.0 * t)


def make_glow():
    im = Image.new('RGB', (GLOW_SIZE, GLOW_SIZE))
    px = im.load()
    c = (GLOW_SIZE - 1) / 2.0
    for y in range(GLOW_SIZE):
        for x in range(GLOW_SIZE):
            r = math.hypot(x - c, y - c) / c
            v = GLOW_PEAK * math.exp(-(r / GLOW_SIGMA) ** 2)
            v *= 1.0 - smoothstep(0.85, 1.0, r)     # bord franc : pas de carre visible
            n = int(round(min(255.0, max(0.0, v))))
            px[x, y] = (n, n, n)
    return im


def make_line():
    im = Image.new('RGB', (LINE_W, LINE_H))
    px = im.load()
    for y in range(LINE_H):
        t = y / float(LINE_H - 1)
        # le long de la lame : apparition a la base, extinction a la pointe
        v = smoothstep(0.0, 0.22, t) * (1.0 - 0.87 * smoothstep(0.88, 1.0, t))
        for x in range(LINE_W):
            u = abs((x / float(LINE_W - 1)) * 2.0 - 1.0)
            if u <= LINE_FLAT:
                h = 1.0                             # plateau sature, comme le jeu
            else:
                h = ((1.0 - u) / (1.0 - LINE_FLAT)) ** LINE_FALL
            n = int(round(min(255.0, max(0.0, LINE_PEAK * v * h))))
            px[x, y] = (n, n, n)
    return im


SHADER = '''// JACoop : lame neutre pour les couleurs de sabre exactes (RVB).
// Meme construction que gfx/effects/sabers/<couleur>_glow et _line du jeu
// (additif, "glow" pour le halo, rgbGen vertex) mais sur une image blanche :
// le moteur y applique la couleur choisie par le joueur. Genere par
// dist/gen_saber_fx.py - ne pas editer a la main.

gfx/jacoop/rgb_glow
{
\tcull\ttwosided
    {
        map gfx/jacoop/rgb_glow
        blendFunc GL_ONE GL_ONE
        glow
        rgbGen vertex
    }
}

gfx/jacoop/rgb_line
{
\tcull\ttwosided
    {
        map gfx/jacoop/rgb_line
        blendFunc GL_ONE GL_ONE
        rgbGen vertex
    }
}
'''


def main():
    if not os.path.isdir(GFX):
        os.makedirs(GFX)
    make_glow().save(os.path.join(GFX, 'rgb_glow.png'))
    make_line().save(os.path.join(GFX, 'rgb_line.png'))
    io.open(os.path.join(SHADERS, 'jacoop_saber.shader'), 'w',
            encoding='latin-1', newline='\n').write(SHADER)
    print('wrote rgb_glow.png, rgb_line.png, jacoop_saber.shader')


if __name__ == '__main__':
    main()
