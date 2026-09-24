# -*- coding: utf-8 -*-
"""
Decor du menu : logo et fond, fournis par JD.

Les originaux vivent dans dist/art/ ; ce script les met a la taille et au
format que le moteur aime (puissances de deux pas obligatoires, mais une
image de menu est redimensionnee a chaque chargement sinon) et les ecrit
dans le pk3.

    python dist/gen_menu_art.py

Sorties :
    dist/pk3/gfx/jacoop/menu_bg.jpg   fond du menu principal ET, par defaut,
                                      des autres menus (ui_shared.cpp,
                                      coopSkins[].packaged)
    dist/pk3/gfx/jacoop/deco_logo.png logo du menu principal, remplace
                                      gfx/menus/jediacademy

Le joueur garde la main : il depose ses propres fichiers dans
base/gfx/jacoop/ (logo, fond_principal, fond_menus, fond_coop) et ils
passent devant ceux-ci. Voir README.txt.
"""

import os
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ART = os.path.join(HERE, "art")
OUT = os.path.join(HERE, "pk3", "gfx", "jacoop")

# (source, sortie, largeur voulue, sauvegarde)
JOBS = [
    ("fond.jpg", "menu_bg.jpg", 1024, dict(quality=92, optimize=True, subsampling=0)),
    ("logo_coop.png", "deco_logo.png", 1024, dict(optimize=True)),
]


def main():
    if not os.path.isdir(OUT):
        os.makedirs(OUT)
    for src, dst, width, save in JOBS:
        srcp = os.path.join(ART, src)
        im = Image.open(srcp)
        # la hauteur suit l'image : c'est le rapport du contenu qui compte,
        # le moteur etire ensuite la texture dans le rectangle du menu
        height = max(1, int(round(im.size[1] * width / float(im.size[0]))))
        im = im.resize((width, height), Image.LANCZOS)
        if dst.endswith(".jpg"):
            im = im.convert("RGB")
        else:
            im = im.convert("RGBA")
        outp = os.path.join(OUT, dst)
        im.save(outp, **save)
        print("%-16s -> %s  %dx%d  %d o" % (src, dst, width, height, os.path.getsize(outp)))


if __name__ == "__main__":
    main()
