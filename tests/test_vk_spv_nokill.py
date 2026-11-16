"""spv_kill_to_merge (nxnative/dm/android/gfx_vk_spv_strip.inc, DM_VK_FS_NOKILL): a fragment shader's alpha-test discards taken out, on hand-made modules,
built with the host's C compiler. spirv-val (on PATH, or SPIRV_VAL) checks the modules after, when it is there.
"""
import os
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[1] / 'nxnative/dm/android/gfx_vk_spv_strip.inc'
HARNESS = r'''
#define _CRT_SECURE_NO_WARNINGS
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gfx_vk_spv_strip.inc"
// harness <in.spv> <out.spv>: writes the module after spv_kill_to_merge (as it was when the transform leaves it) and prints "kills=<n>"
int main(int argc, char **argv) {
    if (argc < 3) return 2;
    (void)spv_strip_clipcull; (void)spv_strip_caps;
    FILE *f = fopen(argv[1], "rb"); if (!f) return 3; fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint32_t *w = malloc((size_t)n), nw = (uint32_t)(n / 4); if (fread(w, 1, (size_t)n, f) != (size_t)n) return 3; fclose(f);
    uint32_t n2 = 0, k = 0, *w2 = spv_kill_to_merge(w, nw, &n2, &k); if (w2) { free(w); w = w2; nw = n2; }
    f = fopen(argv[2], "wb"); fwrite(w, 4, nw, f); fclose(f); free(w);
    printf("kills=%u\n", k);
    return 0;
}
'''
CAP, MEMMODEL, ENTRY, EXECMODE, DECORATE = 17, 14, 15, 16, 71
VOID, BOOL, FLOAT, VECTOR, POINTER, FUNCTYPE, CONST = 19, 20, 22, 23, 32, 33, 43
FUNCTION, LABEL, RETURN, FUNCEND, VARIABLE, LOAD, STORE, CONSTRUCT = 54, 248, 253, 56, 59, 61, 62, 80
FLT, SELMERGE, BRANCH, BRCOND, KILL, PHI = 184, 247, 249, 250, 252, 245


def ins(op, *args):
    return [(len(args) + 1) << 16 | op, *args]


def string(s):
    b = s.encode() + b'\0'
    b += b'\0' * (-len(b) % 4)
    return list(struct.unpack(f'<{len(b) // 4}I', b))


def parse(words):
    out, i = [], 5
    while i < len(words):
        n = words[i] >> 16
        out.append(list(words[i:i + n]))
        i += n
    return out


def fs(body):
    """A fragment shader: float in at location 0, vec4 out at location 0. body(ids) returns the blocks between the input's load (in the entry block, which body
    ends) and the block labelled ids['END'], which stores the output and returns."""
    V_, FT, FN, F, V4, PI, PO, IN, OUT, HALF, ONE, L, X, B, C, END, KILLB, OTHER, VEC = range(1, 20)
    w = [0x07230203, 0x00010300, 0, 40, 0]
    w += ins(CAP, 1) + ins(MEMMODEL, 0, 1)
    w += ins(ENTRY, 4, FN, *string('main'), IN, OUT) + ins(EXECMODE, FN, 7)
    w += ins(DECORATE, IN, 30, 0) + ins(DECORATE, OUT, 30, 0)
    w += ins(VOID, V_) + ins(FUNCTYPE, FT, V_) + ins(FLOAT, F, 32) + ins(VECTOR, V4, F, 4) + ins(BOOL, B)
    w += ins(POINTER, PI, 1, F) + ins(POINTER, PO, 3, V4) + ins(VARIABLE, PI, IN, 1) + ins(VARIABLE, PO, OUT, 3)
    w += ins(CONST, F, HALF, 0x3f000000) + ins(CONST, F, ONE, 0x3f800000)
    w += ins(FUNCTION, V_, FN, 0, FT) + ins(LABEL, L) + ins(LOAD, F, X, IN) + ins(FLT, B, C, X, HALF)
    ids = dict(F=F, X=X, C=C, END=END, KILLB=KILLB, OTHER=OTHER, HALF=HALF, ONE=ONE, L=L)
    w += body(ids)
    w += ins(CONSTRUCT, V4, VEC, X, X, X, ONE) + ins(STORE, OUT, VEC) + ins(RETURN) + ins(FUNCEND)
    return w


class SpvKillToMerge(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = os.environ.get('CC') or shutil.which('cc') or shutil.which('clang') or shutil.which('gcc')
        if not cc:
            raise unittest.SkipTest('no C compiler')
        cls.tmp = tempfile.TemporaryDirectory()
        d = Path(cls.tmp.name)
        (d / 'harness.c').write_text(HARNESS)
        cls.exe = d / 'harness.exe'
        subprocess.run([cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-I', str(SOURCE.parent), '-o', str(cls.exe), str(d / 'harness.c')], check=True)
        cls.val = os.environ.get('SPIRV_VAL') or shutil.which('spirv-val')

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_one(self, words):   # → (words after, kills taken out)
        d = Path(self.tmp.name)
        src, dst = d / 'in.spv', d / 'out.spv'
        src.write_bytes(struct.pack(f'<{len(words)}I', *words))
        r = subprocess.run([str(self.exe), str(src), str(dst)], capture_output=True, text=True, check=True).stdout
        data = dst.read_bytes()
        return list(struct.unpack(f'<{len(data) // 4}I', data)), int(r.strip().split('=')[1])

    def check_valid(self, words):
        if not self.val:
            return
        r = subprocess.run([self.val, '--target-env', 'vulkan1.1', '-'], input=struct.pack(f'<{len(words)}I', *words), capture_output=True)
        self.assertEqual(r.returncode, 0, r.stderr.decode() + r.stdout.decode())

    def test_kill_branch_goes_to_the_merge(self):
        def body(i):   # if (x < 0.5) discard;   the translator's form
            return ins(SELMERGE, i['END'], 0) + ins(BRCOND, i['C'], i['KILLB'], i['END']) + ins(LABEL, i['KILLB']) + ins(KILL) + ins(LABEL, i['END'])
        w = fs(body)
        self.check_valid(w)
        o, kills = self.run_one(w)
        self.assertEqual(kills, 1)
        self.check_valid(o)
        insts = parse(o)
        self.assertFalse([x for x in insts if x[0] & 0xffff == KILL])
        k = [n for n, x in enumerate(insts) if x[0] & 0xffff == LABEL and x[1] == 17][0]   # the kill block
        self.assertEqual(insts[k + 1], ins(BRANCH, 16))                                    # goes on to the merge block
        self.assertEqual(len(o), len(w) + 1)

    def test_kill_as_the_false_branch(self):
        def body(i):
            return (ins(SELMERGE, i['END'], 0) + ins(BRCOND, i['C'], i['OTHER'], i['KILLB']) + ins(LABEL, i['OTHER']) + ins(BRANCH, i['END']) +
                    ins(LABEL, i['KILLB']) + ins(KILL) + ins(LABEL, i['END']))
        o, kills = self.run_one(fs(body))
        self.assertEqual(kills, 1)
        self.check_valid(o)

    def test_merge_with_a_phi_keeps_its_kill(self):
        def body(i):   # the merge block takes a value by OpPhi from the other branch: a new predecessor would need an operand
            return (ins(SELMERGE, i['END'], 0) + ins(BRCOND, i['C'], i['KILLB'], i['OTHER']) + ins(LABEL, i['KILLB']) + ins(KILL) +
                    ins(LABEL, i['OTHER']) + ins(BRANCH, i['END']) + ins(LABEL, i['END']) + ins(PHI, i['F'], 30, i['ONE'], i['OTHER']))
        w = fs(body)
        self.check_valid(w)
        o, kills = self.run_one(w)
        self.assertEqual(kills, 0)
        self.assertEqual(o, w)

    def test_module_without_a_kill_is_left(self):
        def body(i):
            return ins(BRANCH, i['END']) + ins(LABEL, i['END'])
        w = fs(body)
        o, kills = self.run_one(w)
        self.assertEqual(kills, 0)
        self.assertEqual(o, w)


if __name__ == '__main__':
    unittest.main()
