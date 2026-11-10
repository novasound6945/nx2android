"""The text entry's Hangul composition (nxnative/dm/android/hangul_ime.h), without a device."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

DM = Path(__file__).resolve().parents[1] / 'nxnative/dm'

# (keys, the text after them): jamo are typed, '<' is the backspace, '|' ends the syllable (hg_reset), other characters are typed as they are (no jamo)
CASES = [
    ('ㅎ', 'ㅎ'), ('ㅎㅏ', '하'), ('ㅎㅏㄴ', '한'), ('ㅎㅏㄴㄱ', '한ㄱ'), ('ㅎㅏㄴㄱㅡ', '한그'), ('ㅎㅏㄴㄱㅡㄹ', '한글'),
    ('ㄱㅏㄱㅏ', '가가'), ('ㄱㅏㄴㅏ', '가나'), ('ㄱㅏㄱㅅㅏ', '각사'),
    ('ㄷㅏㄹㄱ', '닭'), ('ㄷㅏㄹㄱㅏ', '달가'), ('ㅇㅓㅂㅅㅇㅓ', '없어'), ('ㅇㅓㅂㅅㅓ', '업서'), ('ㅇㅏㄴㅎㅇㅏ', '않아'),
    ('ㄱㅗㅏ', '과'), ('ㅇㅜㅓㄴ', '원'), ('ㅇㅡㅣ', '의'), ('ㄱㅗㅐ', '괘'), ('ㄷㅗㅣ', '되'), ('ㅇㅜㅔ', '웨'), ('ㅇㅜㅣ', '위'),
    ('ㄲㅏㄲ', '깎'), ('ㅇㅣㅆ', '있'), ('ㅇㅣㅆㅇㅓ', '있어'), ('ㅇㅣㅆㅓ', '이써'), ('ㄱㅏㄸ', '가ㄸ'), ('ㄱㅏㄸㅏ', '가따'), ('ㄱㅏㅃ', '가ㅃ'), ('ㄱㅏㅉ', '가ㅉ'),
    ('ㄱㄱ', 'ㄱㄱ'), ('ㄱㅅ', 'ㄱㅅ'), ('ㄱㅏㄱㄱ', '각ㄱ'), ('ㄱㅏㄹㄱㄱ', '갉ㄱ'),
    ('ㅏ', 'ㅏ'), ('ㅗㅏ', 'ㅘ'), ('ㅏㅏ', 'ㅏㅏ'), ('ㅏㄱ', 'ㅏㄱ'), ('ㅏㄱㅏ', 'ㅏ가'), ('ㄱㅏㅏ', '가ㅏ'), ('ㄱㅘㅏ', '과ㅏ'),
    ('ㄷㅏㄹㄱ<', '달'), ('ㄷㅏㄹㄱ<<', '다'), ('ㄷㅏㄹㄱ<<<', 'ㄷ'), ('ㄷㅏㄹㄱ<<<<', ''), ('ㄷㅏㄹㄱ<<<<<', ''),
    ('ㄱㅗㅏ<', '고'), ('ㄱㅗㅏ<<', 'ㄱ'), ('ㅗㅏ<', 'ㅗ'), ('ㅗㅏ<<', ''), ('ㄲㅏㄲ<', '까'),
    ('ㄱㅏㄱㅏ<', '가ㄱ'), ('ㄱㅏㄱㅏ<<', '가'), ('ㄱㅏㄱㅏ<<<', ''),   # the first 가 is finished once the second starts: erasing goes on by characters
    ('ㄱㅏ|ㅏ', '가ㅏ'), ('ㄱㅏ|ㄱ', '가ㄱ'), ('ㄱㅏ|<', ''), ('ㄱㅏ ㄴㅏ', '가 나'), ('ㄱㅏ1ㅏ', '가1ㅏ'),
]

HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include "hangul_ime.h"
static hg_state s; static uint32_t text[64]; static int n;
static void key(uint32_t c) {
    if (c == '<') { uint32_t o; int r = hg_back(&s, &o); assert(r >= 0 && r <= 2); if (n) n--; if (r == 1) text[n++] = o; return; }
    if (c == '|') { hg_reset(&s); return; }
    int was = hg_active(&s); uint32_t out[2] = {0, 0}; int r = hg_feed(&s, c, out);
    if (!r) { assert(!hg_is_jamo(c)); hg_reset(&s); text[n++] = c; return; }
    assert(r == 1 || r == 2);
    if (was) n--;
    text[n++] = out[0]; if (r == 2) text[n++] = out[1];
}
static int check(int id, const uint32_t *keys, int nk, const uint32_t *want, int nw) {
    hg_reset(&s); n = 0;
    for (int i = 0; i < nk; i++) key(keys[i]);
    int ok = n == nw; for (int i = 0; ok && i < n; i++) ok = text[i] == want[i];
    if (!ok) { printf("case %d:", id); for (int i = 0; i < n; i++) printf(" %04X", (unsigned)text[i]); printf("\n"); }
    return ok;
}
int main(void) {
    uint32_t out[2], o; hg_state t; hg_reset(&t);
    assert(!hg_active(&t) && hg_back(&t, &o) == 0);
    assert(hg_feed(&t, 'a', out) == 0 && hg_feed(&t, 0xAC00, out) == 0 && hg_feed(&t, 0x3130, out) == 0 && hg_feed(&t, 0x3164, out) == 0 && !hg_active(&t));
    assert(hg_feed(&t, 0x3131, out) == 1 && out[0] == 0x3131 && hg_active(&t));                      // g: itself
    assert(hg_feed(&t, 0x314F, out) == 1 && out[0] == 0xAC00);                                       // ga
    assert(hg_feed(&t, 0x3131, out) == 1 && out[0] == 0xAC01);                                       // gag
    assert(hg_feed(&t, 0x314F, out) == 2 && out[0] == 0xAC00 && out[1] == 0xAC00);                   // ga ga: the final moves on
    assert(hg_back(&t, &o) == 1 && o == 0x3131 && hg_back(&t, &o) == 2 && !hg_active(&t) && hg_back(&t, &o) == 0);
    hg_reset(&t); assert(hg_feed(&t, 0x3133, out) == 1 && out[0] == 0x3133 && !hg_active(&t));      // a compound consonant alone composes nothing
    int bad = 0;
@CASES@
    return bad;
}
'''


def c_array(s):
    return '{' + ', '.join(str(ord(c)) for c in s) + (', ' if s else '') + '0}'


class HangulIme(unittest.TestCase):
    def test_composition(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            self.skipTest('a C compiler is needed')
        lines = []
        for i, (keys, want) in enumerate(CASES):
            lines.append(f'    {{ static const uint32_t k[] = {c_array(keys)}, w[] = {c_array(want)}; bad += !check({i}, k, {len(keys)}, w, {len(want)}); }}')
        with tempfile.TemporaryDirectory(prefix='nx2a_hangul_') as t:
            c, exe = Path(t) / 'h.c', Path(t) / 'h'
            c.write_text(HARNESS.replace('@CASES@', '\n'.join(lines)), encoding='utf-8')
            r = subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', f'-I{DM / "android"}', str(c), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            r = subprocess.run([str(exe)], capture_output=True, text=True)
            failed = [CASES[int(l.split()[1].rstrip(':'))] for l in r.stdout.splitlines() if l.startswith('case ')]
            self.assertEqual(r.returncode, 0, f'{r.stdout}{r.stderr}\nfailed: {failed}')


if __name__ == '__main__':
    unittest.main()
