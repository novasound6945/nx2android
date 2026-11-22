// NonUniform for run-time indexed arrays of sampled images (gfx_vk_shader.c vk_shader). Maxwell reads a texture handle per thread, so the translator's texture arrays
// (RK_TEX_C2A: the driver's handle table, indexed at run time) may be indexed differently by the invocations of one draw. Vulkan requires such an index to be
// dynamically uniform unless the access is decorated NonUniform (with shaderSampledImageArrayNonUniformIndexing); otherwise the result is undefined, and a GPU may
// read another element's descriptor for the whole wave. Luigi's Mansion 3's lit pixel shaders (314bd84c4accdf14, 4f9ca458e199a83b, ef07efd818bd1ad5) pick a shadow
// map per light from a bit mask read out of a 3D light grid at each pixel: the index differs across a tile's edges.
// spv_nonuniform_arrays decorates NonUniform every OpAccessChain / OpInBoundsAccessChain into a UniformConstant array (or runtime array) of sampled images or combined
// image samplers (not texel buffers) whose index is not a constant, and every value made from it (OpLoad, OpCopyObject, OpSampledImage, OpImage), as GLSL's
// nonuniformEXT does; it declares ShaderNonUniform and SampledImageArrayNonUniformIndexing, and SPV_EXT_descriptor_indexing before SPIR-V 1.5.
// Returns a new module (free it) or NULL when nothing is indexed so; *n_out its words, *n_dec the access chains decorated
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
static uint32_t *spv_nonuniform_arrays(const uint32_t *w, uint32_t nw, uint32_t *n_out, uint32_t *n_dec) {
    *n_out = 0; if (n_dec) *n_dec = 0;
    if (nw < 5 || !w[3] || w[3] > (1u << 24)) return NULL;
    uint32_t bound = w[3];
    uint8_t *k = calloc(bound, 1);   // id → bit 0 a sampled image type (OpTypeImage Sampled 1, not Buffer; OpTypeSampledImage), 1 an array of one, 2 a pointer to such an
                                     // array, 3 a variable of one, 4 a constant, 5 NonUniform (decorated or to be), 6 already decorated NonUniform
    if (!k) return NULL;
    uint32_t ndec = 0, nu = 0, ins_dec = 0, ins_cap = 0, first_fn = 0, have_ext = 0, have_cap1 = 0, have_cap2 = 0;
    for (uint32_t i = 5; i < nw;) { uint32_t n = w[i] >> 16, op = w[i] & 0xffff; if (!n || i + n > nw) break;
        if (op == 17 && n == 2) { ins_cap = i + n; if (w[i + 1] == 5301) have_cap1 = 1; if (w[i + 1] == 5307) have_cap2 = 1; }   // OpCapability ShaderNonUniform / SampledImageArrayNonUniformIndexing
        else if (op == 10 && n >= 8 && !memcmp(&w[i + 1], "SPV_EXT_descriptor_indexing", 28)) have_ext = 1;
        else if (op == 25 && n >= 9 && w[i + 1] < bound && w[i + 7] == 1 && w[i + 3] != 5) k[w[i + 1]] |= 1;   // OpTypeImage, Sampled 1, Dim not Buffer
        else if (op == 27 && n >= 3 && w[i + 1] < bound && w[i + 2] < bound && (k[w[i + 2]] & 1)) k[w[i + 1]] |= 1;   // OpTypeSampledImage
        else if ((op == 28 || op == 29) && n >= 3 && w[i + 1] < bound && w[i + 2] < bound && (k[w[i + 2]] & 1)) k[w[i + 1]] |= 2;   // OpTypeArray / OpTypeRuntimeArray
        else if (op == 32 && n >= 4 && w[i + 1] < bound && w[i + 3] < bound && w[i + 2] == 0 && (k[w[i + 3]] & 2)) k[w[i + 1]] |= 4;   // OpTypePointer UniformConstant
        else if (op == 59 && n >= 4 && w[i + 1] < bound && w[i + 2] < bound && w[i + 3] == 0 && (k[w[i + 1]] & 4)) k[w[i + 2]] |= 8;   // OpVariable UniformConstant
        else if ((op == 43 || op == 50 || op == 41 || op == 42 || op == 48 || op == 49) && n >= 3 && w[i + 2] < bound) k[w[i + 2]] |= 16;   // OpConstant(True/False), OpSpecConstant(...)
        else if (op == 71 && n >= 3 && w[i + 1] < bound && w[i + 2] == 5300) k[w[i + 1]] |= 64;   // OpDecorate NonUniform
        if (op == 71 || op == 72 || op == 73 || op == 74 || op == 75 || op == 332 || op == 5632 || op == 5633) { if (!first_fn) ins_dec = i + n; }
        if (op == 54 && !first_fn) first_fn = i;   // OpFunction
        i += n; }
    if (!first_fn || !ins_cap) { free(k); return NULL; }
    if (!ins_dec) {   // no annotation: before the first type declaration
        for (uint32_t i = 5; i < first_fn;) { uint32_t n = w[i] >> 16, op = w[i] & 0xffff; if (!n) break; if (op >= 19 && op <= 39) { ins_dec = i; break; } i += n; }
        if (!ins_dec) { free(k); return NULL; } }
    for (int pass = 0; pass < 2; pass++)   // (two passes: a use laid out before its definition, through a loop's back edge)
        for (uint32_t i = first_fn; i < nw;) { uint32_t n = w[i] >> 16, op = w[i] & 0xffff; if (!n || i + n > nw) break;
            if ((op == 65 || op == 66) && n >= 5 && w[i + 2] < bound && w[i + 3] < bound && (k[w[i + 3]] & 8) && w[i + 4] < bound && !(k[w[i + 4]] & 16)) {   // Access chain into such a variable, non-constant index
                if (!(k[w[i + 2]] & 32)) { k[w[i + 2]] |= 32; nu++; ndec++; } }
            else if ((op == 61 || op == 83 || op == 100) && n >= 4 && w[i + 2] < bound && w[i + 3] < bound && (k[w[i + 3]] & 32)) { if (!(k[w[i + 2]] & 32)) { k[w[i + 2]] |= 32; nu++; } }   // Load, CopyObject, Image
            else if (op == 86 && n >= 5 && w[i + 2] < bound && w[i + 3] < bound && w[i + 4] < bound && ((k[w[i + 3]] | k[w[i + 4]]) & 32)) { if (!(k[w[i + 2]] & 32)) { k[w[i + 2]] |= 32; nu++; } }   // SampledImage
            i += n; }
    if (!ndec) { free(k); return NULL; }
    uint32_t add_dec = 0; for (uint32_t id = 1; id < bound; id++) if ((k[id] & 32) && !(k[id] & 64)) add_dec++;
    uint32_t extw = have_ext || w[1] >= 0x10500 ? 0 : 1 + 7;   // OpExtension "SPV_EXT_descriptor_indexing" (27 bytes and a terminator: 7 words)
    uint32_t capw = (have_cap1 ? 0 : 2) + (have_cap2 ? 0 : 2);
    uint32_t *o = malloc(((size_t)nw + capw + extw + 3 * add_dec) * 4); if (!o) { free(k); return NULL; }
    uint32_t m = 0;
    memcpy(o, w, (size_t)ins_cap * 4); m = ins_cap;
    if (!have_cap1) { o[m++] = 2u << 16 | 17; o[m++] = 5301; }
    if (!have_cap2) { o[m++] = 2u << 16 | 17; o[m++] = 5307; }
    if (extw) { o[m++] = 8u << 16 | 10; char s[28] = "SPV_EXT_descriptor_indexing"; memcpy(&o[m], s, 28); m += 7; }
    memcpy(o + m, w + ins_cap, (size_t)(ins_dec - ins_cap) * 4); m += ins_dec - ins_cap;
    for (uint32_t id = 1; id < bound; id++) if ((k[id] & 32) && !(k[id] & 64)) { o[m++] = 3u << 16 | 71; o[m++] = id; o[m++] = 5300; }
    memcpy(o + m, w + ins_dec, (size_t)(nw - ins_dec) * 4); m += nw - ins_dec;
    free(k); *n_out = m; if (n_dec) *n_dec = ndec; (void)nu;
    return o;
}
