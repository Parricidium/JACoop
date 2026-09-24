"""JACoop - polices bitmap du jeu (fonts/*.fontdat + *.tga) generees depuis une police TrueType.

Jedi Academy dessine tout son texte avec des atlas de glyphes (qcommon/qfiles.h : 256 x
glyphInfo_t {short width, height, horizAdvance, horizOffset ; int baseline ; float s, t, s2, t2}
puis short pointSize, height, ascender, descender, int hack = 7180 octets). Les menus
choisissent 'font 1..4' = ocr_a (petite), ergoec (moyenne), anewhope (titres), arialnb ;
rd-common/tr_font.cpp charge aussi des variantes <nom>_sharp1..8 plus grandes qu'il prend
quand le texte est rendu plus gros que le pointSize (ecrans HD), ce qui garde le texte net.

On remplace les quatre polices du jeu par la meme famille sans serif (Titillium Web, SIL OFL,
dist/fonts-src/) en gardant les pointSize/hauteurs du jeu pour ne pas casser la mise en page,
avec des variantes 2x et 4x pour le 1080p / 4K.

    python dist/fontgen.py            -> dist/pk3/fonts/*.fontdat, *.tga (+ apercu dist/out/fonts-preview.png)
"""
import io, os, struct, sys
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'dist', 'fonts-src')
DST = os.path.join(ROOT, 'dist', 'pk3', 'fonts')

# nom du jeu -> (ttf, pointSize du jeu, hauteur des capitales visee en pixels au pointSize, majuscules forcees,
#                compression horizontale (Titillium est plus large que les polices du jeu))
FONTS = {
    'ocr_a':    ('TitilliumWeb-Regular.ttf',  18, 13.0, False, 0.90),   # console, textes petits, HUD
    'ergoec':   ('TitilliumWeb-SemiBold.ttf', 20, 14.0, False, 0.92),   # texte courant des menus
    'anewhope': ('TitilliumWeb-Bold.ttf',     17, 15.0, True,  0.92),   # titres et boutons (police capitales du jeu)
    'arialnb':  ('TitilliumWeb-Regular.ttf',  14, 10.0, False, 0.92),   # sous-titres / textes cinematiques
}
# en-tete des .fontdat du jeu (height, ascender, descender) : le moteur place la ligne de base a
# height - descender/2 sous le haut de l'item, il faut donc garder ces valeurs (x facteur de variante)
STOCK_HDR = {'ocr_a': (21, 17, 4), 'ergoec': (22, 17, 5), 'anewhope': (20, 17, 3), 'arialnb': (14, 11, 3)}
VARIANTS = [1, 2]           # base (512x256), _sharp1 (x2, ecrans HD) ; le 4K reutilise la x2
GLYPH_COUNT = 256


def cp_char(code):
    """caractere pour le code 0..255 : latin-1, avec la zone 0x80-0x9F prise dans cp1252 (oe, euro...)."""
    if code < 32:
        return None
    if 0x80 <= code <= 0x9F:
        try:
            return bytes([code]).decode('cp1252')
        except UnicodeDecodeError:
            return None
    return chr(code)


def cap_height_px(font):
    bb = font.getbbox('H')
    return bb[3] - bb[1]


def build(name, ttf, point_size, cap_px, upper, mult, squeeze=1.0):
    """rasterise 256 glyphes ; renvoie (fontdat bytes, image RGBA)."""
    target_cap = cap_px * mult
    # taille de police PIL telle que la hauteur des capitales colle a celle de la police du jeu
    size = int(round(target_cap * 1.45))
    font = ImageFont.truetype(os.path.join(SRC, ttf), size)
    for _ in range(12):
        c = cap_height_px(font)
        if abs(c - target_cap) < 0.6:
            break
        size = max(4, int(round(size * target_cap / max(c, 1))))
        font = ImageFont.truetype(os.path.join(SRC, ttf), size)
    ascent, descent = font.getmetrics()

    W = 512 * mult
    H = 256 * mult
    img = Image.new('RGBA', (W, H), (255, 255, 255, 0))
    draw = ImageDraw.Draw(img)
    pad = 1 * mult
    x = pad
    y = pad
    row_h = 0
    glyphs = []
    space_adv = int(round(font.getlength(' ') * squeeze * 1.5))   # Titillium's space is narrow for a game font
    for code in range(GLYPH_COUNT):
        ch = cp_char(code)
        if code == 0xAD:
            ch = '-'                       # soft hyphen drawn as a hyphen
        if upper and ch and len(ch.upper()) == 1:
            ch = ch.upper()                # (not for sharp s / micro sign: two letters or Greek)
        if ch is None or ch == '\n' or ch == '\r' or ch == '\t':
            glyphs.append((0, 0, 0, 0, 0, 0.0, 0.0, 0.0, 0.0))
            continue
        if ch == ' ' or ch == '\xa0':
            # width 0 would make the engine draw a '.' instead: a 1x1 transparent cell
            glyphs.append((1, 1, space_adv, 0, 0, 0.0, 0.0, 1.0 / W, 1.0 / H))
            continue
        adv = font.getlength(ch) * squeeze
        bb = font.getbbox(ch)              # (x0, y0, x1, y1), origine = haut de la ligne (ascender)
        if bb is None or bb[2] <= bb[0] or bb[3] <= bb[1]:
            glyphs.append((1, 1, int(round(adv)), 0, 0, 0.0, 0.0, 1.0 / W, 1.0 / H))
            continue
        gw0 = bb[2] - bb[0]
        gh = bb[3] - bb[1]
        gw = max(1, int(round(gw0 * squeeze)))
        if x + gw + pad > W:
            x = pad
            y += row_h + pad
            row_h = 0
        if y + gh + pad > H:
            raise SystemExit('%s: atlas %dx%d trop petit' % (name, W, H))
        # dessine le glyphe a part, le compresse en largeur, le colle en (x, y)
        cell = Image.new('RGBA', (gw0, gh), (255, 255, 255, 0))
        ImageDraw.Draw(cell).text((-bb[0], -bb[1]), ch, font=font, fill=(255, 255, 255, 255))
        if gw != gw0:
            cell = cell.resize((gw, gh), Image.LANCZOS)
        img.paste(cell, (x, y), cell)
        s, t, s2, t2 = x / W, y / H, (x + gw) / W, (y + gh) / H
        glyphs.append((gw, gh, int(round(adv)), int(round(bb[0] * squeeze)), ascent - bb[1], s, t, s2, t2))
        x += gw + pad
        row_h = max(row_h, gh)

    dat = io.BytesIO()
    for g in glyphs:
        dat.write(struct.pack('<hhhhiffff', *g))
    hh, ha, hd = STOCK_HDR[name]
    dat.write(struct.pack('<hhhhi', point_size * mult, hh * mult, ha * mult, hd * mult, 0))
    assert dat.tell() == 7180
    return dat.getvalue(), img


def write_tga(path, img):
    """TGA 32 bits non compresse, origine en haut a gauche (descripteur 0x28), BGRA."""
    W, H = img.size
    px = img.tobytes('raw', 'BGRA')
    with open(path, 'wb') as f:
        f.write(struct.pack('<BBBHHBHHHHBB', 0, 0, 2, 0, 0, 0, 0, 0, W, H, 32, 0x28))
        f.write(px)


def main():
    os.makedirs(DST, exist_ok=True)
    for old in os.listdir(DST):
        if old.endswith('.tga'):
            os.remove(os.path.join(DST, old))
    previews = []
    for name, (ttf, point_size, cap_px, upper, squeeze) in FONTS.items():
        for i, mult in enumerate(VARIANTS):
            out = name if i == 0 else '%s_sharp%d' % (name, i)
            dat, img = build(name, ttf, point_size, cap_px, upper, mult, squeeze)
            open(os.path.join(DST, out + '.fontdat'), 'wb').write(dat)
            img.save(os.path.join(DST, out + '.png'), optimize=True)
            print('%-18s pointSize %3d atlas %dx%d' % (out, point_size * mult, img.size[0], img.size[1]))
            if i == 0:
                previews.append((name, img))
    # apercu : les 4 atlas de base sur fond noir + une ligne accentuee rendue avec chaque police
    sheet = Image.new('RGB', (512, 256 * len(previews) + 30 * len(previews)), (0, 0, 0))
    for k, (name, img) in enumerate(previews):
        sheet.paste(img, (0, 256 * k), img)
    y = 256 * len(previews)
    for name, (ttf, point_size, cap_px, upper, squeeze) in FONTS.items():
        f = ImageFont.truetype(os.path.join(SRC, ttf), 20)
        ImageDraw.Draw(sheet).text((4, y + 4), '%s: Elu deja a cote - ca fait 3 jours. (accents) Oe' % name, font=f, fill=(255, 255, 255))
        y += 30
    os.makedirs(os.path.join(ROOT, 'dist', 'out'), exist_ok=True)
    sheet.save(os.path.join(ROOT, 'dist', 'out', 'fonts-preview.png'))
    # licence a cote des polices
    lic = os.path.join(SRC, 'OFL.txt')
    if os.path.exists(lic):
        open(os.path.join(DST, 'OFL-TitilliumWeb.txt'), 'wb').write(open(lic, 'rb').read())


if __name__ == '__main__':
    main()
