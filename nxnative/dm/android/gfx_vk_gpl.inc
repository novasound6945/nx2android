// ---------------- graphics pipeline libraries (V.gpl; included by gfx_vk_exec.c after build_pipeline) ----------------
// A pipeline met for the first time cost the driver's whole compile on the replay thread (Dark Souls Remastered on a Retroid Pocket 6, Turnip: 44 → 772 pipelines in
// its first minutes, 510 [longframe]s up to 150 ms), or, built in the background, its draws were left out until it was ready (a deferred renderer: whole objects
// flashed grey). With VK_EXT_graphics_pipeline_library a pipeline is four parts: vertex input (gpl_vi), pre-rasterization shaders (gpl_pr), fragment shader (gpl_fs)
// and fragment output (gpl_fo). The two shader parts are what costs: they are made for every shader of the bundle before the game starts (vk_gpl_prepare, the
// preparation screen of datasync_android.c, kept by the pipeline cache with a reuse mark), with the states they would otherwise depend on left dynamic (extended
// dynamic state; gpl_dyn sets them at the bind). The two others hold no shader and are made at the draw. A draw whose pipeline does not exist yet links the four
// at once without link-time optimization (fast linking: no compile), and the whole pipeline (build_pipeline, as before) is compiled in the background (gpl_opt_*)
// and takes over once ready. A part the preparation could not know (integer-texel views, a vertex shader's outputs added for its fragment shader, the fragment
// depth and dual-source variants, an alpha test other than GREATER, a geometry shader) is made at that draw: one shader, not a whole pipeline (or, DM_VK_ASYNC_PARTS, in the
// background while the draw is left out: gpl_parts_async). Otherwise no draw is left out for its pipeline. The alpha test's reference is a push constant
// (gfx_vk_shader.c spv_alpha_test), so its variant's part does not depend on it: the preparation makes the GREATER one of every fragment shader that takes it (vk_shader_alpha_ok; the function Dark Souls Remastered uses), others are made at their first draw. A fragment
// shader that writes its float outputs as integers (vk_spv_fragout.h spv_int_writer: Luigi's Mansion 3's ~1,400 G-buffer shaders) also gets its integer output part,
// every float output UINT (the variant int_out_bits gives a draw into all-UINT targets), so its first G-buffer draw compiles nothing. The parts made at draws are
// recorded (DM_ROOT/vk_gpl_seen.bin, gpl_seen_*) and made first at the next start's preparation or warm-up.
// Not for rasterizer-discard and transform-feedback draws (rare; as before), nor DM_VK_SPV_ALT / DM_VK_X_FLAT_VS pipelines (debugging).
#include <sys/stat.h>
VkPipelineLayout vk_pipeline_layout_lib(VkDescriptorSetLayout sets[5], int nsets);
uint64_t vk_bundle_hash(uint32_t *nshaders);
int vk_shader_keys(uint32_t stage, uint64_t *out, int max);
void vk_pipeline_cache_save(void);

typedef struct { uint32_t topo, restart, nattr, nbind; struct { uint32_t loc, binding, fmt, off; } attr[16]; struct { uint32_t binding, stride, rate, divisor; } bind[17]; } gvikey;
typedef struct { uint64_t vs, gs, um0, im0, um2, im2; uint32_t xo0, xo2; gpl_rast r; uint32_t vi0; } gprkey;   // gs: its translation for the draw's topology (vk_shader_gs_topo); vi0: the vertex inputs' signedness flips (vi_flip_of)
typedef struct { uint64_t fs, um, im; uint32_t fd, ms; } gfskey;   // fd: vk_shader_module's variant bits (the alpha test's function in bits 4-7; its reference is a push constant; integer targets' outputs in 8-23); ms: samples | a2c << 8 for shaders that need the multisample state (gpl_fs_ms), else 0
typedef struct { uint32_t ncol, cfmt[8], dfmt, samples, a2c, blend_en, logic_en, logic_op, wmask[8]; struct { uint32_t src, dst, srca, dsta, eq, eqa; } bl[8]; } gfokey;
static nvn_hmap g_gvi = {.klen = sizeof(gvikey)}, g_gpr = {.klen = sizeof(gprkey)}, g_gfs = {.klen = sizeof(gfskey)}, g_gfo = {.klen = sizeof(gfokey)};
static pthread_mutex_t g_glib_mu = PTHREAD_MUTEX_INITIALIZER;   // the parts' tables and the statistics: the replay, preparation, warm-up and background threads
// link: a linked pipeline's whole time at its draw, the parts made there included; link_only: the link call alone; part: a shader part made at a draw; parts_at: those
// that were alpha test variants (fragment parts with a function in fd), parts_int integer output variants (fd bits 8-23); prep_at / ns_prep_at / ns_prep_all: the
// preparation's alpha test parts and their time (summed over threads); prep_int / ns_prep_int: its integer output parts (gpl_part_i), int_writers: the shaders that take one
// bg_part / parts_bg / bg_queued / bg_qfull: the shader parts made on the background pipeline threads for draws left out (DM_VK_ASYNC_PARTS, gpl_parts_async), their jobs and full queues
static struct { gpl_stat link, link_only, part, opt, bg_part; uint64_t links, parts[2], parts_at, parts_int, link_fail, part_fail, queued, qfull, opt_fail; uint64_t ns_part, ns_link, n_old, ns_old;
                uint64_t parts_bg[2], bg_queued, bg_qfull;
                uint64_t prep_at, prep_at_skip, ns_prep_at, ns_prep_all, prep_int, ns_prep_int, int_writers, prep_seen; } g_gst;   // (under g_glib_mu)

// DM_VK_GPL_LTO=1 (read once at startup: the parts must all be made the same way): the parts keep their link-time optimization information and a draw links them
// with link-time optimization, a pipeline as good as the whole one, never the fast link's (diagnosis: the fast-linked pipelines' colour flashes on Turnip)
static int gpl_lto(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_GPL_LTO"); v = e && atoi(e) ? 1 : 0; if (v) VKLOG("GPL: DM_VK_GPL_LTO: draws link with link-time optimization"); } return v; }
static VkPipeline gpl_make(VkGraphicsPipelineLibraryFlagsEXT part, VkGraphicsPipelineCreateInfo *ci, VkPipelineCache cache) {
    VkGraphicsPipelineLibraryCreateInfoEXT li = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT, (void *)ci->pNext, part};
    ci->pNext = &li; ci->flags |= VK_PIPELINE_CREATE_LIBRARY_BIT_KHR; ci->basePipelineIndex = -1;
    if (gpl_lto()) ci->flags |= VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT;
    VkPipeline p = VK_NULL_HANDLE; VkResult r = PSERIAL(vkCreateGraphicsPipelines(V.dev, cache, 1, ci, NULL, &p));
    if (cache) vk_pipeline_cache_changed();
    if (r) { static int n; if (n++ < 16) VKLOG("[RED] GPL: a pipeline library (part %#x) failed (%d)", (unsigned)part, r); return VK_NULL_HANDLE; }
    return p;
}
// a part from its table, or made (outside the lock) and put there; another thread's that came first is kept
static VkPipeline gpl_put(nvn_hmap *m, const void *key, VkPipeline p) {
    pthread_mutex_lock(&g_glib_mu); VkPipeline had = nvn_hm_get(m, key); if (!had && p) nvn_hm_put(m, key, p); pthread_mutex_unlock(&g_glib_mu);
    if (had) { if (p) vkDestroyPipeline(V.dev, p, NULL); return had; }
    return p;
}
static VkPipeline gpl_get(nvn_hmap *m, const void *key) { pthread_mutex_lock(&g_glib_mu); VkPipeline p = nvn_hm_get(m, key); pthread_mutex_unlock(&g_glib_mu); return p; }

// vertex input: the attributes, bindings, topology and primitive restart as drawn (no shader: made at the draw)
static VkPipeline gpl_vi(const pkey *k) {
    gvikey key; memset(&key, 0, sizeof key); key.topo = k->topo; key.restart = k->restart; key.nattr = k->nattr; key.nbind = k->nbind;
    memcpy(key.attr, k->attr, sizeof key.attr[0] * k->nattr); memcpy(key.bind, k->bind, sizeof key.bind[0] * k->nbind);
    VkPipeline p = gpl_get(&g_gvi, &key); if (p) return p;
    VkVertexInputAttributeDescription ad[16]; VkVertexInputBindingDescription bd[17]; VkVertexInputBindingDivisorDescriptionEXT dd[17]; uint32_t ndd = 0;
    for (uint32_t i = 0; i < k->nattr; i++) ad[i] = (VkVertexInputAttributeDescription){k->attr[i].loc, k->attr[i].binding, (VkFormat)k->attr[i].fmt, k->attr[i].off};
    for (uint32_t i = 0; i < k->nbind; i++) { bd[i] = (VkVertexInputBindingDescription){k->bind[i].binding, k->bind[i].stride, (VkVertexInputRate)k->bind[i].rate};
        if (k->bind[i].rate && k->bind[i].divisor > 1) dd[ndd++] = (VkVertexInputBindingDivisorDescriptionEXT){k->bind[i].binding, k->bind[i].divisor}; }
    VkPipelineVertexInputDivisorStateCreateInfoEXT dvs = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT, NULL, ndd, dd};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, ndd ? &dvs : NULL, 0, k->nbind, bd, k->nattr, ad};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, (VkPrimitiveTopology)k->topo, k->restart};
    VkGraphicsPipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; ci.pVertexInputState = &vi; ci.pInputAssemblyState = &ia;
    return gpl_put(&g_gvi, &key, gpl_make(VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT, &ci, VK_NULL_HANDLE));
}
// pre-rasterization: the vertex (and geometry) shader with its layout (its own sets only, independent), viewport and rasterization state. Dynamic: viewport, scissor,
// depth bias and line width (as every pipeline), cull mode, front face, depth bias enable (Vulkan 1.3), and the extended dynamic state 3 ones the device has (V.gpl_eds3);
// what is not dynamic is in the key (pk->r). Rasterizer discard stays off (those draws do not come here)
static VkPipeline gpl_pr_make(const gprkey *pk, vkshader_t *vs, vkshader_t *gs) {
    int bs = build_begin();
    if (bs >= 0) { snprintf(g_build[bs].what, sizeof g_build[bs].what, "pre-rasterization library, vertex shader %016llx (views %#llx/%#llx, outputs added %#x), geometry %016llx; clamp %u mode %u clip %u",
                            (unsigned long long)pk->vs, (unsigned long long)pk->um0, (unsigned long long)pk->im0, pk->xo0, (unsigned long long)pk->gs, pk->r.clamp, pk->r.mode, pk->r.clip); g_build[bs].phase = 1; }
    VkPipelineShaderStageCreateInfo ss[2]; uint32_t ns = 0;
    ss[ns++] = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, vk_sg_pin(VK_SHADER_STAGE_VERTEX_BIT, NULL), 0, VK_SHADER_STAGE_VERTEX_BIT, vk_shader_module(vs, pk->um0, pk->im0, pk->xo0, 0, pk->vi0), "main", NULL};
    if (gs) ss[ns++] = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, vk_sg_pin(VK_SHADER_STAGE_GEOMETRY_BIT, NULL), 0, VK_SHADER_STAGE_GEOMETRY_BIT, vk_shader_module(gs, pk->um2, pk->im2, pk->xo2, 0, 0), "main", NULL};
    VkDescriptorSetLayout dsl[5] = {vs->dsl}; int nsets = 1; if (gs) { dsl[gs->set] = gs->dsl; nsets = gs->set + 1; }
    VkPipelineViewportDepthClipControlCreateInfoEXT dcc = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT, NULL, pk->r.clip};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, pk->r.clip ? &dcc : NULL, 0, 1, NULL, 1, NULL};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0, pk->r.clamp, 0,
        pk->r.mode == 1 ? VK_POLYGON_MODE_POINT : pk->r.mode == 2 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, 0, 0, 0, 0, 1.0f};
    VkDynamicState dyn[12] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_LINE_WIDTH, VK_DYNAMIC_STATE_CULL_MODE,
                              VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE}; uint32_t nd = 7;
    if (V.gpl_eds3 & GPL_E3_CLAMP) dyn[nd++] = VK_DYNAMIC_STATE_DEPTH_CLAMP_ENABLE_EXT;
    if (V.gpl_eds3 & GPL_E3_POLY) dyn[nd++] = VK_DYNAMIC_STATE_POLYGON_MODE_EXT;
    if (V.gpl_eds3 & GPL_E3_CLIP) dyn[nd++] = VK_DYNAMIC_STATE_DEPTH_CLIP_NEGATIVE_ONE_TO_ONE_EXT;
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, nd, dyn};
    VkGraphicsPipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; ci.stageCount = ns; ci.pStages = ss; ci.pViewportState = &vp; ci.pRasterizationState = &rs;
    ci.pDynamicState = &dy; ci.layout = vk_pipeline_layout_lib(dsl, nsets);
    if (bs >= 0) g_build[bs].phase = 2;
    VkPipeline p = gpl_make(VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT, &ci, V.pcache);
    build_end(bs); return p;
}
// A fragment shader that runs per sample (the SampleRateShading capability: SampleId, SamplePosition, the Sample decoration) needs the multisample state in its part,
// the same as the output part's (then in its key); others take none and serve every sample count
static int gpl_fs_ms(const vkshader_t *s) {
    if (!s || !s->spv) return 0;
    for (uint32_t i = 5; i < s->nspv;) { uint32_t n = s->spv[i] >> 16, op = s->spv[i] & 0xffff; if (!n) break;
        if (op == 17 && n >= 2 && s->spv[i + 1] == 35) return 1;   // OpCapability SampleRateShading
        if (op == 71 && n >= 3 && s->spv[i + 2] == 17) return 1;   // OpDecorate Sample
        if (op == 54) break;
        i += n; }
    return 0;
}
// fragment shader: the shader (its variant), its layout (its own set, independent) and the depth-stencil state, all dynamic (extended dynamic state 1: test, write,
// compare, stencil test and operations; compare and write masks and reference as every pipeline). No fragment stage (a depth-only program): a part without one
static VkPipeline gpl_fs_make(const gfskey *fk, vkshader_t *fs) {
    int bs = build_begin();
    if (bs >= 0) { snprintf(g_build[bs].what, sizeof g_build[bs].what, "fragment shader library, shader %016llx (views %#llx/%#llx, variant %#x, multisample %#x)",
                            (unsigned long long)fk->fs, (unsigned long long)fk->um, (unsigned long long)fk->im, fk->fd, fk->ms); g_build[bs].phase = 1; }
    VkPipelineShaderStageCreateInfo ss = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, vk_sg_pin(VK_SHADER_STAGE_FRAGMENT_BIT, NULL), 0, VK_SHADER_STAGE_FRAGMENT_BIT,
        fs ? vk_shader_module(fs, fk->um, fk->im, 0, (int)fk->fd, 0) : VK_NULL_HANDLE, "main", NULL};   // (an alpha test variant reads its reference from the push constants)
    VkDescriptorSetLayout dsl[5] = {0}; int nsets = 0; if (fs) { dsl[fs->set] = fs->dsl; nsets = fs->set + 1; }
    VkStencilOpState so = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS, 0xff, 0xff, 0};
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, NULL, 0, 0, 0, VK_COMPARE_OP_ALWAYS, 0, 0, so, so, 0, 1};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, (VkSampleCountFlagBits)(fk->ms & 0xff), 0, 0, NULL, fk->ms >> 8 & 1, 0};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
                            VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, sizeof dyn / sizeof *dyn, dyn};
    VkGraphicsPipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; ci.stageCount = fs ? 1 : 0; ci.pStages = fs ? &ss : NULL; ci.pDepthStencilState = &ds;
    ci.pMultisampleState = fk->ms ? &ms : NULL; ci.pDynamicState = &dy; ci.layout = vk_pipeline_layout_lib(dsl, nsets);
    if (bs >= 0) g_build[bs].phase = 2;
    VkPipeline p = gpl_make(VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT, &ci, V.pcache);
    build_end(bs); return p;
}
// fragment output: blending, write masks, samples and alpha to coverage, the attachments' formats (no shader: made at the draw). Dynamic: the blend constants
static VkPipeline gpl_fo(const pkey *k) {
    gfokey key; memset(&key, 0, sizeof key); key.ncol = k->ncol; memcpy(key.cfmt, k->cfmt, sizeof key.cfmt); key.dfmt = k->dfmt; key.samples = k->samples; key.a2c = k->a2c;
    key.blend_en = k->blend_en; key.logic_en = k->logic_en; key.logic_op = k->logic_en ? k->logic_op : 0; memcpy(key.wmask, k->wmask, sizeof key.wmask); memcpy(key.bl, k->bl, sizeof key.bl);
    VkPipeline p = gpl_get(&g_gfo, &key); if (p) return p;
    VkPipelineColorBlendAttachmentState cba[8];
    for (uint32_t i = 0; i < k->ncol; i++) cba[i] = (VkPipelineColorBlendAttachmentState){(k->blend_en >> i) & 1, bfac(k->bl[i].src), bfac(k->bl[i].dst), bop(k->bl[i].eq),
        bfac(k->bl[i].srca), bfac(k->bl[i].dsta), bop(k->bl[i].eqa), k->wmask[i]};
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, k->logic_en, (VkLogicOp)k->logic_op, k->ncol, cba, {0, 0, 0, 0}};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, (VkSampleCountFlagBits)k->samples, 0, 0, NULL, k->a2c, 0};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 1, dyn};
    VkFormat cf[8]; for (uint32_t i = 0; i < k->ncol; i++) cf[i] = (VkFormat)k->cfmt[i];
    VkFormat df = (VkFormat)k->dfmt; int has_stencil = df == VK_FORMAT_D24_UNORM_S8_UINT || df == VK_FORMAT_D32_SFLOAT_S8_UINT || df == VK_FORMAT_S8_UINT;
    VkPipelineRenderingCreateInfo rci = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, k->ncol, cf, df == VK_FORMAT_S8_UINT ? VK_FORMAT_UNDEFINED : df, has_stencil ? df : VK_FORMAT_UNDEFINED};
    VkGraphicsPipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rci}; ci.pColorBlendState = &cb; ci.pMultisampleState = &ms; ci.pDynamicState = &dy;
    return gpl_put(&g_gfo, &key, gpl_make(VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT, &ci, VK_NULL_HANDLE));
}
// the shader parts' keys: as the draw makes them, and as the preparation does (no variant but the GREATER alpha test, the static states at their defaults)
static gprkey gpl_pr_key(const vkshader_t *vs, const vkshader_t *gs, uint64_t um0, uint64_t im0, uint32_t xo0, uint64_t um2, uint64_t im2, uint32_t xo2, gpl_rast r) {
    gprkey k; memset(&k, 0, sizeof k); k.vs = vs->key; k.gs = gs ? gs->key : 0; k.um0 = um0; k.im0 = im0; k.xo0 = xo0;
    if (gs) { k.um2 = um2; k.im2 = im2; k.xo2 = xo2; }
    k.r = r; return k;
}
static gfskey gpl_fs_key(const vkshader_t *fs, uint64_t um, uint64_t im, uint32_t fd, uint32_t samples, uint32_t a2c) {
    gfskey k; memset(&k, 0, sizeof k); if (!fs) return k;
    k.fs = fs->key; k.um = um; k.im = im; k.fd = fd; k.ms = gpl_fs_ms(fs) ? samples | a2c << 8 : 0; return k;
}
// a shader part: from its table, else made now (*made: its time, for the statistics; 0 when it was there)
// a part the driver refused once is not made again: every other draw state with the same shaders would compile it again (one compile each, a few ms on the
// replay thread), only to be refused again; its draws are left out as the first one's (graphics_pipeline, gfx_vk_exec_draws.inc). (under g_glib_mu)
static nvn_hmap g_gpr_bad = {.klen = sizeof(gprkey)}, g_gfs_bad = {.klen = sizeof(gfskey)};
static VkPipeline gpl_part(int fs_part, const void *key, vkshader_t *a, vkshader_t *b, uint64_t *made) {
    nvn_hmap *bad = fs_part ? &g_gfs_bad : &g_gpr_bad;
    nvn_hmap *m = fs_part ? &g_gfs : &g_gpr; VkPipeline p = gpl_get(m, key); *made = 0; if (p) return p;
    if (gpl_get(bad, key)) return VK_NULL_HANDLE;
    uint64_t t0 = tick(); p = fs_part ? gpl_fs_make(key, a) : gpl_pr_make(key, a, b); *made = tick() - t0 + 1;
    if (!p) { pthread_mutex_lock(&g_glib_mu); g_gst.part_fail++; if (!nvn_hm_get(bad, key)) nvn_hm_put(bad, key, (void *)1); pthread_mutex_unlock(&g_glib_mu); return VK_NULL_HANDLE; }
    return gpl_put(m, key, p);
}
// ---- the parts the draws made, kept for the next start (DM_ROOT/vk_gpl_seen.bin, beside the pipeline cache) ----
// What a part depends on beyond its shader is the draw's: the bound textures' formats (integer-texel views), the vertex buffers' formats (vi0: inputs read with the
// other signedness), the fragment shader's inputs (outputs added), the fragment depth, dual-source and alpha test variants. The preparation cannot know them, so
// such a part is made at its first draw: Super Mario Bros. Wonder on a Retroid Pocket Nova made 54 pre-rasterization parts (most with a vertex-input flip, 5
// with uint-texel views 0x80) and 4 fragment parts at draws in its first 7 minutes, 7-35 ms each and up to ten in one frame, and each set the game's interval
// to 2. Each one's key is recorded here; the next start makes them first: on the preparation's screen, or at the head of the warm-up (from the pipeline cache,
// before the game meets them). A key the preparation makes anyway (gpl_pr_std / gpl_fs_std: a draw that came before the warm-up) is not recorded. The header
// holds the keys' version and sizes, the extended dynamic state 3 states and the bundle's hash: another bundle, device or key layout starts the list again
#define GSEEN_MAGIC 0x31534747u   // "GGS1"
typedef struct { uint32_t magic, key_ver, pr_size, fs_size, eds3, pad; uint64_t bundle; } gseen_hdr;
typedef struct { uint32_t fs, pad; union { gprkey pr; gfskey f; } k; } gseen_rec;   // fs: a fragment part (k.f), else a pre-rasterization one (k.pr)
static gseen_rec *g_gs; static int g_gsn;   // read at the start (vk_gpl_prepare): the preparation's and the warm-up's first g_gsn parts
static FILE *g_gsf; static nvn_hmap g_gs_have = {.klen = sizeof(gseen_rec)};
enum { GSEEN_Q = 32 };
static gseen_rec g_gsq[GSEEN_Q]; static int g_gsq_n, g_gsw_on; static pthread_mutex_t g_gsq_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_gsq_cv = PTHREAD_COND_INITIALIZER;
static void *gpl_seen_writer(void *arg) {   // (a thread of its own: a flush to the files folder can take tens of ms, as the prewarm record's)
    (void)arg; pthread_setname_np(pthread_self(), "vk gpl seen"); static gseen_rec b[GSEEN_Q];
    for (;;) {
        pthread_mutex_lock(&g_gsq_mu); while (!g_gsq_n) pthread_cond_wait(&g_gsq_cv, &g_gsq_mu);
        int n = g_gsq_n; memcpy(b, g_gsq, sizeof *b * (size_t)n); g_gsq_n = 0; pthread_mutex_unlock(&g_gsq_mu);
        fwrite(b, sizeof *b, (size_t)n, g_gsf); fflush(g_gsf);
    }
    return NULL;
}
static int gpl_pr_std(const gprkey *k) { return !k->um0 && !k->im0 && !k->xo0 && !k->gs && !k->vi0 && !k->r.clamp && !k->r.mode; }   // (r.clip: both made where it is not dynamic)
static int gpl_fs_std(const gfskey *k, const vkshader_t *fs) {
    if (k->um || k->im || k->ms) return 0;
    return !k->fd || (fs && k->fd == (uint32_t)NVN_ALPHA_FUNC_GREATER << 4) || (fs && fs->int_writer && k->fd == (fs->fout_locs & 0xff) << 8);
}
static void gpl_seen_note(int fs_part, const void *key) {   // a part made at a draw (gpl_link)
    if (!g_gsf) return;
    gseen_rec r; memset(&r, 0, sizeof r); r.fs = (uint32_t)fs_part; if (fs_part) r.k.f = *(const gfskey *)key; else r.k.pr = *(const gprkey *)key;
    pthread_mutex_lock(&g_gsq_mu);
    if (!nvn_hm_get(&g_gs_have, &r)) { nvn_hm_put(&g_gs_have, &r, (void *)1);
        if (!g_gsw_on) { g_gsw_on = 1; pthread_t t; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED); pthread_create(&t, &a, gpl_seen_writer, NULL); pthread_attr_destroy(&a); }
        if (g_gsq_n < GSEEN_Q) g_gsq[g_gsq_n++] = r;   // (a full queue drops the record: that part is only made at its draw again next time)
        pthread_cond_signal(&g_gsq_cv); }
    pthread_mutex_unlock(&g_gsq_mu);
}
// read the list (dropping a torn last record), write it again and keep the file open to add to it. DM_VK_GPL_SEEN=0 (start-up): neither read nor written
static void gpl_seen_open(const char *root, uint64_t bundle) {
    const char *e = getenv("DM_VK_GPL_SEEN"); if ((e && !atoi(e)) || !root || g_gsf) return;
    char path[1100]; snprintf(path, sizeof path, "%s/vk_gpl_seen.bin", root);
    gseen_hdr want = {GSEEN_MAGIC, GPL_KEY_VER, (uint32_t)sizeof(gprkey), (uint32_t)sizeof(gfskey), (uint32_t)V.gpl_eds3, 0, bundle}, have;
    FILE *f = fopen(path, "rb");
    if (f && fread(&have, sizeof have, 1, f) == 1 && !memcmp(&have, &want, sizeof have)) {
        fseek(f, 0, SEEK_END); long n = (ftell(f) - (long)sizeof have) / (long)sizeof(gseen_rec); fseek(f, sizeof have, SEEK_SET);
        if (n > 0 && (g_gs = malloc((size_t)n * sizeof *g_gs))) g_gsn = (int)fread(g_gs, sizeof *g_gs, (size_t)n, f);
    }
    if (f) fclose(f);
    for (int i = 0; i < g_gsn; i++) nvn_hm_put(&g_gs_have, &g_gs[i], (void *)1);
    g_gsf = fopen(path, "wb"); if (g_gsf) { fwrite(&want, sizeof want, 1, g_gsf); if (g_gsn) fwrite(g_gs, sizeof *g_gs, (size_t)g_gsn, g_gsf); fflush(g_gsf); }
    else VKLOG("[RED] GPL: %s could not be written: the parts made at draws are not kept for the next start", path);
}
static void gpl_seen_make(const gseen_rec *r, uint64_t *made) {   // one listed part, at the preparation or the warm-up
    *made = 0;
    if (r->fs) { vkshader_t *fs = r->k.f.fs ? vk_shader(r->k.f.fs, 1) : NULL; if (r->k.f.fs && !fs) return; gpl_part(1, &r->k.f, fs, NULL, made); }
    else { vkshader_t *vs = r->k.pr.gs ? NULL : vk_shader(r->k.pr.vs, 0); if (!vs) return; gpl_part(0, &r->k.pr, vs, NULL, made); }   // (with a geometry shader: not recorded)
}
static int gpl_eligible(const pkey *k) {
    return V.gpl && !k->raster_discard && !k->alt && !k->debug_mode && !k->gs_split &&   // (gs_split: two geometry passes, gfx_vk_exec_xfb.inc, whole pipelines only)
           !(S.sh[0] && S.sh[0]->xfb) && !(S.sh[2] && S.sh[2]->xfb);
}
// the draw's shader parts' keys and shaders (S.sh: the replay thread's)
static void gpl_draw_keys(const pkey *k, gprkey *out_pk, gfskey *out_fk, vkshader_t **out_vs, vkshader_t **out_fs, vkshader_t **out_gs) {
    vkshader_t *vs = S.sh[0], *fs = S.sh[1], *gs = S.sh[2] ? vk_shader_gs_topo(S.sh[2], (VkPrimitiveTopology)k->topo) : NULL;
    uint32_t xo0 = (gs ? gs->in_locs : fs ? fs->in_locs : 0) & ~vs->out_locs, xo2 = gs ? (fs ? fs->in_locs : 0) & ~gs->out_locs : 0;   // as build_pipeline
    gprkey pk = gpl_pr_key(vs, gs, k->um[0], k->im[0], xo0, k->um[2], k->im[2], xo2, gpl_rast_key(V.gpl_eds3, k->depth_clamp, k->mode, k->clip_neg1, V.has_depth_clip_ctl));
    pk.vi0 = vi_flip_of(k, vs);   // (the preparation's parts have none: a draw that needs a flip makes its part)
    gfskey fk = gpl_fs_key(fs, k->um[1], k->im[1], (k->fdz ? 1 : 0) | (k->dual ? 2 : 0) | (k->atest & 15) << 4 | int_out_bits(k, fs), k->samples, k->a2c);   // (integer targets: the preparation's part when every float output is UINT and the shader writes integers)
    // DM_VK_GPL_FSMS=1 (live, diagnosis of the colour flashes while a linked pipeline draws): every fragment part of a multisampled (or alpha-to-coverage) draw takes
    // the draw's multisample state, as the whole pipeline's fragment shader does, not only the per-sample shaders (gpl_fs_ms). Valid either way (VUID-...-06633/06635:
    // the same state as the output part's); those parts are made at their first draw (one shader each), the preparation's stay for single-sampled draws
    { const char *e = getenv("DM_VK_GPL_FSMS"); if (fs && e && atoi(e) && !fk.ms && (k->samples > 1 || k->a2c)) fk.ms = k->samples | k->a2c << 8; }
    *out_pk = pk; *out_fk = fk; *out_vs = vs; *out_fs = fs; *out_gs = gs;
}
// the four parts of a draw's pipeline (shader parts from the preparation, or made now), linked without link-time optimization. NULL when one could not be made (logged)
static VkPipeline gpl_link(const pkey *k, VkPipelineLayout layout) {
    uint64_t t0 = tick();
    gprkey pk; gfskey fk; vkshader_t *vs, *fs, *gs; gpl_draw_keys(k, &pk, &fk, &vs, &fs, &gs);
    uint64_t mpr = 0, mfs = 0;
    VkPipeline lib[4] = {gpl_vi(k), gpl_part(0, &pk, vs, gs, &mpr), gpl_part(1, &fk, fs, NULL, &mfs), gpl_fo(k)}, p = VK_NULL_HANDLE;
    uint64_t t1 = tick();
    if (lib[0] && lib[1] && lib[2] && lib[3]) {
        VkPipelineLibraryCreateInfoKHR li = {VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR, NULL, 4, lib};
        VkGraphicsPipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &li}; ci.layout = layout; ci.basePipelineIndex = -1;
        if (gpl_lto()) ci.flags |= VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT;
        VkResult r = PSERIAL(vkCreateGraphicsPipelines(V.dev, gpl_lto() ? V.pcache : VK_NULL_HANDLE, 1, &ci, NULL, &p));
        if (gpl_lto() && V.pcache) vk_pipeline_cache_changed();
        if (r) { p = VK_NULL_HANDLE; static int n; if (n++ < 16) VKLOG("[RED] GPL: linking failed (%d): shaders %016llx %016llx %016llx: built as before", r, (unsigned long long)k->key[0], (unsigned long long)k->key[1], (unsigned long long)k->key[2]); } }
    else { static int n; if (n++ < 16) VKLOG("[RED] GPL: a part is missing (vertex input %d, pre-rasterization %d, fragment %d, output %d): shaders %016llx %016llx %016llx: built as before",
                                              lib[0] != 0, lib[1] != 0, lib[2] != 0, lib[3] != 0, (unsigned long long)k->key[0], (unsigned long long)k->key[1], (unsigned long long)k->key[2]); }
    uint64_t t2 = tick();
    pthread_mutex_lock(&g_glib_mu);
    if (p) { g_gst.links++; gpl_stat_add(&g_gst.link, t2 - t0); gpl_stat_add(&g_gst.link_only, t2 - t1); } else g_gst.link_fail++;
    g_gst.ns_link += t2 - t1; g_gst.ns_part += t1 - t0;
    if (mpr) { g_gst.parts[0]++; gpl_stat_add(&g_gst.part, mpr); } if (mfs) { g_gst.parts[1]++; gpl_stat_add(&g_gst.part, mfs); if (fk.fd >> 4 & 15) g_gst.parts_at++; if (fk.fd >> 8) g_gst.parts_int++; }
    uint64_t nl = g_gst.links; pthread_mutex_unlock(&g_glib_mu);
    if (mpr && lib[1] && !gpl_pr_std(&pk)) gpl_seen_note(0, &pk);
    if (mfs && lib[2] && !gpl_fs_std(&fk, fs)) gpl_seen_note(1, &fk);
    if (mpr || mfs) { static int n; if (n++ < 40) VKLOG("GPL: shader part made at a draw: %s%s %.2f ms (vertex %016llx views %#llx/%#llx outputs added %#x geometry %016llx; fragment %016llx views %#llx/%#llx variant %#x ms %#x)",
                                                     mpr ? "pre-rasterization" : "", mfs ? (mpr ? " + fragment" : "fragment") : "", (mpr + mfs) / 1e6, (unsigned long long)pk.vs, (unsigned long long)pk.um0, (unsigned long long)pk.im0, pk.xo0,
                                                     (unsigned long long)pk.gs, (unsigned long long)fk.fs, (unsigned long long)fk.um, (unsigned long long)fk.im, fk.fd, fk.ms); }
    // DM_VK_GPL_TRACE=n (live): the first n linked pipelines' keys, to set beside the frames that flashed (which shaders, sample count, variants, states)
    { static int nt; const char *e = getenv("DM_VK_GPL_TRACE"); if (p && e && nt < atoi(e)) { nt++;
        VKLOG("GPL trace %d: shaders %016llx %016llx %016llx; samples %u a2c %u, colour %u (format %u) depth format %u; outputs added %#x/%#x, views %#llx/%#llx %#llx/%#llx, fragment variant %#x ms %#x; "
              "cull %u front %u bias %u depth test %u write %u func %u stencil %u; clamp %u mode %u clip -1..1 %u; topology %u restart %u; blend %#x mask %#x",
              nt, (unsigned long long)k->key[0], (unsigned long long)k->key[1], (unsigned long long)k->key[2], k->samples, k->a2c, k->ncol, k->cfmt[0], k->dfmt, pk.xo0, pk.xo2,
              (unsigned long long)k->um[0], (unsigned long long)k->im[0], (unsigned long long)k->um[1], (unsigned long long)k->im[1], fk.fd, fk.ms,
              k->cull, k->front, k->bias_en, k->dt, k->dw, k->df, k->st, k->depth_clamp, k->mode, k->clip_neg1, k->topo, k->restart, k->blend_en, k->wmask[0]); } }
    if (p && (nl <= 8 || !(nl & (nl - 1)))) VKLOG("GPL: %llu pipelines linked at draws so far; this one %.2f ms (parts %.2f ms, link %.2f ms)", (unsigned long long)nl, (t2 - t0) / 1e6, (t1 - t0) / 1e6, (t2 - t1) / 1e6);
    return p;
}
// ---- a shader part a draw needs and nobody made yet, made on the background pipeline threads (gfx_vk_exec_pipelines.inc async_pipe_thread) ----
// Made at the draw, one costs the replay thread its whole compile: Super Mario Bros. Wonder on a Retroid Pocket Nova, first start, 58 pre-rasterization parts
// (vertex-input flips, uint-texel views) and a few fragment ones at 20-30 ms each, one at 590 ms. Instead (DM_VK_ASYNC_PARTS, live, default 1) the draw is left out
// (counted with the draws skipped for no pipeline) while the part is made in the background, and a later draw links it (fast: no compile). Not with
// DM_VK_ASYNC_PIPE=0 (Dark Souls Remastered's profile: a deferred renderer whose objects flash grey for a pass left out), nor for the draws async_pipe never leaves
// out (targets drawn in fewer than three presents in a row: a target drawn once would stay missing; linear render targets, rasterizer-discard and transform-feedback
// draws), nor when the queue is full: those make the part at the draw, as before. A part being made is in g_gpr_pend / g_gfs_pend (one job per key, under
// g_glib_mu) until it is in its table or the driver refused it (then the draw links as before, which fails and builds the whole pipeline the way without libraries).
// The parts made so are recorded for the next start (gpl_seen_note) as those made at draws. No teardown: the threads are the process's (it ends with _exit)
static nvn_hmap g_gpr_pend = {.klen = sizeof(gprkey)}, g_gfs_pend = {.klen = sizeof(gfskey)};   // (under g_glib_mu)
struct gpl_pjob { gprkey pk; gfskey fk; vkshader_t *vs, *gs, *fs; unsigned parts; };   // parts: GPL_PART_PR / GPL_PART_FS to make (vk_gpl.h)
static void gpl_pjob_run(struct gpl_pjob *j) {   // (a background pipeline thread)
    uint64_t mpr = 0, mfs = 0; VkPipeline ppr = VK_NULL_HANDLE, pfs = VK_NULL_HANDLE;
    if (j->parts & GPL_PART_PR) ppr = gpl_part(0, &j->pk, j->vs, j->gs, &mpr);
    if (j->parts & GPL_PART_FS) pfs = gpl_part(1, &j->fk, j->fs, NULL, &mfs);
    pthread_mutex_lock(&g_glib_mu);
    if (j->parts & GPL_PART_PR) nvn_hm_del(&g_gpr_pend, &j->pk);
    if (j->parts & GPL_PART_FS) nvn_hm_del(&g_gfs_pend, &j->fk);
    if (mpr) { g_gst.parts_bg[0]++; gpl_stat_add(&g_gst.bg_part, mpr); } if (mfs) { g_gst.parts_bg[1]++; gpl_stat_add(&g_gst.bg_part, mfs); }
    pthread_mutex_unlock(&g_glib_mu);
    if (mpr && ppr && !gpl_pr_std(&j->pk)) gpl_seen_note(0, &j->pk);
    if (mfs && pfs && !gpl_fs_std(&j->fk, j->fs)) gpl_seen_note(1, &j->fk);
    if (mpr || mfs) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 40) VKLOG("GPL: shader part made in the background for a draw left out: %s%s %.2f ms (vertex %016llx views %#llx/%#llx outputs added %#x flips %#x; fragment %016llx views %#llx/%#llx variant %#x ms %#x)",
            mpr ? "pre-rasterization" : "", mfs ? (mpr ? " + fragment" : "fragment") : "", (mpr + mfs) / 1e6, (unsigned long long)j->pk.vs, (unsigned long long)j->pk.um0, (unsigned long long)j->pk.im0,
            j->pk.xo0, j->pk.vi0, (unsigned long long)j->fk.fs, (unsigned long long)j->fk.um, (unsigned long long)j->fk.im, j->fk.fd, j->fk.ms); }
    free(j);
}
// 1: a shader part of the draw's linked pipeline is being made in the background, the draw is left out; 0: link now (gpl_link makes what is missing, as before)
static int gpl_parts_async(const pkey *k) {   // (the replay thread)
    int async = g_async_parts && async_skip_ok(k);
    if (!async) return 0;   // (the keys are not worked out for nothing: gpl_link does)
    gprkey pk; gfskey fk; vkshader_t *vs, *fs, *gs; gpl_draw_keys(k, &pk, &fk, &vs, &fs, &gs);
    unsigned need = 0, pend = 0, q;
    pthread_mutex_lock(&g_glib_mu);
    if (!nvn_hm_get(&g_gpr, &pk) && !nvn_hm_get(&g_gpr_bad, &pk)) { need |= GPL_PART_PR; if (nvn_hm_get(&g_gpr_pend, &pk)) pend |= GPL_PART_PR; }
    if (fs && !nvn_hm_get(&g_gfs, &fk) && !nvn_hm_get(&g_gfs_bad, &fk)) { need |= GPL_PART_FS; if (nvn_hm_get(&g_gfs_pend, &fk)) pend |= GPL_PART_FS; }   // (no fragment shader: a part without one, nothing compiled)
    int plan = gpl_parts_plan(async, need, pend, &q);
    if (plan == GPL_PARTS_QUEUE) { if (q & GPL_PART_PR) nvn_hm_put(&g_gpr_pend, &pk, (void *)1); if (q & GPL_PART_FS) nvn_hm_put(&g_gfs_pend, &fk, (void *)1); }
    pthread_mutex_unlock(&g_glib_mu);
    if (plan != GPL_PARTS_QUEUE) return plan == GPL_PARTS_WAIT;
    struct gpl_pjob *j = malloc(sizeof *j);
    if (j) { j->pk = pk; j->fk = fk; j->vs = vs; j->gs = gs; j->fs = fs; j->parts = q; }
    int ok = j && apq_push(k, j);
    pthread_mutex_lock(&g_glib_mu);
    if (ok) g_gst.bg_queued++;
    else { g_gst.bg_qfull++; if (q & GPL_PART_PR) nvn_hm_del(&g_gpr_pend, &pk); if (q & GPL_PART_FS) nvn_hm_del(&g_gfs_pend, &fk); }   // (made at the draw instead)
    pthread_mutex_unlock(&g_glib_mu);
    if (!ok) { free(j); return 0; }
    return 1;
}
// a linked pipeline's dynamic state, at its bind (and in each new command buffer): its key's, which the libraries left out
static void gpl_dyn(VkCommandBuffer cb, const pkey *k) {
    vkCmdSetCullMode(cb, k->cull); vkCmdSetFrontFace(cb, k->front ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE); vkCmdSetDepthBiasEnable(cb, k->bias_en);
    vkCmdSetDepthTestEnable(cb, k->dt); vkCmdSetDepthWriteEnable(cb, k->dt && k->dw); vkCmdSetDepthCompareOp(cb, cmpop(k->df)); vkCmdSetStencilTestEnable(cb, k->st);
    vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_BIT, sop(k->sfail[0]), sop(k->szpass[0]), sop(k->szfail[0]), cmpop(k->sf[0]));
    vkCmdSetStencilOp(cb, VK_STENCIL_FACE_BACK_BIT, sop(k->sfail[1]), sop(k->szpass[1]), sop(k->szfail[1]), cmpop(k->sf[1]));
    if (V.gpl_eds3 & GPL_E3_CLAMP) vkCmdSetDepthClampEnableEXT(cb, k->depth_clamp);
    if (V.gpl_eds3 & GPL_E3_POLY) vkCmdSetPolygonModeEXT(cb, k->mode == 0 ? VK_POLYGON_MODE_POINT : k->mode == 1 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL);
    if (V.gpl_eds3 & GPL_E3_CLIP) vkCmdSetDepthClipNegativeOneToOneEXT(cb, k->clip_neg1 && V.has_depth_clip_ctl);
}

// ---- the whole pipeline, compiled in the background to take over from a linked one (below the game's threads) ----
enum { GOQ = 1024 };
static pkey g_goq[GOQ]; static int g_goq_h, g_goq_t, g_goq_on; static pthread_mutex_t g_goq_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_goq_cv = PTHREAD_COND_INITIALIZER;
static void *gpl_opt_thread(void *arg) {
    (void)arg; pthread_setname_np(pthread_self(), "vk gpl whole"); setpriority(PRIO_PROCESS, 0, 10);
    for (;;) {
        pthread_mutex_lock(&g_goq_mu); while (g_goq_h == g_goq_t) pthread_cond_wait(&g_goq_cv, &g_goq_mu);
        pkey k = g_goq[g_goq_t % GOQ]; g_goq_t++; pthread_mutex_unlock(&g_goq_mu);
        vkshader_t *stg[3] = {0}; VkPipeline p = (VkPipeline)1;
        for (int s = 0; s < 3; s++) if (k.key[s]) stg[s] = vk_shader(k.key[s], s);
        uint64_t t0 = tick();
        if (stg[0]) { VkDescriptorSetLayout dsl[5] = {0}; int nsets = 0;
            for (int s = 0; s < 3; s++) if (stg[s]) { dsl[stg[s]->set] = stg[s]->dsl; if (stg[s]->set + 1 > nsets) nsets = stg[s]->set + 1; }
            p = build_pipeline(&k, vk_pipeline_layout_gfx(dsl, nsets), stg); }
        uint64_t dt = tick() - t0;
        pthread_mutex_lock(&g_pipe_mu);
        VkPipeline had = nvn_hm_get(&g_pipes, &k);
        if (had && had != PIPE_PENDING) { if (p != (void *)1) vkDestroyPipeline(V.dev, p, NULL); }   // the prewarm made it meanwhile
        else { nvn_hm_put(&g_pipes, &k, p); if (p != (void *)1) pipe_record(&k); }
        pthread_mutex_unlock(&g_pipe_mu);
        pthread_mutex_lock(&g_glib_mu); if (p == (void *)1) g_gst.opt_fail++; else gpl_stat_add(&g_gst.opt, dt); pthread_mutex_unlock(&g_glib_mu);
        atomic_fetch_add(&g_gpl_swaps, 1);
    }
    return NULL;
}
static void gpl_opt_queue(const pkey *k) {   // (under nothing) a full queue leaves the linked pipeline in use; a later draw queues it again
    pthread_mutex_lock(&g_goq_mu);
    if (!g_goq_on) { g_goq_on = 1; for (int i = 0; i < 2; i++) { pthread_t t; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED); pthread_create(&t, &a, gpl_opt_thread, NULL); pthread_attr_destroy(&a); } }
    int ok = g_goq_h - g_goq_t < GOQ; if (ok) { g_goq[g_goq_h % GOQ] = *k; g_goq_h++; pthread_cond_signal(&g_goq_cv); }
    pthread_mutex_unlock(&g_goq_mu);
    if (ok) { pthread_mutex_lock(&g_pipe_mu); if (!nvn_hm_get(&g_pipes, k)) nvn_hm_put(&g_pipes, k, PIPE_PENDING); pthread_mutex_unlock(&g_pipe_mu); }
    pthread_mutex_lock(&g_glib_mu); if (ok) g_gst.queued++; else g_gst.qfull++; pthread_mutex_unlock(&g_glib_mu);
}
// the host's long-frame line (hle_nvn.c: "new pipelines %u + shader libraries %u, %.1f ms in all"): what the replay thread made at draws. Pipelines: linked ones and
// whole ones built there (the path without libraries); libraries: shader parts made at draws (with the vertex input and output parts' time in the linked pipelines')
void mtl_create_stats(uint64_t *lib, uint64_t *pso) { pthread_mutex_lock(&g_glib_mu); *lib = g_gst.parts[0] + g_gst.parts[1]; *pso = g_gst.links + g_gst.link_fail + g_gst.n_old; pthread_mutex_unlock(&g_glib_mu); }
void mtl_create_ns(uint64_t *lib, uint64_t *pso) { pthread_mutex_lock(&g_glib_mu); *lib = g_gst.ns_part; *pso = g_gst.ns_link + g_gst.ns_old; pthread_mutex_unlock(&g_glib_mu); }
static void gpl_old_built(uint64_t ns) { pthread_mutex_lock(&g_glib_mu); g_gst.n_old++; g_gst.ns_old += ns; pthread_mutex_unlock(&g_glib_mu); }
void vk_gpl_report(void) {   // gfx_vk_present.c, every 600 presents
    if (!V.gpl) return;
    double l50, l95, lmx, k50, k95, kmx, p50, p95, pmx, o50, o95, omx;
    pthread_mutex_lock(&g_glib_mu); typeof(g_gst) s = g_gst; pthread_mutex_unlock(&g_glib_mu);
    gpl_stat_pct(&s.link, &l50, &l95, &lmx); gpl_stat_pct(&s.link_only, &k50, &k95, &kmx); gpl_stat_pct(&s.part, &p50, &p95, &pmx); gpl_stat_pct(&s.opt, &o50, &o95, &omx);
    double b50, b95, bmx; gpl_stat_pct(&s.bg_part, &b50, &b95, &bmx);
    VKLOG("GPL: %llu pipelines linked at draws (ms p50 %.2f p95 %.2f max %.2f, parts made there included; the link alone ms p50 %.2f p95 %.2f max %.2f), %llu failed; "
          "shader parts made at draws: %llu pre-rasterization + %llu fragment, %llu of them alpha test variants and %llu integer output variants (ms p50 %.2f p95 %.2f max %.2f), %llu failed; "
          "shader parts made in the background for draws left out: %llu pre-rasterization + %llu fragment (ms p50 %.2f p95 %.2f max %.2f), %llu jobs queued (%llu queue full: made at the draw); "
          "whole pipelines in the background: %llu queued (%llu queue full), %llu took over (ms p50 %.1f p95 %.1f max %.1f), %llu failed; whole pipelines built at draws %llu (%.1f ms); draws left out for no pipeline %llu",
          (unsigned long long)s.links, l50, l95, lmx, k50, k95, kmx, (unsigned long long)s.link_fail, (unsigned long long)s.parts[0], (unsigned long long)s.parts[1], (unsigned long long)s.parts_at, (unsigned long long)s.parts_int, p50, p95, pmx, (unsigned long long)s.part_fail,
          (unsigned long long)s.parts_bg[0], (unsigned long long)s.parts_bg[1], b50, b95, bmx, (unsigned long long)s.bg_queued, (unsigned long long)s.bg_qfull,
          (unsigned long long)s.queued, (unsigned long long)s.qfull, (unsigned long long)atomic_load(&g_gpl_swaps) - s.opt_fail, o50, o95, omx, (unsigned long long)s.opt_fail, (unsigned long long)s.n_old, s.ns_old / 1e6,
          (unsigned long long)g_vkst.skip_pipe);
}

// ---- the preparation (before the game: datasync_android.c's screen) and the warm-up (a later start, in the background) ----
// Every vertex shader's pre-rasterization part (a second one with depth clip -1..1 when that state cannot be dynamic) and every fragment shader's part, in their
// variant without views, outputs added, fragment depth or dual source, without the alpha test and (for those that take it: vk_shader_alpha_ok) with the GREATER one, and
// (for those that write integers: vkshader_t int_writer) the integer output one without the alpha test. The pipeline cache keeps the driver's compiles (saved every 15 s, so an app
// stopped midway keeps what it made: the next start makes the rest and the made ones come from the cache); DM_ROOT/vk_gpl.id marks a finished preparation of this
// bundle on this driver with these keys (vk_gpl.h gpl_id). With the mark, a start makes the parts again from the cache on two threads below the game's (the warm-up),
// and a draw that comes first makes its part at once (a cache hit). DM_VK_GPL_PREP_THREADS=n (start-up): the preparation's threads (default: the cores but one, 2-8)
static uint64_t *g_gk[2]; static int g_gkn[2], g_gnr; static _Atomic int g_gi_next, g_gi_done, g_gi_made; static gpl_stat g_gprep;
static int g_gtotal(void) { return g_gsn + g_gkn[0] * g_gnr + 3 * g_gkn[1]; }   // the listed parts (gpl_seen_open), pre-rasterization parts, fragment parts, their GREATER alpha test variants, their integer output variants (the last two: a shader without one is skipped)
static int g_gint0(void) { return g_gsn + g_gkn[0] * g_gnr + 2 * g_gkn[1]; }    // the first of the integer output variants (a mark from before them: the preparation of those alone)
static void gpl_keys_load(void) {
    if (g_gk[0]) return;
    uint32_t n = 0; vk_bundle_hash(&n); g_gnr = gpl_rast_variants(V.gpl_eds3, V.has_depth_clip_ctl);
    for (int s = 0; s < 2; s++) { g_gk[s] = malloc(sizeof(uint64_t) * (n ? n : 1)); g_gkn[s] = g_gk[s] ? vk_shader_keys((uint32_t)s, g_gk[s], (int)n) : 0; }
}
static void gpl_part_i(int i) {
    uint64_t made = 0;
    if (i < g_gsn) { gpl_seen_make(&g_gs[i], &made);
        if (made) { atomic_fetch_add(&g_gi_made, 1); pthread_mutex_lock(&g_glib_mu); g_gst.prep_seen++; gpl_stat_add(&g_gprep, made); g_gst.ns_prep_all += made; pthread_mutex_unlock(&g_glib_mu); }
        return; }
    i -= g_gsn;
    if (i < g_gkn[0] * g_gnr) { vkshader_t *vs = vk_shader(g_gk[0][i / g_gnr], 0); if (!vs) return;
        gprkey pk = gpl_pr_key(vs, NULL, 0, 0, 0, 0, 0, 0, gpl_rast_key(V.gpl_eds3, 0, 2, (uint32_t)(i % g_gnr), V.has_depth_clip_ctl)); gpl_part(0, &pk, vs, NULL, &made); }
    else { int j = i - g_gkn[0] * g_gnr, blk = j / g_gkn[1], at = blk == 1, iv = blk == 2; vkshader_t *fs = vk_shader(g_gk[1][j % g_gkn[1]], 1); if (!fs) return;
        if (at && !vk_shader_alpha_ok(fs)) { pthread_mutex_lock(&g_glib_mu); g_gst.prep_at_skip++; pthread_mutex_unlock(&g_glib_mu); return; }   // (no float colour 0: no variant, as at a draw)
        uint32_t fl = fs->fout_locs & 0xff;
        if (iv && !(fs->int_writer && fl && int_out_on())) return;   // (no integer writes: no integer target expected)
        if (iv) { pthread_mutex_lock(&g_glib_mu); g_gst.int_writers++; pthread_mutex_unlock(&g_glib_mu); }
        gfskey fk = gpl_fs_key(fs, 0, 0, at ? NVN_ALPHA_FUNC_GREATER << 4 : iv ? fl << 8 : 0, 1, 0); gpl_part(1, &fk, fs, NULL, &made);
        if (at && made) { pthread_mutex_lock(&g_glib_mu); g_gst.prep_at++; g_gst.ns_prep_at += made; pthread_mutex_unlock(&g_glib_mu); }
        if (iv && made) { pthread_mutex_lock(&g_glib_mu); g_gst.prep_int++; g_gst.ns_prep_int += made; pthread_mutex_unlock(&g_glib_mu); } }
    if (made) { atomic_fetch_add(&g_gi_made, 1); pthread_mutex_lock(&g_glib_mu); gpl_stat_add(&g_gprep, made); g_gst.ns_prep_all += made; pthread_mutex_unlock(&g_glib_mu); }
}
static void *gpl_prep_thread(void *arg) {
    pthread_setname_np(pthread_self(), arg ? "vk gpl warm" : "vk gpl prep"); if (arg) setpriority(PRIO_PROCESS, 0, 10);
    for (;;) { int i = atomic_fetch_add(&g_gi_next, 1); if (i >= g_gtotal()) break; gpl_part_i(i); atomic_fetch_add(&g_gi_done, 1); }
    return NULL;
}
static void gpl_log_done(const char *what, uint64_t ns, int nt) {
    double p50, p95, mx; pthread_mutex_lock(&g_glib_mu); gpl_stat_pct(&g_gprep, &p50, &p95, &mx); pthread_mutex_unlock(&g_glib_mu);
    pthread_mutex_lock(&g_glib_mu); uint64_t at = g_gst.prep_at, ats = g_gst.prep_at_skip, nat = g_gst.ns_prep_at, nall = g_gst.ns_prep_all, ni = g_gst.prep_int, iw = g_gst.int_writers, nsi = g_gst.ns_prep_int; pthread_mutex_unlock(&g_glib_mu);
    double share = nall ? (double)nat / (double)nall : 0;
    VKLOG("GPL: %s: %d vertex shaders (x%d) + %d fragment shaders (+ their GREATER alpha test variants) → %d shader parts made in %.1f s on %d threads (per part ms p50 %.2f p95 %.2f max %.2f); pipeline libraries failed %llu",
          what, g_gkn[0], g_gnr, g_gkn[1], atomic_load(&g_gi_made), ns / 1e9, nt, p50, p95, mx, (unsigned long long)g_gst.part_fail);
    VKLOG("GPL: %s: alpha test (GREATER) fragment parts %llu made, %llu shaders without a float colour 0 skipped; their time %.1f s of %.1f s summed over threads (%.0f%%: about %.1f s of the %.1f s)",
          what, (unsigned long long)at, (unsigned long long)ats, nat / 1e9, nall / 1e9, share * 100, share * ns / 1e9, ns / 1e9);
    if (g_gsn) { pthread_mutex_lock(&g_glib_mu); uint64_t ps = g_gst.prep_seen; pthread_mutex_unlock(&g_glib_mu);
                 VKLOG("GPL: %s: %d parts the last runs made at draws (vk_gpl_seen.bin): %llu made now (the others from this run's tables or failed)", what, g_gsn, (unsigned long long)ps); }
    if (iw) VKLOG("GPL: %s: %llu fragment shaders write integers: their integer output parts %llu made, %.1f s summed over threads (%.0f%%)", what, (unsigned long long)iw, (unsigned long long)ni,
                  nsi / 1e9, nall ? 100.0 * (double)nsi / (double)nall : 0);
}
static void *gpl_warm_main(void *arg) {
    (void)arg; pthread_setname_np(pthread_self(), "vk gpl warm"); uint64_t t0 = tick(); pthread_t t[2]; int nt = 0;
    for (int i = 0; i < 2; i++) if (!pthread_create(&t[nt], NULL, gpl_prep_thread, (void *)1)) nt++;
    for (int i = 0; i < nt; i++) pthread_join(t[i], NULL);
    gpl_log_done("warm-up from the pipeline cache done", tick() - t0, nt);
    return NULL;
}
static void gpl_id_now(gpl_id *id) {
    memset(id, 0, sizeof *id); id->magic = GPL_ID_MAGIC; id->key_ver = GPL_KEY_VER; id->int_ver = GPL_INT_VER; id->vendor = V.props.vendorID; id->device = V.props.deviceID; id->driver = V.props.driverVersion;
    id->eds3 = (uint32_t)V.gpl_eds3; id->sg_pin = V.sg_pin_stages ? V.sg_pin.requiredSubgroupSize : 0; memcpy(id->uuid, V.props.pipelineCacheUUID, 16);
    uint32_t n = 0; id->bundle = vk_bundle_hash(&n); id->nshaders = n;
}
// The front end (datasync_android.c dm_prep_begin), on a thread of its own before the game: 1 when the parts were made now (note: progress, from this thread every
// 100 ms), 0 when there is nothing to do (no bundle: an OpenGL ES game; DM_VK_GPL=0; no libraries on this device; made before: the warm-up is started)
int vk_gpl_prepare(void (*note)(void *u, int done, int total), void *u) {
    const char *e = getenv("DM_VK_GPL"); if (e && !atoi(e)) return 0;
    char path[1100]; const char *b = getenv("DM_VK_SHADERS"), *root = getenv("DM_ROOT"); struct stat st;
    if (!b) { snprintf(path, sizeof path, "%s/shaders.dmsv", root ? root : "."); b = path; }
    if (stat(b, &st) || !root) return 0;
    uint64_t t0 = tick();
    g_vk_early = 1; vk_init(); g_vk_early = 0;
    if (!V.gpl) return 0;
    gpl_id want, have; gpl_id_now(&want); if (!want.nshaders) return 0;
    snprintf(path, sizeof path, "%s/vk_gpl.id", root);
    int had = gpl_id_read(path, &have);
    gpl_seen_open(root, want.bundle);
    gpl_keys_load();
    long nc = sysconf(_SC_NPROCESSORS_ONLN); int nt = nc > 1 ? (int)nc - 1 : 1; if (nt < 2) nt = 2; if (nt > 8) nt = 8;
    if ((e = getenv("DM_VK_GPL_PREP_THREADS")) && atoi(e) > 0) nt = atoi(e) > 16 ? 16 : atoi(e);
    if (had && gpl_id_up_to_date(&want, &have) && have.int_ver != GPL_INT_VER && g_gkn[1]) {
        // a mark from before the integer output parts: those alone, on the preparation's threads and screen (a bundle without integer writers: its shaders read, nothing made)
        uint64_t t1 = tick(); int i0 = g_gint0(), total = g_gtotal() - i0; atomic_store(&g_gi_next, i0); atomic_store(&g_gi_done, i0);
        pthread_t t[16]; int ntr = 0;
        for (int i = 0; i < nt; i++) if (!pthread_create(&t[ntr], NULL, gpl_prep_thread, NULL)) ntr++;
        if (!ntr) gpl_prep_thread(NULL);
        uint64_t saved = t1;
        for (;;) { int d = atomic_load(&g_gi_done) - i0; if (note) note(u, d, total); if (d >= total) break;
            usleep(100000); if (tick() - saved > 15000000000ull) { vk_pipeline_cache_save(); saved = tick(); } }
        for (int i = 0; i < ntr; i++) pthread_join(t[i], NULL);
        pthread_mutex_lock(&g_glib_mu); uint64_t iw = g_gst.int_writers, ni = g_gst.prep_int, fail = g_gst.part_fail; pthread_mutex_unlock(&g_glib_mu);
        if (ni) vk_pipeline_cache_save();
        have.int_ver = GPL_INT_VER; if (!gpl_id_write(path, &have)) VKLOG("[RED] GPL: the reuse mark %s could not be written: the next start looks for integer writers again", path);
        VKLOG("GPL: a mark from before the integer output parts: %llu of %d fragment shaders write integers, %llu parts made in %.1f s on %d threads (pipeline libraries failed %llu)",
              (unsigned long long)iw, g_gkn[1], (unsigned long long)ni, (tick() - t1) / 1e9, ntr ? ntr : 1, (unsigned long long)fail);
        pthread_mutex_lock(&g_glib_mu); g_gst.int_writers = g_gst.prep_int = g_gst.ns_prep_int = g_gst.ns_prep_all = g_gst.prep_seen = 0; memset(&g_gprep, 0, sizeof g_gprep); pthread_mutex_unlock(&g_glib_mu);   // (the warm-up's own)
        atomic_store(&g_gi_next, 0); atomic_store(&g_gi_done, 0); atomic_store(&g_gi_made, 0);
        if (ni) { pthread_t w; if (!pthread_create(&w, NULL, gpl_warm_main, NULL)) pthread_detach(w); return 1; }
    }
    if (had && gpl_id_up_to_date(&want, &have)) {
        VKLOG("GPL: shader parts of this bundle (%u shaders, %016llx) made before on this driver: no preparation; made again from the pipeline cache in the background, first the %d the last runs made at draws", want.nshaders, (unsigned long long)want.bundle, g_gsn);
        pthread_t t; if (!pthread_create(&t, NULL, gpl_warm_main, NULL)) pthread_detach(t);
        return 0; }
    VKLOG("GPL: preparing the shader parts: %s", !had ? "first start (no mark)" : have.magic != GPL_ID_MAGIC || have.key_ver != want.key_ver ? "the parts' key changed"
          : have.bundle != want.bundle || have.nshaders != want.nshaders ? "the shader bundle changed" : have.vendor != want.vendor || have.device != want.device || have.driver != want.driver || memcmp(have.uuid, want.uuid, 16) ? "the driver changed"
          : !gpl_id_same(&want, &have) ? "the dynamic states or the subgroup size changed" : "the last one did not finish");
    pthread_t t[16]; int ntr = 0; uint64_t t1 = tick();
    for (int i = 0; i < nt; i++) if (!pthread_create(&t[ntr], NULL, gpl_prep_thread, NULL)) ntr++;
    if (!ntr) gpl_prep_thread(NULL);
    int total = g_gtotal(); uint64_t saved = tick();
    for (;;) { int d = atomic_load(&g_gi_done); if (note) note(u, d, total); if (d >= total) break;
        usleep(100000); if (tick() - saved > 15000000000ull) { vk_pipeline_cache_save(); saved = tick(); } }
    for (int i = 0; i < ntr; i++) pthread_join(t[i], NULL);
    vk_pipeline_cache_save(); want.complete = 1;
    if (!gpl_id_write(path, &want)) VKLOG("[RED] GPL: the reuse mark %s could not be written: the next start prepares again", path);
    gpl_log_done("preparation done", tick() - t1, ntr ? ntr : 1);
    VKLOG("GPL: preparation %.1f s in all (device made, bundle hashed, parts made, cache saved)", (tick() - t0) / 1e9);
    return 1;
}
