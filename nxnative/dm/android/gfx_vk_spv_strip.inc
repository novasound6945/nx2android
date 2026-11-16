// Included by gfx_vk_shader.c (and by tests/test_vk_spv_strip.py's host harness): two SPIR-V clean-ups made once when a module is loaded, so its base module and
// every variant (spv_add_outputs, spv_int_images, spv_alpha_test ... all start from vkshader_t.spv) carry them. Needs <stdint.h>, <stdlib.h>, <string.h> only.
//
// spv_strip_clipcull: the translator declares every vertex shader's output block as gl_PerVertex {Position, PointSize, ClipDistance[8], CullDistance[1]} and
// writes only the first two. 8 + 1 = 9 is over Adreno's maxCombinedClipAndCullDistances (8): Dark Souls Remastered's linked pipelines on Turnip (Adreno 740)
// drew garbage colours with fast-linked libraries and crashed the driver's link-time optimisation (nir_opt_varyings_bulk). The unused trailing ClipDistance /
// CullDistance members of a non-arrayed Output block are dropped (the struct loses them, with their OpMemberDecorate / OpMemberName); their array and float types
// stay declared, unused (valid). A member is used when an access chain on a variable of the block takes it as its first index; any other use of the block (a load
// or store of the whole block, a copy, a pointer passed to a function, a non-constant first index) leaves the module as it is.
// NULL when nothing is dropped. Only for vertex shaders of a bundle without geometry / tessellation stages (gfx_vk_shader.c spv_xf): a later stage's gl_in
// block still declaring the members would no longer match.
//
// spv_strip_caps: the translator declares Float64 in every module and FragmentShaderPixelInterlockEXT in every pixel shader. Float64 goes when no 64-bit float type
// is declared; the interlock capabilities (pixel, sample, shading rate) when no interlock execution mode (5366-5371) and no Begin/EndInvocationInterlockEXT
// (5364 / 5365) is in the module, and then OpExtension "SPV_EXT_fragment_shader_interlock" too. NULL when nothing is dropped.

static uint32_t spv_strip_fn(const uint32_t *w, uint32_t nw) {   // the first OpFunction's word index (nw if none); 0 when an instruction is malformed
    for (uint32_t i = 5; i < nw;) { uint32_t n = w[i] >> 16; if (!n || i + n > nw) return 0; if ((w[i] & 0xffff) == 54) return i; i += n; }
    return nw;
}
static int spv_strip_lit(uint32_t op, uint32_t m) {   // word m of a function-body instruction is a literal, not an id (the ones the translator emits: a literal
    // that equals the block variable's id would otherwise read as a use; Dark Souls Remastered's Fma, GLSL.std.450 instruction 50, did in 2 of 391 vertex shaders)
    switch (op) {
    case 12: return m == 4;           // OpExtInst: the set's instruction number
    case 79: return m >= 5;           // OpVectorShuffle: components
    case 81: return m >= 4;           // OpCompositeExtract: indices
    case 82: return m >= 5;           // OpCompositeInsert: indices
    case 8: return m >= 2;            // OpLine: line, column
    case 246: case 247: return m >= 2 + (op == 246);   // OpLoopMerge / OpSelectionMerge: control
    case 251: return m >= 3 && (m & 1);    // OpSwitch: literal, label pairs after the default (32-bit selectors)
    case 61: return m >= 4;           // OpLoad: memory operands
    case 62: return m >= 3;           // OpStore: memory operands
    default: return 0;
    }
}
static uint32_t *spv_strip_clipcull(const uint32_t *w, uint32_t nw, uint32_t *onw) {
    if (nw < 6 || w[0] != 0x07230203) return NULL;
    uint32_t bound = w[3], fn = spv_strip_fn(w, nw); if (!fn || bound > (1u << 22)) return NULL;
    // per struct id: member count, its builtin per member (0 none, 3 ClipDistance, 4 CullDistance, 1 another), keep = members kept, bad = any use we don't follow
    enum { MAXS = 8, MAXM = 32 };
    struct { uint32_t id, nm, keep, bad, nptr, ptr[4]; uint8_t bi[MAXM]; } st[MAXS]; int ns = 0;
    for (uint32_t i = 5; i < fn; i += w[i] >> 16) {   // OpMemberDecorate s m BuiltIn b
        uint32_t n = w[i] >> 16, op = w[i] & 0xffff;
        if (op == 72 && n == 5 && w[i + 3] == 11 && (w[i + 4] == 3 || w[i + 4] == 4)) {
            int k = 0; while (k < ns && st[k].id != w[i + 1]) k++;
            if (k == ns) { if (ns == MAXS) return NULL; memset(&st[ns], 0, sizeof st[ns]); st[ns++].id = w[i + 1]; }
            if (w[i + 2] < MAXM) st[k].bi[w[i + 2]] = (uint8_t)w[i + 4]; else st[k].bad = 1; }
    }
    if (!ns) return NULL;
    for (int k = 0; k < ns; k++) {
        int found = 0;
        for (uint32_t i = 5; i < fn; i += w[i] >> 16) { uint32_t n = w[i] >> 16, op = w[i] & 0xffff;
            if (op == 30 && w[i + 1] == st[k].id) { found = 1; st[k].nm = n - 2; if (st[k].nm > MAXM) st[k].bad = 1; }
            else if (op == 72 && n >= 4 && w[i + 1] == st[k].id && w[i + 3] == 11 && w[i + 2] < MAXM && !st[k].bi[w[i + 2]]) st[k].bi[w[i + 2]] = 1;
            else if (op == 32 && n == 4 && w[i + 3] == st[k].id) { if (w[i + 2] != 3 || st[k].nptr == 4) st[k].bad = 1; else st[k].ptr[st[k].nptr++] = w[i + 1]; }   // Output pointers only
            else if ((op == 28 || op == 29) && w[i + 2] == st[k].id) st[k].bad = 1;   // an arrayed block (tessellation control output, or an input)
            else if (op == 30 && w[i + 1] != st[k].id) { for (uint32_t m = 2; m < n; m++) if (w[i + m] == st[k].id) st[k].bad = 1; }   // nested in another struct
        }
        if (!found || !st[k].nptr) st[k].bad = 1;
        uint32_t keep = st[k].nm; while (keep && (st[k].bi[keep - 1] == 3 || st[k].bi[keep - 1] == 4)) keep--;
        st[k].keep = keep;
    }
    // the variables of each block, then every use of them in the functions
    uint32_t vars[16], vs[16], nv = 0;
    for (uint32_t i = 5; i < fn; i += w[i] >> 16) { uint32_t n = w[i] >> 16;
        if ((w[i] & 0xffff) == 59 && n >= 4) for (int k = 0; k < ns; k++) for (uint32_t p = 0; p < st[k].nptr; p++) if (w[i + 1] == st[k].ptr[p]) {
            if (nv == 16) return NULL;
            vars[nv] = w[i + 2]; vs[nv++] = (uint32_t)k; } }
    for (uint32_t i = fn; i < nw;) { uint32_t n = w[i] >> 16, op = w[i] & 0xffff; if (!n || i + n > nw) return NULL;
        for (uint32_t v = 0; v < nv; v++) { int k = (int)vs[v];
            for (uint32_t m = 1; m < n; m++) if (spv_strip_lit(op, m)) continue; else if (w[i + m] == vars[v] || ((op != 65 && op != 66 && op != 67) && m == 1 && w[i + 1] == st[k].id)) {
                // an access chain on it (base: word 3; the first index: word 4, or 5 after a PtrAccessChain's element); anything else: left alone
                uint32_t first = op == 67 ? 5 : 4;
                if ((op == 65 || op == 66 || op == 67) && m == 3 && n > first) {
                    uint32_t c = w[i + first], idx = ~0u;
                    for (uint32_t j = 5; j < fn; j += w[j] >> 16) if ((w[j] & 0xffff) == 43 && (w[j] >> 16) == 4 && w[j + 2] == c) { idx = w[j + 3]; break; }   // OpConstant
                    if (idx == ~0u) st[k].bad = 1; else if (idx >= st[k].keep) st[k].keep = idx + 1;
                } else st[k].bad = 1;
            } }
        i += n; }
    int any = 0; for (int k = 0; k < ns; k++) if (!st[k].bad && st[k].keep < st[k].nm) any = 1;
    if (!any) return NULL;
    uint32_t *o = malloc((size_t)nw * 4), no = 5; if (!o) return NULL; memcpy(o, w, 20);
    for (uint32_t i = 5; i < nw;) { uint32_t n = w[i] >> 16, op = w[i] & 0xffff; int k = -1;
        if (i < fn && (op == 30 || op == 72 || op == 6)) for (int q = 0; q < ns; q++) if (!st[q].bad && st[q].keep < st[q].nm && w[i + 1] == st[q].id) k = q;
        if (k >= 0 && op == 30) { o[no++] = (2 + st[k].keep) << 16 | 30; memcpy(&o[no], &w[i + 1], (1 + st[k].keep) * 4); no += 1 + st[k].keep; }
        else if (k >= 0 && w[i + 2] >= st[k].keep) {}   // OpMemberDecorate / OpMemberName of a dropped member
        else { memcpy(&o[no], &w[i], n * 4); no += n; }
        i += n; }
    *onw = no; return o;
}
static uint32_t *spv_strip_caps(const uint32_t *w, uint32_t nw, uint32_t *onw) {
    if (nw < 6 || w[0] != 0x07230203) return NULL;
    int f64 = 0, ilk = 0, caps = 0;   // caps: 1 Float64 declared, 2 an interlock capability declared
    for (uint32_t i = 5; i < nw;) { uint32_t n = w[i] >> 16, op = w[i] & 0xffff; if (!n || i + n > nw) return NULL;
        if (op == 22 && n >= 3 && w[i + 2] == 64) f64 = 1;   // OpTypeFloat 64
        if ((op == 16 || op == 331) && n >= 3 && w[i + 2] >= 5366 && w[i + 2] <= 5371) ilk = 1;   // OpExecutionMode(Id) {Pixel,Sample,ShadingRate}Interlock{Ordered,Unordered}EXT
        if (op == 5364 || op == 5365) ilk = 1;   // OpBegin / OpEndInvocationInterlockEXT
        if (op == 17 && n == 2) { if (w[i + 1] == 10) caps |= 1; if (w[i + 1] == 5378 || w[i + 1] == 5363 || w[i + 1] == 5372) caps |= 2; }
        i += n; }
    int drop_f64 = (caps & 1) && !f64, drop_ilk = (caps & 2) && !ilk;
    if (!drop_f64 && !drop_ilk) return NULL;
    static const char ext[] = "SPV_EXT_fragment_shader_interlock";   // 33 characters and its NUL: 9 words
    uint32_t *o = malloc((size_t)nw * 4), no = 5; if (!o) return NULL; memcpy(o, w, 20);
    for (uint32_t i = 5; i < nw;) { uint32_t n = w[i] >> 16, op = w[i] & 0xffff;
        int drop = (op == 17 && n == 2 && drop_f64 && w[i + 1] == 10) ||
                   (op == 17 && n == 2 && drop_ilk && (w[i + 1] == 5378 || w[i + 1] == 5363 || w[i + 1] == 5372)) ||
                   (op == 10 && n == 10 && drop_ilk && !memcmp(&w[i + 1], ext, sizeof ext));
        if (!drop) { memcpy(&o[no], &w[i], n * 4); no += n; }
        i += n; }
    *onw = no; return o;
}

// A fragment shader's alpha-test discards taken out: each block that is only "OpLabel; OpKill" and is a branch of a selection ("OpSelectionMerge %m; OpBranchConditional
// c %kill %m" or with the branches the other way) goes on to the selection's merge block in place of the OpKill. For the colour pass of a game that draws depth first
// and tests the colour pass with EQUAL: its discard only repeats the depth pass's, so nothing changes where the two agree, and where they do not the pixel takes the
// material's colour in place of the clear colour. A merge block that starts with OpPhi keeps its OpKill. NULL when there is none to take out. *nk: how many were
__attribute__((unused)) static uint32_t *spv_kill_to_merge(const uint32_t *w, uint32_t nw, uint32_t *onw, uint32_t *nk) {
    uint32_t bound = nw > 3 ? w[3] : 0, kills = 0; if (!bound) return NULL;
    uint32_t *mg = calloc(bound, 4); uint8_t *phi = calloc(bound, 1); if (!mg || !phi) { free(mg); free(phi); return NULL; }
    for (uint32_t i = 5, pm = 0, pl = 0; i < nw;) { uint32_t n = w[i] >> 16, op = w[i] & 0xffff; if (!n || i + n > nw) { free(mg); free(phi); return NULL; }
        if (op == 250 && n >= 4 && pm) { uint32_t t = w[i + 2], f = w[i + 3]; if (t < bound && t != pm) mg[t] = pm; if (f < bound && f != pm) mg[f] = pm; }   // OpBranchConditional after OpSelectionMerge
        if (op == 245 && pl && pl < bound) phi[pl] = 1;   // OpPhi: its block
        pm = op == 247 && n >= 3 ? w[i + 1] : 0;   // OpSelectionMerge
        pl = op == 248 ? w[i + 1] : op == 245 ? pl : 0;   // OpLabel (and the OpPhis after it)
        i += n; }
    uint32_t *o = malloc(((size_t)nw + nw / 2 + 8) * 4), no = 5; if (!o) { free(mg); free(phi); return NULL; }
    memcpy(o, w, 20);
    for (uint32_t i = 5, pl = 0; i < nw;) { uint32_t n = w[i] >> 16, op = w[i] & 0xffff;
        if (op == 252 && pl && pl < bound && mg[pl] && mg[pl] < bound && !phi[mg[pl]]) { o[no++] = 2u << 16 | 249; o[no++] = mg[pl]; kills++; }   // OpKill → OpBranch %merge
        else { memcpy(&o[no], &w[i], n * 4); no += n; }
        pl = op == 248 ? w[i + 1] : 0;
        i += n; }
    free(mg); free(phi);
    if (!kills) { free(o); return NULL; }
    *onw = no; if (nk) *nk = kills; return o;
}
