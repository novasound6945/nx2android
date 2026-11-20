#!/usr/bin/env python3
# The start screen's font (dm/android/startscreen.c): Noto Sans KR, Regular and Bold, cut to what the screen draws, from the variable font Google Fonts publishes:
#   https://github.com/google/fonts/raw/main/ofl/notosanskr/NotoSansKR%5Bwght%5D.ttf   (and OFL.txt beside it; dm/third_party/noto-sans-kr/README.md has the hashes)
# Kept: the 2,350 Hangul syllables of KS X 1001 (a game's name in Korean), its compatibility jamo, ASCII, Latin-1, and the punctuation and arrows the screens use.
# Each weight is an instance of the variable font (wght 400 and 700), with TrueType outlines (glyf: what stb_truetype reads), no hinting, no GSUB / GPOS (the screen
# lays out with advances and the kern pairs only), so it stays small.
# Usage: make_start_font.py NotoSansKR[wght].ttf out_dir   (needs fontTools: pip install fonttools)
import sys
from pathlib import Path
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer
from fontTools import subset

WEIGHTS = {'Regular': 400, 'Bold': 700}


def ksx1001_hangul():   # the 2,350 syllables: EUC-KR lead bytes B0..C8, trail bytes A1..FE
    return [ord(bytes([a, b]).decode('euc-kr')) for a in range(0xB0, 0xC9) for b in range(0xA1, 0xFF)]


def codepoints():
    cps = set(range(0x20, 0x7F)) | set(range(0xA0, 0x100)) | set(range(0x3131, 0x318F))   # ASCII, Latin-1, the compatibility jamo
    cps |= set(range(0x2010, 0x2028)) | {0x2030, 0x2032, 0x2033, 0x2039, 0x203A, 0x20A9, 0x2122}   # dashes, quotes, the bullet and ellipsis, ‰, ′ ″, ‹ ›, ₩, ™
    cps |= set(range(0x2190, 0x2196)) | {0x00D7, 0x2212, 0x2713, 0x25CF, 0x25CB, 0x3001, 0x3002, 0x300C, 0x300D, 0xFFFD}   # arrows, ×, −, ✓, ● ○, 、 。 「 」
    return sorted(cps | set(ksx1001_hangul()))


def build(src, out_dir):
    out_dir = Path(out_dir); out_dir.mkdir(parents=True, exist_ok=True); made = []
    for name, w in WEIGHTS.items():
        f = instancer.instantiateVariableFont(TTFont(src, recalcTimestamp=False), {'wght': w})
        o = subset.Options(); o.layout_features = []; o.hinting = False; o.desubroutinize = True; o.drop_tables += ['GSUB', 'GPOS', 'GDEF', 'STAT', 'DSIG', 'BASE', 'vhea', 'vmtx']
        o.name_IDs = ['*']; o.name_languages = ['*']; o.notdef_outline = True
        s = subset.Subsetter(o); s.populate(unicodes=codepoints()); s.subset(f)
        p = out_dir / f'NotoSansKR-{name}.subset.ttf'; f.save(p); made.append(p)
    return made


if __name__ == '__main__':
    if len(sys.argv) != 3: sys.exit('usage: make_start_font.py NotoSansKR[wght].ttf out_dir')
    for p in build(sys.argv[1], sys.argv[2]): print(f'{p}: {p.stat().st_size} bytes')
