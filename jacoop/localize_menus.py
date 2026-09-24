# -*- coding: utf-8 -*-
"""
Menus du mod en deux langues : francais pour un jeu en francais, anglais pour
toutes les autres langues (JD, 24/09).

Chaque texte francais des menus (text, descText, libelles des listes) connu de
i18n/menus_en.py devient une reference @JACOOP_<CLE> ; le jeu la resout avec
sa langue (se_language) dans strings/<langue>/jacoop.str, fichiers ecrits ici :
    english  : l'anglais
    french   : le francais (l'anglais en texte de reference, comme le jeu)
    german, spanish : l'anglais (les autres langues du jeu)

Lance a la fin de skin_menus.py (donc apres gen_coop_menus.py) :
    python dist/gen_coop_menus.py ; python dist/skin_menus.py
Un texte francais absent de la table est signale : l'ajouter a la table.
"""

import glob, hashlib, io, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, 'i18n'))
from menus_en import EN          # noqa: E402

UI = os.path.join(HERE, 'pk3', 'ui')
STRINGS = os.path.join(HERE, 'pk3', 'strings')
PACKAGE = 'JACOOP'              # le nom du fichier .str fait le prefixe des cles
OTHERS = ('german', 'spanish')  # les autres langues livrees avec le jeu : l'anglais

# textes francais sans equivalent a traduire (valeurs, noms propres, chiffres)
KEEP = re.compile(r'^[\d\sx:.,/()%+-]*s?$')


def key_for(fr, used):
    slug = re.sub(r'[^A-Z0-9]+', '_', fr.upper()).strip('_')[:40].strip('_') or 'T'
    key = slug
    if key in used and used[key] != fr:
        key = '%s_%s' % (slug[:33], hashlib.md5(fr.encode('latin-1')).hexdigest()[:6].upper())
    used[key] = fr
    return key


def localize():
    used, missing = {}, set()
    # une cle par texte de la table, dans l'ordre de la table : les memes cles a
    # chaque passage, et des fichiers de langue complets meme si on relance ce
    # script sur des menus deja traduits
    keys = {fr: key_for(fr, used) for fr in EN}

    def ref(fr):
        if fr.startswith('@') or not re.search(r'[A-Za-z]', fr) or KEEP.match(fr):
            return None
        if fr not in EN:
            missing.add(fr)
            return None
        return '@%s_%s' % (PACKAGE, keys[fr])

    for path in sorted(glob.glob(os.path.join(UI, '*.menu'))):
        s = io.open(path, encoding='latin-1', newline='').read()
        orig = s

        def sub_text(m):
            r = ref(m.group(3))
            return m.group(0) if r is None else '%s%s"%s"' % (m.group(1), m.group(2), r)
        s = re.sub(r'^(\s*(?:text|descText)\b)(\s+)"([^"]*)"', sub_text, s, flags=re.M)

        def sub_list(m):
            def one(mm):
                r = ref(mm.group(1))
                return mm.group(0) if r is None else '"%s"%s' % (r, mm.group(2))
            # dans une liste : "libelle" valeur "libelle" valeur ...
            return m.group(1) + re.sub(r'"([^"]*)"(\s+"?[^"\s}]+"?)', one, m.group(2)) + m.group(3)
        s = re.sub(r'(cvar(?:Str|Float)List\s*\{)([^}]*)(\})', sub_list, s)
        if s != orig:
            io.open(path, 'w', encoding='latin-1', newline='').write(s)

    # les fichiers de langue
    entries = sorted(((k, fr, EN[fr]) for fr, k in keys.items()), key=lambda e: e[0])
    head = ('// JACoop - textes des menus (genere par dist/localize_menus.py, ne pas editer)\n'
            'VERSION             "1"\nCONFIG              "W:\\bin\\stringed.cfg"\nFILENOTES           "JACoop menus"\n\n')

    def write(lang, second):
        d = os.path.join(STRINGS, lang)
        if not os.path.isdir(d):
            os.makedirs(d)
        out = [head]
        for k, fr, en in entries:
            out.append('REFERENCE           %s\nLANG_ENGLISH        "%s"\n' % (k, en))
            if second:
                tag, text = second(fr, en)
                out.append('%-20s"%s"\n' % (tag, text))
            out.append('\n')
        out.append('ENDMARKER\n\n')    # le chargeur du jeu refuse un fichier sans
        io.open(os.path.join(d, 'jacoop.str'), 'w', encoding='latin-1', newline='\r\n').write(''.join(out))

    write('english', None)
    write('french', lambda fr, en: ('LANG_FRENCH', fr))
    for lang in OTHERS:
        write(lang, lambda fr, en, lang=lang: ('LANG_' + lang.upper(), en))
    print('localize_menus: %d textes -> @%s_..., strings/{english,french,%s}/jacoop.str'
          % (len(entries), PACKAGE, ','.join(OTHERS)))
    for fr in sorted(missing):
        print('  NON TRADUIT : %s' % fr)
    return missing


if __name__ == '__main__':
    localize()
