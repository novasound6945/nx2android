"""NonUniform on run-time indexed texture arrays (nxnative/dm/android/vk_spv_nonuniform.h): an access chain into an array of sampled images with an index that is
not a constant is decorated NonUniform, with every value made from it, and the module stays valid (Luigi's Mansion 3 picks a shadow map per pixel from its 32-texture
driver-table array). A constant index, a texel buffer array and a module without such arrays are left alone."""
import shutil, subprocess, tempfile, unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include <stdio.h>
#include "vk_spv_nonuniform.h"
int main(int argc, char **argv) {   // in.spv out.spv: prints the access chains decorated (0: no module made)
    if (argc < 3) return 1;
    FILE *f = fopen(argv[1], "rb"); if (!f) return 1;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint32_t *w = malloc((size_t)n); if (fread(w, 1, (size_t)n, f) != (size_t)n) return 1; fclose(f);
    uint32_t no = 0, nd = 0, *o = spv_nonuniform_arrays(w, (uint32_t)(n / 4), &no, &nd);
    printf("%u\n", nd);
    if (o) { f = fopen(argv[2], "wb"); if (!f) return 1; fwrite(o, 4, no, f); fclose(f); free(o); }
    free(w); return 0;
}
'''
DYNAMIC = r'''
#version 450
layout(location = 0) in vec2 uv;
layout(location = 1) flat in int which;
layout(location = 0) out vec4 o;
layout(binding = 1) uniform sampler2DArrayShadow shadows[32];   // indexed per pixel (a light's shadow map)
layout(binding = 2) uniform sampler2D plain;
void main() {
    int i = which & 31;
    o = textureGather(shadows[i], vec3(uv, 0.0), 0.5) + vec4(textureSize(shadows[i], 0).x) + texture(plain, uv);
}
'''
CONSTANT = r'''
#version 450
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;
layout(binding = 1) uniform sampler2D tex[4];
layout(binding = 2) uniform samplerBuffer bufs[4];
layout(location = 1) flat in int which;
void main() { o = texture(tex[2], uv) + texelFetch(bufs[which & 3], 0); }
'''

@unittest.skipUnless(all(shutil.which(t) for t in ('cc', 'glslc', 'spirv-val', 'spirv-dis')), 'a host C compiler, glslc and SPIRV-Tools are required')
class NonUniformArrays(unittest.TestCase):
    def run_one(self, d, src, opt):
        (d / 'f.frag').write_text(src, encoding='utf-8')
        subprocess.run(['glslc', '--target-env=vulkan1.1', opt, str(d / 'f.frag'), '-o', str(d / 'f.spv')], check=True)
        out = d / 'o.spv'
        if out.exists(): out.unlink()
        r = subprocess.run([str(d / 'h'), str(d / 'f.spv'), str(out)], capture_output=True, text=True, check=True)
        return int(r.stdout.strip()), out

    def test_dynamic_and_constant(self):
        with tempfile.TemporaryDirectory() as t:
            d = Path(t)
            (d / 'h.c').write_text(HARNESS, encoding='utf-8')
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'nxnative/dm/android'), str(d / 'h.c'), '-o', str(d / 'h')], check=True)
            for opt in ('-O0', '-O'):
                n, out = self.run_one(d, DYNAMIC, opt)
                self.assertGreaterEqual(n, 1, opt)
                subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', str(out)], check=True)
                dis = subprocess.run(['spirv-dis', str(out)], capture_output=True, text=True, check=True).stdout
                self.assertIn('OpCapability ShaderNonUniform', dis)
                self.assertIn('OpCapability SampledImageArrayNonUniformIndexing', dis)
                self.assertIn('OpExtension "SPV_EXT_descriptor_indexing"', dis)
                self.assertGreaterEqual(dis.count(' NonUniform\n'), 2 * n, opt)   # the access chains and the images loaded through them
                n, out = self.run_one(d, CONSTANT, opt)   # a constant index; a texel buffer array (another capability): untouched
                self.assertEqual(n, 0, opt); self.assertFalse(out.exists(), opt)

if __name__ == '__main__':
    unittest.main()
