// Integer colour targets (gfx_vk_shader.c vk_shader_module, fd bits 8-15 / 16-23). The translator types every fragment output float (the Switch shader has no output
// types: Maxwell writes an output register's 32 bits to the target as they are, and a shader that means an integer stores the float with its bits). Vulkan takes a
// colour attachment's numeric type from the output's: a float output into a UINT / SINT attachment gives poison values (Fragment Output Interface; on Adreno /
// Turnip garbage or black). Luigi's Mansion 3 draws its scene into an R32Uint + RGBA8Uint G-buffer from about 1,400 such shaders; Metal refuses those pipelines and
// upstream gives them a second entry point (dmsh_build.py fs_uint, nvn_mtl.m pso_all_uint) that returns the same bits as uint (as_type: nothing converted).
// This is the Vulkan counterpart, made per pipeline from the attachments' formats: the 32-bit float outputs at the locations in umask (UINT targets) / smask (SINT
// targets) become the unsigned / signed integer type of the same width and component count, every store into them (the variable or a component through an access
// chain) stores OpBitcast of the float value, and every load from them loads the integer and casts back. Outputs already integer, and the other locations, are left
// as they are. NULL when no output is to change, or the module uses such an output in a way this does not rewrite (a function argument, a copy, a pointer select)
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef struct { uint32_t op, a, b; } spv_otype;   // 21 int (width, signedness), 22 float (width, 0), 23 vector (component, count), 32 pointer (storage class, pointee)
// a bit per Location of the Output variables whose type is a 32-bit float scalar or vector: the outputs spv_int_outputs can retype
static uint32_t spv_float_out_locs(const uint32_t *w, uint32_t nw) {
    if (nw < 5 || !w[3] || w[3] > nw * 4) return 0;
    uint32_t bound = w[3], fn = nw, m = 0;
    spv_otype *ty = calloc(bound, sizeof *ty); uint32_t *loc = calloc(bound, 4);
    if (!ty || !loc) { free(ty); free(loc); return 0; }
    for (uint32_t i = 5; i < nw;) { uint32_t n = w[i] >> 16, op = w[i] & 65535;
        if (!n || i + n > nw) { m = 0; goto done; }
        if (op == 54) { fn = i; break; }
        if ((op == 21 || op == 23 || op == 32) && n == 4 && w[i + 1] < bound) ty[w[i + 1]] = (spv_otype){op, w[i + 2], w[i + 3]};
        if (op == 22 && n >= 3 && w[i + 1] < bound) ty[w[i + 1]] = (spv_otype){22, w[i + 2], 0};
        if (op == 71 && n == 4 && w[i + 2] == 30 && w[i + 1] < bound && w[i + 3] < 32) loc[w[i + 1]] = 1u << w[i + 3];
        i += n; }
    for (uint32_t i = 5; i < fn;) { uint32_t n = w[i] >> 16, op = w[i] & 65535;
        if (op == 59 && n >= 4 && w[i + 3] == 3 && w[i + 2] < bound && loc[w[i + 2]] && w[i + 1] < bound && ty[w[i + 1]].op == 32 && ty[w[i + 1]].b < bound) {
            spv_otype t = ty[ty[w[i + 1]].b]; if (t.op == 23 && t.a < bound) t = ty[t.a];
            if (t.op == 22 && t.a == 32) m |= loc[w[i + 2]]; }
        i += n; }
done:
    free(ty); free(loc); return m;
}
// the type t with its 32-bit float scalar made int of signedness sgn (a vector's component, a pointer's pointee): an existing declaration, else a new one appended to dec
static uint32_t spv_oint(uint32_t t, uint32_t sgn, spv_otype *ty, uint32_t *bound, uint32_t cap, uint32_t *dec, uint32_t *nd) {
    if (t >= *bound) return 0;
    spv_otype k = ty[t];
    if (k.op == 22 && k.a == 32) k = (spv_otype){21, 32, sgn};
    else if (k.op == 23) { k.a = spv_oint(k.a, sgn, ty, bound, cap, dec, nd); if (!k.a) return 0; }
    else if (k.op == 32) { k.b = spv_oint(k.b, sgn, ty, bound, cap, dec, nd); if (!k.b) return 0; }
    else return 0;
    for (uint32_t i = 1; i < *bound; i++) if (ty[i].op == k.op && ty[i].a == k.a && ty[i].b == k.b) return i;   // (no duplicate non-aggregate types: the existing one)
    if (*bound >= cap) return 0;
    uint32_t id = (*bound)++; ty[id] = k;
    dec[(*nd)++] = 4u << 16 | k.op; dec[(*nd)++] = id; dec[(*nd)++] = k.a; dec[(*nd)++] = k.b;
    return id;
}
static uint32_t *spv_int_outputs(const uint32_t *w, uint32_t nw, uint32_t umask, uint32_t smask, uint32_t *onw) {
    if (nw < 5 || !(umask | smask) || !w[3] || w[3] > nw * 4) return NULL;
    enum { EXTRA = 64 };   // new type ids at most (each output: int, vector, two pointers; access chains reuse them)
    uint32_t old = w[3], cap = old + EXTRA, bound = old, fn = nw, nd = 0, ng = 0, no = 5, nchg = 0;
    spv_otype *ty = calloc(cap, sizeof *ty);
    uint32_t *loc = calloc(old, 4), *chg = calloc(old, 4), *dec = malloc(EXTRA * 4 * 4), *globals = malloc((size_t)nw * 4);
    uint32_t nmem = 0;   // the stores and loads through a changed pointer: each takes a cast of 4 words
    uint32_t *out = NULL;
    if (!ty || !loc || !chg || !dec || !globals) goto fail;
    for (uint32_t i = 5; i < nw;) { uint32_t n = w[i] >> 16, op = w[i] & 65535;
        if (!n || i + n > nw) goto fail;
        if (op == 54) { fn = i; break; }
        if ((op == 21 || op == 23 || op == 32) && n == 4 && w[i + 1] < old) ty[w[i + 1]] = (spv_otype){op, w[i + 2], w[i + 3]};
        if (op == 22 && n >= 3 && w[i + 1] < old) ty[w[i + 1]] = (spv_otype){22, w[i + 2], 0};
        if (op == 71 && n == 4 && w[i + 2] == 30 && w[i + 1] < old && w[i + 3] < 32) loc[w[i + 1]] = 1u << w[i + 3];   // Location
        i += n; }
    // the Output variables to retype (the globals move after the new declarations, as vk_spv_vertex.h does)
    for (uint32_t i = 5; i < fn;) { uint32_t n = w[i] >> 16, op = w[i] & 65535;
        if (op == 59) { memcpy(globals + ng, w + i, n * 4);
            uint32_t v = w[i + 2], l = v < old ? loc[v] : 0;
            if (n >= 4 && w[i + 3] == 3 && (l & (umask | smask)) && w[i + 1] < old && ty[w[i + 1]].op == 32 && ty[w[i + 1]].b < old) {
                spv_otype t = ty[ty[w[i + 1]].b]; if (t.op == 23 && t.a < old) t = ty[t.a];
                if (t.op == 22 && t.a == 32) { uint32_t p = spv_oint(w[i + 1], (l & smask) ? 1 : 0, ty, &bound, cap, dec, &nd); if (!p) goto fail; globals[ng + 1] = p; chg[v] = p; nchg++; } }
            ng += n; }
        i += n; }
    if (!nchg) goto fail;
    // access chains into them take the integer pointer type too; uses this does not rewrite give up
    for (uint32_t i = fn; i < nw;) { uint32_t n = w[i] >> 16, op = w[i] & 65535;
        if (!n || i + n > nw) goto fail;
        if ((op == 65 || op == 66) && n >= 4 && w[i + 3] < old && chg[w[i + 3]]) {   // OpAccessChain / OpInBoundsAccessChain
            spv_otype bp = ty[chg[w[i + 3]]]; spv_otype bt = ty[bp.b]; uint32_t sgn = bt.op == 23 ? ty[bt.a].b : bt.b;
            uint32_t p = spv_oint(w[i + 1], sgn, ty, &bound, cap, dec, &nd); if (!p || w[i + 2] >= old) goto fail;
            chg[w[i + 2]] = p; }
        else if ((op == 62 && n >= 3 && w[i + 1] < old && chg[w[i + 1]]) || (op == 61 && n >= 4 && w[i + 3] < old && chg[w[i + 3]])) nmem++;   // OpStore / OpLoad
        else if (op == 57 || op == 63 || op == 64 || op == 67 || op == 70 || op == 83 || op == 169 || op == 245) {   // a call, a copy, a pointer access chain, a copy or select of a pointer, a phi
            for (uint32_t j = 1; j < n; j++) if (w[i + j] < old && chg[w[i + j]]) goto fail; }
        i += n; }
    out = malloc(((size_t)nw + nd + 4 * (size_t)nmem) * 4);
    if (!out) goto fail;
    memcpy(out, w, 20);
    for (uint32_t i = 5; i < fn;) { uint32_t n = w[i] >> 16; if ((w[i] & 65535) != 59) { memcpy(out + no, w + i, n * 4); no += n; } i += n; }
    memcpy(out + no, dec, nd * 4); no += nd;
    memcpy(out + no, globals, ng * 4); no += ng;
    for (uint32_t i = fn; i < nw;) { uint32_t n = w[i] >> 16, op = w[i] & 65535;
        if ((op == 65 || op == 66) && w[i + 3] < old && chg[w[i + 3]]) { memcpy(out + no, w + i, n * 4); out[no + 1] = chg[w[i + 2]]; no += n; }
        else if (op == 62 && n >= 3 && w[i + 1] < old && chg[w[i + 1]]) {   // OpBitcast %int %t %value; OpStore %ptr %t
            uint32_t t = bound++; out[no++] = 4u << 16 | 124; out[no++] = ty[chg[w[i + 1]]].b; out[no++] = t; out[no++] = w[i + 2];
            memcpy(out + no, w + i, n * 4); out[no + 2] = t; no += n; }
        else if (op == 61 && n >= 4 && w[i + 3] < old && chg[w[i + 3]]) {   // OpLoad %int %t %ptr; OpBitcast %float %result %t
            uint32_t t = bound++; memcpy(out + no, w + i, n * 4); out[no + 1] = ty[chg[w[i + 3]]].b; out[no + 2] = t; no += n;
            out[no++] = 4u << 16 | 124; out[no++] = w[i + 1]; out[no++] = w[i + 2]; out[no++] = t; }
        else { memcpy(out + no, w + i, n * 4); no += n; }
        i += n; }
    out[3] = bound; *onw = no;
    free(ty); free(loc); free(chg); free(dec); free(globals); return out;
fail:
    free(ty); free(loc); free(chg); free(dec); free(globals); free(out); return NULL;
}
