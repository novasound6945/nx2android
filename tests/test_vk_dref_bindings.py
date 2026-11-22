"""The bindings a depth comparison reads (nxnative/dm/android/vk_spv_dref.h): stage_set gives them no integer view and vk_shader_module never retypes them, as
an integer image under OpImage*Dref* / a Dref gather is invalid SPIR-V (Luigi's Mansion 3's 32-texture shadow array on Turnip: its fragment part was refused)."""
import shutil, subprocess, tempfile, unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include <stdio.h>
#include "vk_spv_dref.h"
int main(int argc, char **argv) {   // in.spv binding...: prints the mask over the bindings given
    if (argc < 2) return 1;
    FILE *f = fopen(argv[1], "rb"); if (!f) return 1;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint32_t *w = malloc((size_t)n); if (fread(w, 1, (size_t)n, f) != (size_t)n) return 1; fclose(f);
    uint16_t b[64]; int nb = 0; for (int i = 2; i < argc && nb < 64; i++) b[nb++] = (uint16_t)atoi(argv[i]);
    printf("%#llx\n", (unsigned long long)spv_dref_bindings(w, (uint32_t)(n / 4), b, nb)); free(w); return 0;
}
'''
FRAG = r'''
#version 450
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;
layout(binding = 1) uniform sampler2D colour;                 // sampled plainly
layout(binding = 3) uniform sampler2DArrayShadow shadows[4];  // an array of shadow maps: a Dref gather through an access chain
layout(binding = 5) uniform sampler2DShadow one;              // a Dref sample
layout(binding = 7) uniform usampler2D ids;                   // integer texels, no comparison
void main() {
    o = texture(colour, uv) + textureGather(shadows[1], vec3(uv, 0.0), 0.5) + vec4(texture(one, vec3(uv, 0.5))) + vec4(texelFetch(ids, ivec2(uv), 0));
}
'''

@unittest.skipUnless(all(shutil.which(t) for t in ('cc', 'glslc')), 'a host C compiler and glslc are required')
class DrefBindings(unittest.TestCase):
    def test_mask(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)
            (p / 'h.c').write_text(HARNESS, encoding='utf-8'); (p / 'f.frag').write_text(FRAG, encoding='utf-8')
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'nxnative/dm/android'), str(p / 'h.c'), '-o', str(p / 'h')], check=True)
            for opt in ('-O0', '-O'):
                subprocess.run(['glslc', '--target-env=vulkan1.1', opt, str(p / 'f.frag'), '-o', str(p / 'f.spv')], check=True)
                r = subprocess.run([str(p / 'h'), str(p / 'f.spv'), '1', '3', '5', '7'], capture_output=True, text=True, check=True)
                self.assertEqual(r.stdout.strip(), '0x6', opt)   # indices 1 (binding 3) and 2 (binding 5)
                r = subprocess.run([str(p / 'h'), str(p / 'f.spv'), '7', '1'], capture_output=True, text=True, check=True)
                self.assertEqual(r.stdout.strip(), '0', opt)     # neither compared

if __name__ == '__main__':
    unittest.main()
