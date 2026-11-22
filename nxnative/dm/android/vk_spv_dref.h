// Sampled bindings a shader compares depth with (gfx_vk_shader.c vk_shader, gfx_vk_exec_descriptors.inc stage_set). The integer texel variant (gfx_vk_shader.c
// spv_int_images) retypes a binding's image to uint / int when a view of an integer format, or a stencil, is bound there; a depth comparison (OpImage*Dref*, Dref
// gather) cannot take an integer image (its result is a float compare; spirv-val: "Expected Image 'Sampled Type' to be the same as Result Type components").
// Luigi's Mansion 3 reads its shadow maps through a 32-texture array (RK_TEX_C2A); one of the 32 handles was an integer view, the whole array was retyped, and its
// Dref gathers left the fragment part invalid: Turnip refused it (-1) and the draws were left out (shaders ef07efd818bd1ad5, 314bd84c4accdf14, 4f9ca458e199a83b).
// A bit per entry of bind[] (the shader's reflection bindings by index) whose variable an image of a depth comparison comes from: through OpLoad, OpAccessChain,
// OpInBoundsAccessChain, OpCopyObject, OpSampledImage and OpImage, as spv_int_images follows them
#include <stdint.h>
#include <stdlib.h>
static uint64_t spv_dref_bindings(const uint32_t *w, uint32_t nw, const uint16_t *bind, int nb) {
    if (nw < 5 || !w[3] || w[3] > nw * 4 || nb <= 0) return 0;
    if (nb > 64) nb = 64;
    uint32_t bound = w[3]; uint8_t *src = calloc(bound, 1);   // id → 1 + the index in bind[] of the variable it comes from
    if (!src) return 0;
    uint64_t m = 0;
    for (uint32_t i = 5; i < nw;) { uint32_t n = w[i] >> 16, op = w[i] & 0xffff; if (!n || i + n > nw) break;
        if (op == 71 && n >= 4 && w[i + 2] == 33 && w[i + 1] < bound)   // OpDecorate Binding
            for (int k = 0; k < nb; k++) if (w[i + 3] == bind[k]) { src[w[i + 1]] = (uint8_t)(k + 1); break; }
        i += n; }
    for (uint32_t i = 5; i < nw;) { uint32_t n = w[i] >> 16, op = w[i] & 0xffff; if (!n || i + n > nw) break;
        if ((op == 61 || op == 65 || op == 66 || op == 83 || op == 86 || op == 100) && n >= 4 && w[i + 2] < bound && w[i + 3] < bound && src[w[i + 3]])
            src[w[i + 2]] = src[w[i + 3]];   // Load, AccessChain, InBoundsAccessChain, CopyObject, SampledImage (its image), Image
        else if ((op == 89 || op == 90 || op == 93 || op == 94 || op == 97 || op == 307 || op == 308 || op == 311 || op == 312 || op == 315) && n >= 4 && w[i + 3] < bound && src[w[i + 3]])
            m |= 1ull << (src[w[i + 3]] - 1);   // Sample(Proj)Dref{Implicit,Explicit}Lod, DrefGather and their sparse forms: the sampled image
        i += n; }
    free(src); return m;
}
