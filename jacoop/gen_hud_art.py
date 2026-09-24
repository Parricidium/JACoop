# -*- coding: utf-8 -*-
"""
Art du HUD facon Jedi: Fallen Order (JD, 23/09).

Le moteur ne charge que jpg / png / tga : pas de SVG a l'execution. Les formes
sont donc soit de la geometrie pure cote code (net a toute resolution, aucune
image), soit - quand il faut des coins arrondis - une image rendue ICI en haute
definition, a partir d'une description vectorielle.

    python dist/gen_hud_art.py

Sorties (dist/pk3/gfx/jacoop/) :
    hud_bar.png     capsule blanche a bouts arrondis, decoupee en trois par le
                    code (bout gauche / milieu etire / bout droit) pour rester
                    nette quelle que soit la longueur de la barre
    hud_ring.png    pastille ronde de l'arme, anneau fin

Tout est blanc : la couleur vient du code (cgi_R_SetColor), ce qui evite une
image par teinte et laisse la vie, la Force et les alertes partager la meme.
"""

import os
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "pk3", "gfx", "jacoop")

SS = 8          # suréchantillonnage : on dessine 8x trop grand puis on réduit


def capsule(w, h, radius):
    """capsule blanche sur fond transparent"""
    im = Image.new("RGBA", (w * SS, h * SS), (255, 255, 255, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([0, 0, w * SS - 1, h * SS - 1], radius=radius * SS,
                        fill=(255, 255, 255, 255))
    return im.resize((w, h), Image.LANCZOS)


def ring(size, thickness):
    """anneau fin, blanc"""
    im = Image.new("RGBA", (size * SS, size * SS), (255, 255, 255, 0))
    d = ImageDraw.Draw(im)
    d.ellipse([0, 0, size * SS - 1, size * SS - 1], fill=(255, 255, 255, 255))
    inset = thickness * SS
    d.ellipse([inset, inset, size * SS - 1 - inset, size * SS - 1 - inset],
              fill=(255, 255, 255, 0))
    return im.resize((size, size), Image.LANCZOS)


def main():
    if not os.path.isdir(OUT):
        os.makedirs(OUT)

    # La capsule fait 3 fois sa hauteur : le tiers gauche et le tiers droit sont
    # les bouts arrondis, le tiers du milieu est uni et peut etre etire autant
    # qu'on veut sans deformer les bouts.
    h = 64
    bar = capsule(h * 3, h, h // 2)
    p = os.path.join(OUT, "hud_bar.png")
    bar.save(p, optimize=True)
    print("hud_bar.png   %dx%d  %d o" % (bar.size[0], bar.size[1], os.path.getsize(p)))

    r = ring(128, 7)
    p = os.path.join(OUT, "hud_ring.png")
    r.save(p, optimize=True)
    print("hud_ring.png  %dx%d  %d o" % (r.size[0], r.size[1], os.path.getsize(p)))


if __name__ == "__main__":
    main()
