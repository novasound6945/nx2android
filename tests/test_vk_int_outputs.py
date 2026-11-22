"""The integer colour target variant (nxnative/dm/android/vk_spv_fragout.h): float outputs into UINT / SINT targets retyped, stores and loads cast, spirv-val clean;
and spv_int_writer, which picks the shaders whose integer output part the shader preparation makes (written from integer bits, or nothing but constants)."""
import re, shutil, subprocess, tempfile, unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include <stdio.h>
#include "vk_spv_fragout.h"
int main(int argc, char **argv) {   // in.spv out.spv umask smask: prints the float output locations, exit 2 when no variant
    if (argc != 5) return 1;
    FILE *f = fopen(argv[1], "rb"); if (!f) return 1;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint32_t *w = malloc((size_t)n); if (fread(w, 1, (size_t)n, f) != (size_t)n) return 1; fclose(f);
    uint32_t fl = spv_float_out_locs(w, (uint32_t)(n / 4)), no = 0;
    uint32_t *o = spv_int_outputs(w, (uint32_t)(n / 4), (uint32_t)strtoul(argv[3], 0, 0) & fl, (uint32_t)strtoul(argv[4], 0, 0) & fl, &no);
    printf("%#x\n", fl); free(w);
    if (!o) return 2;
    f = fopen(argv[2], "wb"); fwrite(o, 4, no, f); fclose(f); free(o); return 0;
}
'''
FRAG = r'''
#version 450
layout(location = 0) in vec4 v;
layout(location = 0) out vec4 o0;    // a UINT target: written from integer bits, and read back
layout(location = 1) out vec4 o1;    // a SINT target: component stores
layout(location = 2) out vec4 o2;    // a float target: left as it is
layout(location = 3) out uvec4 o3;   // integer already: left as it is
void main() {
    o0 = uintBitsToFloat(uvec4(7u, 8u, floatBitsToUint(v.x), 0xffffffffu));
    o0.y += v.y;
    o1.x = intBitsToFloat(-5); o1.yzw = v.yzw;
    o2 = v;
    o3 = uvec4(1u);
}
'''

WRITER = r'''
#include <stdio.h>
#include "vk_spv_fragout.h"
int main(int argc, char **argv) {   // in.spv: prints spv_int_writer
    (void)spv_float_out_locs; (void)spv_int_outputs;   // (the header's other functions, unused here)
    if (argc != 2) return 1;
    FILE *f = fopen(argv[1], "rb"); if (!f) return 1;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint32_t *w = malloc((size_t)n); if (fread(w, 1, (size_t)n, f) != (size_t)n) return 1; fclose(f);
    printf("%d\n", spv_int_writer(w, (uint32_t)(n / 4))); free(w); return 0;
}
'''
WRITERS = {   # GLSL body → spv_int_writer
    'o0.x = uintBitsToFloat(floatBitsToUint(v.x) | 1u); o0.yzw = v.yzw;': 1,   # one component from integer bits
    'o0 = uintBitsToFloat(uvec4(floatBitsToUint(v.x), 2u, 3u, 4u));': 1,        # the whole vector
    'o0 = vec4(0.0); o1 = vec4(1.0, 0.0, 0.0, 1.0);': 1,                      # nothing but constants (a clear)
    'o0 = v; o1 = v * 2.0;': 0,                                                # float colour
    'o0 = vec4(0.0); o1 = v;': 0,                                              # a constant and a float
}

def need(*tools): return all(shutil.which(t) for t in tools)

@unittest.skipUnless(need('cc', 'glslc', 'spirv-val', 'spirv-dis'), 'a host C compiler, glslc and SPIRV-Tools are required')
class IntOutputs(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(); p = cls.p = Path(cls.tmp.name)
        (p / 'h.c').write_text(HARNESS, encoding='utf-8'); (p / 'f.frag').write_text(FRAG, encoding='utf-8')
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'nxnative/dm/android'), str(p / 'h.c'), '-o', str(p / 'h')], check=True)
        subprocess.run(['glslc', '--target-env=vulkan1.1', '-O0', str(p / 'f.frag'), '-o', str(p / 'f.spv')], check=True)
        (p / 'w.c').write_text(WRITER, encoding='utf-8')
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'nxnative/dm/android'), str(p / 'w.c'), '-o', str(p / 'w')], check=True)

    @classmethod
    def tearDownClass(cls): cls.tmp.cleanup()

    def run_h(self, um, sm):
        out = self.p / f'o_{um}_{sm}.spv'
        r = subprocess.run([str(self.p / 'h'), str(self.p / 'f.spv'), str(out), hex(um), hex(sm)], capture_output=True, text=True)
        return r, out

    def outputs(self, spv):   # location → the output variable's pointee type name, as spirv-dis names it
        d = subprocess.run(['spirv-dis', '--raw-id', str(spv)], capture_output=True, text=True, check=True).stdout
        loc = {m[0]: int(m[1]) for m in re.findall(r'OpDecorate %(\d+) Location (\d+)', d)}
        ptr = dict(re.findall(r'%(\d+) = OpTypePointer Output %(\d+)', d))
        types = {m[0]: m[1] for m in re.findall(r'%(\d+) = (OpType\w+(?: %?\w+)*)', d)}
        res = {}
        for var, pt in re.findall(r'%(\d+) = OpVariable %(\d+) Output', d):
            if var in loc: res[loc[var]] = types[ptr[pt]]
        def name(t): return re.sub(r'%(\d+)', lambda m: '(' + name(types[m[1]]) + ')', t)
        return {l: name(t) for l, t in res.items()}, d

    def test_retypes_only_integer_locations(self):
        r, out = self.run_h(0b1001, 0b0010)   # UINT at 0 and 3, SINT at 1
        self.assertEqual(r.returncode, 0, r.stderr); self.assertEqual(r.stdout.strip(), '0x7')   # location 3 is uint already: not a float output
        subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', str(out)], check=True)
        before, _ = self.outputs(self.p / 'f.spv'); after, dis = self.outputs(out)
        self.assertEqual(after[0], 'OpTypeVector (OpTypeInt 32 0) 4')
        self.assertEqual(after[1], 'OpTypeVector (OpTypeInt 32 1) 4')
        self.assertEqual(after[2], before[2]); self.assertEqual(after[3], before[3])
        self.assertIn('OpBitcast', dis)

    def test_nothing_to_do(self):
        r, _ = self.run_h(0, 0); self.assertEqual(r.returncode, 2)
        r, _ = self.run_h(0b1000, 0); self.assertEqual(r.returncode, 2)   # only the location that is integer already

    def test_float_location_untouched_alone(self):
        r, out = self.run_h(0, 0b0100)   # a SINT target at the float location 2
        self.assertEqual(r.returncode, 0, r.stderr)
        subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', str(out)], check=True)
        before, _ = self.outputs(self.p / 'f.spv'); after, _ = self.outputs(out)
        self.assertEqual(after[2], 'OpTypeVector (OpTypeInt 32 1) 4')
        self.assertEqual({l: t for l, t in after.items() if l != 2}, {l: t for l, t in before.items() if l != 2})

    def test_int_writer(self):
        for i, (body, want) in enumerate(WRITERS.items()):
            src = self.p / f'w{i}.frag'; spv = self.p / f'w{i}.spv'
            src.write_text('#version 450\nlayout(location = 0) in vec4 v;\nlayout(location = 0) out vec4 o0;\nlayout(location = 1) out vec4 o1;\nvoid main() { ' + body + ' }\n', encoding='utf-8')
            subprocess.run(['glslc', '--target-env=vulkan1.1', '-O0', str(src), '-o', str(spv)], check=True)
            r = subprocess.run([str(self.p / 'w'), str(spv)], capture_output=True, text=True, check=True)
            self.assertEqual(r.stdout.strip(), str(want), body)
        r = subprocess.run([str(self.p / 'w'), str(self.p / 'f.spv')], capture_output=True, text=True, check=True)
        self.assertEqual(r.stdout.strip(), '1')   # FRAG: o0 from integer bits

if __name__ == '__main__': unittest.main()
