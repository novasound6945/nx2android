"""The load-time SPIR-V clean-ups (nxnative/dm/android/gfx_vk_spv_strip.inc: spv_strip_clipcull, spv_strip_caps) on hand-made modules, built with the host's C compiler.

spirv-val (on PATH, or SPIRV_VAL) checks the modules after, when it is there. SPV_STRIP_CORPUS=<dir of <key>_<stage>.spv dumps> also runs both over every module
there in the order gfx_vk_shader.c applies them (caps on every stage, clip/cull on vertex shaders) and validates each result.
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
// harness <list>: each line "<stage> <in.spv> <out.spv>"; writes out.spv (the module after both, as the runtime applies them) and prints "<in> clip=<0|1> caps=<0|1>"
int main(int argc, char **argv) {
    if (argc < 2) return 2; FILE *l = fopen(argv[1], "r"); if (!l) return 2; char in[1024], out[1024]; int stage;
    while (fscanf(l, "%d %1023s %1023s", &stage, in, out) == 3) {
        FILE *f = fopen(in, "rb"); if (!f) return 3; fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        uint32_t *w = malloc((size_t)n), nw = (uint32_t)(n / 4); if (fread(w, 1, (size_t)n, f) != (size_t)n) return 3; fclose(f);
        int caps = 0, clip = 0; uint32_t n2 = 0, *w2 = spv_strip_caps(w, nw, &n2); if (w2) { free(w); w = w2; nw = n2; caps = 1; }
        if (stage == 0) { w2 = spv_strip_clipcull(w, nw, &n2); if (w2) { free(w); w = w2; nw = n2; clip = 1; } }
        f = fopen(out, "wb"); fwrite(w, 4, nw, f); fclose(f); free(w);
        printf("%s clip=%d caps=%d\n", in, clip, caps);
    }
    return 0;
}
'''
CAP, EXT, IMPORT, MEMMODEL, ENTRY, EXECMODE, NAME, MEMBERNAME, DECORATE, MEMBERDECORATE = 17, 10, 11, 14, 15, 16, 5, 6, 71, 72
VOID, INT, FLOAT, VECTOR, ARRAY, STRUCT, POINTER, FUNCTYPE = 19, 21, 22, 23, 28, 30, 32, 33
CONST, COMPOSITE = 43, 44
FUNCTION, LABEL, RETURN, FUNCEND, VARIABLE, LOAD, STORE, CHAIN = 54, 248, 253, 56, 59, 61, 62, 65
OUTPUT = 3


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


def vs(write_clip=False, whole_store=False, extra_caps=()):
    """A vertex shader with the translator's gl_PerVertex {Position, PointSize, ClipDistance[8], CullDistance[1]} writing Position and PointSize."""
    V_, FN, F, V4, U, C8, C1, A8, A1, S, PS, PF, PV4, VAR, I0, I1, I2, F0, F1, V1, L, VL, PSV = range(1, 24)
    w = [0x07230203, 0x00010300, 0, 30, 0]
    w += ins(CAP, 1) + ins(CAP, 10)
    for c in extra_caps:
        w += ins(CAP, c)
    w += ins(MEMMODEL, 0, 1)
    w += ins(ENTRY, 0, FN, *string('main'), VAR)
    w += ins(NAME, S, *string('gl_PerVertex'))
    for m, nm in enumerate(['gl_Position', 'gl_PointSize', 'gl_ClipDistance', 'gl_CullDistance']):
        w += ins(MEMBERNAME, S, m, *string(nm))
    w += ins(MEMBERDECORATE, S, 0, 11, 0) + ins(MEMBERDECORATE, S, 0, 18)   # BuiltIn Position, Invariant
    w += ins(MEMBERDECORATE, S, 1, 11, 1) + ins(MEMBERDECORATE, S, 2, 11, 3) + ins(MEMBERDECORATE, S, 3, 11, 4)
    w += ins(DECORATE, S, 2)   # Block
    w += ins(VOID, V_) + ins(FUNCTYPE, 24, V_) + ins(FLOAT, F, 32) + ins(VECTOR, V4, F, 4) + ins(INT, U, 32, 0)
    w += ins(CONST, U, C8, 8) + ins(CONST, U, C1, 1) + ins(ARRAY, A8, F, C8) + ins(ARRAY, A1, F, C1)
    w += ins(STRUCT, S, V4, F, A8, A1) + ins(POINTER, PS, OUTPUT, S) + ins(POINTER, PF, OUTPUT, F) + ins(POINTER, PV4, OUTPUT, V4)
    w += ins(VARIABLE, PS, VAR, OUTPUT)
    w += ins(CONST, U, I0, 0) + ins(CONST, U, I1, 1) + ins(CONST, U, I2, 2) + ins(CONST, F, F0, 0) + ins(CONST, F, F1, 0x3f800000)
    w += ins(COMPOSITE, V4, V1, F0, F0, F0, F1)
    w += ins(FUNCTION, V_, FN, 0, 24) + ins(LABEL, L)
    w += ins(CHAIN, PV4, VL, VAR, I0) + ins(STORE, VL, V1)
    w += ins(CHAIN, PF, PSV, VAR, I1) + ins(STORE, PSV, F1)
    if write_clip:
        w += ins(CHAIN, PF, 25, VAR, I2, I0) + ins(STORE, 25, F0)
    if whole_store:
        w += ins(LOAD, S, 26, VAR)
    w += ins(RETURN) + ins(FUNCEND)
    return w, S


class SpvStrip(unittest.TestCase):
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

    def run_many(self, items):   # [(stage, words)] → [(words after, clip, caps)]
        d = Path(self.tmp.name)
        lines, outs = [], []
        for k, (stage, words) in enumerate(items):
            src, dst = d / f'in{k}.spv', d / f'out{k}.spv'
            src.write_bytes(struct.pack(f'<{len(words)}I', *words))
            lines.append(f'{stage} {src} {dst}')
            outs.append(dst)
        (d / 'list.txt').write_text('\n'.join(lines) + '\n')
        r = subprocess.run([str(self.exe), str(d / 'list.txt')], capture_output=True, text=True, check=True).stdout.split('\n')
        res = []
        for line, dst in zip(r, outs):
            data = dst.read_bytes()
            res.append((list(struct.unpack(f'<{len(data) // 4}I', data)), 'clip=1' in line, 'caps=1' in line))
        return res

    def check_valid(self, words):
        if not self.val:
            return
        r = subprocess.run([self.val, '--target-env', 'vulkan1.1', '-'], input=struct.pack(f'<{len(words)}I', *words), capture_output=True)
        self.assertEqual(r.returncode, 0, r.stderr.decode() + r.stdout.decode())

    def test_unused_trailing_members_dropped(self):
        w, s = vs()
        (o, clip, caps), = self.run_many([(0, w)])
        self.assertTrue(clip)
        self.assertTrue(caps)   # Float64 without a 64-bit float type
        insts = parse(o)
        st = [x for x in insts if x[0] & 0xffff == STRUCT and x[1] == s][0]
        self.assertEqual(len(st), 4)   # result id + Position + PointSize
        self.assertFalse([x for x in insts if x[0] & 0xffff in (MEMBERDECORATE, MEMBERNAME) and x[1] == s and x[2] >= 2])
        self.assertFalse([x for x in insts if x[0] & 0xffff == CAP and x[1] == 10])
        self.check_valid(o)

    def test_used_member_kept(self):
        w, s = vs(write_clip=True, extra_caps=(32,))   # ClipDistance capability: it writes gl_ClipDistance[0]
        (o, clip, _), = self.run_many([(0, w)])
        self.assertTrue(clip)   # CullDistance alone goes
        st = [x for x in parse(o) if x[0] & 0xffff == STRUCT and x[1] == s][0]
        self.assertEqual(len(st), 5)
        self.check_valid(o)

    def test_whole_block_use_left_alone(self):
        w, _ = vs(whole_store=True)
        (o, clip, _), = self.run_many([(0, w)])
        self.assertFalse(clip)

    def test_fragment_stage_and_interlock(self):
        F, FN, L, V_ = 3, 1, 2, 4
        base = [0x07230203, 0x00010300, 0, 10, 0]
        body = ins(MEMMODEL, 0, 1) + ins(ENTRY, 4, FN, *string('main')) + ins(EXECMODE, FN, 7)
        fn = ins(VOID, V_) + ins(FUNCTYPE, 5, V_) + ins(FUNCTION, V_, FN, 0, 5) + ins(LABEL, L) + ins(RETURN) + ins(FUNCEND)
        caps = ins(CAP, 1) + ins(CAP, 5378) + ins(CAP, 10) + ins(EXT, *string('SPV_EXT_fragment_shader_interlock'))
        plain = base + caps + body + fn
        moded = base + caps + body + ins(EXECMODE, FN, 5366) + fn   # PixelInterlockOrderedEXT: the capability stays
        f64 = base + caps + body + ins(FLOAT, 6, 64) + fn
        (a, _, ca), (b, _, cb), (c, _, cc) = self.run_many([(1, plain), (1, moded), (1, f64)])
        self.assertTrue(ca)
        self.assertEqual([x[1] for x in parse(a) if x[0] & 0xffff == CAP], [1])
        self.assertFalse([x for x in parse(a) if x[0] & 0xffff == EXT])
        self.check_valid(a)
        self.assertTrue(cb)   # Float64 still goes
        self.assertEqual(sorted(x[1] for x in parse(b) if x[0] & 0xffff == CAP), [1, 5378])
        self.assertTrue([x for x in parse(b) if x[0] & 0xffff == EXT])
        self.assertTrue(cc)
        self.assertEqual(sorted(x[1] for x in parse(c) if x[0] & 0xffff == CAP), [1, 10])

    def test_corpus(self):
        corpus = os.environ.get('SPV_STRIP_CORPUS')
        if not corpus:
            self.skipTest('SPV_STRIP_CORPUS not set')
        files = sorted(Path(corpus).glob('*_[0-5].spv'))
        self.assertTrue(files)
        out = Path(os.environ.get('SPV_STRIP_OUT') or self.tmp.name) / 'corpus'
        out.mkdir(parents=True, exist_ok=True)
        lst = out / 'list.txt'
        lst.write_text(''.join(f'{f.stem[-1]} {f} {out / f.name}\n' for f in files))
        r = subprocess.run([str(self.exe), str(lst)], capture_output=True, text=True, check=True).stdout.splitlines()
        self.assertEqual(len(r), len(files))
        nclip = sum('clip=1' in x for x in r)
        ncaps = sum('caps=1' in x for x in r)
        nvs = sum(f.stem.endswith('_0') for f in files)
        fails = []
        if self.val:
            for f in files:
                v = subprocess.run([self.val, '--target-env', 'vulkan1.1', str(out / f.name)], capture_output=True, text=True)
                if v.returncode:
                    fails.append((f.name, (v.stderr + v.stdout).strip().splitlines()[:2]))
        print(f'\ncorpus: {len(files)} modules ({nvs} vertex); clip/cull stripped {nclip}, capabilities stripped {ncaps}; '
              f'spirv-val {"not run" if not self.val else f"failures {len(fails)}"}')
        for name, msg in fails[:10]:
            print('  ', name, msg)
        self.assertFalse(fails)


if __name__ == '__main__':
    unittest.main()
