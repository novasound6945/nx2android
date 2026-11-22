// ---------------- programs and pipelines ----------------
static uint64_t key64(const uint8_t k[8]) { uint64_t v = 0; for (int i = 0; i < 8; i++) v = v << 8 | k[i]; return v; }
// DM_NVN_SHDUMP (debugging; hle_nvn.c then keeps each program's control sections): a shader missing from the bundle is written to <files>/shader_<key>.bin as its 0x880-byte
// control section followed by its code area (control +0x704 bytes), so it can be looked for in the game files (Xenoblade Chronicles 3's title had one such program)
static void shader_dump(const prog_t *p, unsigned i, uint64_t kk) {
    static int on = -1; if (on < 0) on = getenv("DM_NVN_SHDUMP") != NULL;
    if (!on || i >= p->nstage || !p->sd[i].control || !p->sd[i].data) return;
    uint32_t ncode; memcpy(&ncode, (const uint8_t *)p->sd[i].control + 0x704, 4);
    const char *dir = getenv("DM_ROOT"); char path[1024]; snprintf(path, sizeof path, "%s/shader_%016llx.bin", dir ? dir : ".", (unsigned long long)kk);
    FILE *f = fopen(path, "wb"); if (!f) return;
    fwrite(p->sd[i].control, 1, 0x880, f); if (ncode < (1u << 24)) fwrite((const void *)(uintptr_t)p->sd[i].data, 1, ncode, f); fclose(f);
    VKLOG("shader %016llx written to %s", (unsigned long long)kk, path);
}
static void bind_program(const p_bind_program *bp) {
    // NVN binds by stage mask (vertex 1, fragment 2, geometry 4, tessellation 8 / 0x10, compute 0x20): only the masked stages change, so a game that keeps
    // its vertex and fragment shaders in programs of their own (NEO, AI: THE SOMNIUM FILES) binds them one after the other and draws with both.
    // prog_key's index is the position in the program's shader array (as the game passed them to SetShaders), not the stage: the stage is the bundle's.
    int vk_shader_stage(uint64_t key);
    uint32_t mask = bp->stages ? (uint32_t)bp->stages & 0x3f : 0x3f;
    for (int st = 0; st < 6; st++) if (mask >> st & 1) S.sh[st] = NULL;
    S.prog = bp->prog;
    if (S.prog) for (unsigned i = 0; i < 6; i++) {
        uint8_t k[8]; if (!prog_key(S.prog, i, k)) break;
        uint64_t kk = key64(k); int st = kk ? vk_shader_stage(kk) : -1;
        if (kk && st < 0) {   // the draw or dispatch is skipped. Logged once per shader (the first 16 binds were 4 shaders of Xenoblade Chronicles 3 over and over, which hid others)
            static uint64_t seen[256]; static int nseen; int known = 0; for (int j = 0; j < nseen; j++) if (seen[j] == kk) { known = 1; break; }
            if (!known && nseen < 256) { seen[nseen++] = kk; void vk_shader_missing(uint64_t key); vk_shader_missing(kk); shader_dump(S.prog, i, kk); } }
        if (st < 0 || st > 5 || !(mask >> st & 1)) continue;
        S.sh[st] = vk_shader(kk, st);
    }
    S.prog_ok = S.sh[0] || S.sh[5];   // a vertex-only program draws depth only (prepare_draw)
}
typedef struct {
    uint64_t key[3]; uint32_t topo, restart, raster_discard, cull, front, mode, depth_clamp, bias_en, debug_mode;
    uint32_t dt, dw, df, st, sf[2], sfail[2], szfail[2], szpass[2];
    uint32_t nattr; struct { uint32_t loc, binding, fmt, off; } attr[16];
    uint32_t nbind; struct { uint32_t binding, stride, rate, divisor; } bind[17];
    uint32_t ncol, cfmt[8], dfmt, samples, a2c, blend_en, logic_en, logic_op, wmask[8];
    struct { uint32_t src, dst, srca, dsta, eq, eqa; } bl[8];
    uint32_t clip_neg1, alt, fdz, dual, gs_split;   // fdz: fragment depth; dual: dual-source blending; gs_split: separated geometry effects/rasterization
    uint32_t atest;   // the alpha test (below): its NVN function (0 = none: ALWAYS, off, no fragment stage); the reference is a push constant, not in the key
    uint64_t um[3], im[3];   // each stage's sampled bindings given integer views (vk_shader_module)
} pkey;
static nvn_hmap g_pipes = {.klen = sizeof(pkey)}; static pthread_mutex_t g_pipe_mu = PTHREAD_MUTEX_INITIALIZER;   // the replay thread and the prewarm threads
static VkPipeline build_pipeline(const pkey *k, VkPipelineLayout layout, vkshader_t *const stg[3]);
static void pipe_record(const pkey *k);
// The last key and its pipeline (the replay thread's only; vk_pipes_drop_idle, on that thread too, clears it): runs of draws with one pipeline skip the 992-byte hash and the lock
static pkey g_last_k; static VkPipeline g_last_p;
// 1 + the present each graphics pipeline was last drawn with, by handle (the replay thread's only: vk_pipes_drop_idle), noted once a present on the fast path. Only under
// memory pressure (gfx_vk_core.c g_mem_pressure), the only time pipelines go: g_seen_since is the present from which every pipeline drawn with is in it (~0: not under
// pressure now), and one not in it since then counts as last drawn with then
static nvn_hmap g_pipe_seen = {.klen = sizeof(VkPipeline)}; static uint64_t g_last_seen, g_seen_since = ~0ull;
static void pipe_seen(VkPipeline p) {
    extern uint64_t g_tex_frame; extern _Atomic int g_mem_pressure; uint64_t now = __atomic_load_n(&g_tex_frame, __ATOMIC_RELAXED); g_last_seen = now;
    if (!atomic_load_explicit(&g_mem_pressure, memory_order_relaxed)) { g_seen_since = ~0ull; return; }
    if (g_seen_since == ~0ull) g_seen_since = now;
    if (p && p != (void *)1) nvn_hm_put(&g_pipe_seen, &p, (void *)(uintptr_t)(now + 1));
}
// Pipelines built in the background. A pipeline met for the first time costs the driver's compile on the replay thread, 20-35 ms each on Adreno 830 (no pipeline
// libraries there): in Super Mario Odyssey the first cap throw at a breakable object, the first swim, made 2-7 new pipelines a frame (175 ms frames, seen as 40, 30
// fps or lower for a moment), then never again in that run; the prewarm (below) covers what an earlier run met. A draw whose pipeline does not exist yet is queued for
// a background thread and left out until it is built, a few frames, when every target of its pass was drawn in each of the two presents before: a target drawn every
// frame shows it a few frames late. Not two presents at any time: an image kept from the title screen and drawn once at a scene's load lost SMO's fog for the run. A target drawn for the first time (an environment map made once) builds at once, as a draw left out there would stay missing, and so
// do rasterizer-discard and transform-feedback draws (they write memory later draws read). Age goes by the image: counted by size and format instead, SMO's first scene
// after Resume lost its coin counter and tutorial movie for good and showed the Cap Kingdom fog unparted (targets drawn once, in classes the title screen had aged).
// So a capture's new full-screen targets still build at once (SMO's first frog capture: 33 pipelines, two 300 ms frames, once). So do passes into a linear render target:
// the CPU reads it (lin_back), and SMO's fog wave solver feeds what it reads into the next step, so one step left out lost the Cap Kingdom fog for the run.
// DM_VK_ASYNC_PIPE=0 (live): every pipeline is built at once
#define PIPE_PENDING ((VkPipeline)(uintptr_t)2)
enum { APQ = 256 };
static pkey g_apq[APQ]; static int g_apq_h, g_apq_t, g_apq_on;   // queued keys (a draw left out counts as skipped for no pipeline)
static pthread_mutex_t g_apq_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_apq_cv = PTHREAD_COND_INITIALIZER;
static void *async_pipe_thread(void *arg) {
    (void)arg; pthread_setname_np(pthread_self(), "vk async pipe");
    for (;;) {
        pthread_mutex_lock(&g_apq_mu); while (g_apq_h == g_apq_t) pthread_cond_wait(&g_apq_cv, &g_apq_mu);
        pkey k = g_apq[g_apq_t % APQ]; g_apq_t++; pthread_mutex_unlock(&g_apq_mu);
        vkshader_t *stg[3] = {0}; VkPipeline p = (VkPipeline)1;
        for (int s = 0; s < 3; s++) if (k.key[s] && !(s == 1 && k.raster_discard)) stg[s] = vk_shader(k.key[s], s);
        if (stg[0]) { VkDescriptorSetLayout dsl[5] = {0}; int nsets = 0;
            for (int s = 0; s < 3; s++) if (stg[s]) { dsl[stg[s]->set] = stg[s]->dsl; if (stg[s]->set + 1 > nsets) nsets = stg[s]->set + 1; }
            uint64_t t0 = tick(); p = build_pipeline(&k, vk_pipeline_layout_gfx(dsl, nsets), stg);
            { static int n; if (n++ < 6) VKLOG("pipeline built in the background in %.1f ms (shaders %016llx %016llx)", (tick() - t0) / 1e6, (unsigned long long)k.key[0], (unsigned long long)k.key[1]); } }
        pthread_mutex_lock(&g_pipe_mu);
        VkPipeline had = nvn_hm_get(&g_pipes, &k);
        if (had && had != PIPE_PENDING) { if (p != (void *)1) vkDestroyPipeline(V.dev, p, NULL); }   // a prewarm thread made it meanwhile (as graphics_pipeline does: the one in the table may be bound already, the new one goes)
        else { nvn_hm_put(&g_pipes, &k, p); if (p != (void *)1) pipe_record(&k); }
        pthread_mutex_unlock(&g_pipe_mu);
    }
    return NULL;
}
static int async_pipe(const pkey *k) {   // 1: queued (the draw is left out); under nothing (takes g_pipe_mu)
    if (!g_async_pipe || k->raster_discard || (S.sh[0] && S.sh[0]->xfb) || (S.sh[2] && S.sh[2]->xfb)) return 0;
    for (int i = 0; i < S.ncol && i < 8; i++) if (S.rt_has_col[i] && (S.rt_col[i].d.flags & NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) return 0;   // (the CPU reads it: lin_back)
    int natt = 0; for (int i = 0; i < 9; i++) { const vkimg_t *a = P_att[i].im; if (!a) continue; natt++; if (a->rt_frames < 3) {
            if (g_prof_on) { static int n; if (n++ < 300) VKLOG("pipeline built here: attachment %d %ux%u nvn format %#x drawn in %llu presents in a row (shaders %016llx %016llx)", i, a->w, a->h, a->nvn_format, (unsigned long long)a->rt_frames, (unsigned long long)k->key[0], (unsigned long long)k->key[1]); }
            return 0; } }   // (counted in this present too)
    if (!natt) return 0;
    pthread_mutex_lock(&g_apq_mu);
    if (!g_apq_on) { g_apq_on = 1; for (int i = 0; i < 2; i++) { pthread_t t; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED); pthread_create(&t, &a, async_pipe_thread, NULL); pthread_attr_destroy(&a); } }
    int ok = g_apq_h - g_apq_t < APQ; if (ok) { g_apq[g_apq_h % APQ] = *k; g_apq_h++; pthread_cond_signal(&g_apq_cv); }
    pthread_mutex_unlock(&g_apq_mu);
    if (ok) { pthread_mutex_lock(&g_pipe_mu); if (!nvn_hm_get(&g_pipes, k)) nvn_hm_put(&g_pipes, k, PIPE_PENDING); pthread_mutex_unlock(&g_pipe_mu); }
    return ok;
}
// Graphics pipeline libraries (gfx_vk_gpl.inc): a draw that can take a linked pipeline does until its whole one is built (vk_gpl.h gpl_route), and none is left out
static nvn_hmap g_gfast = {.klen = sizeof(pkey)};   // linked pipelines by key (the replay thread's only; (VkPipeline)1: the link failed), kept after the whole one took over (command buffers may hold them)
static nvn_hmap g_gtook = {.klen = sizeof(pkey)};   // keys whose whole pipeline has drawn after their linked one (the replay thread's only; vk_ev VKEV_TAKEOVER once per key)
static int g_pipe_new;   // graphics_pipeline linked a pipeline at this draw (1), or a whole one took over at it (2): the crumbs' flags
// graphics_pipeline: how this draw's pipeline is new (the hang report's first uses, vk_hang_report): 0 drawn before, 1 built at this draw, 2 linked at it, 3 a whole one
// taking over from its linked one, 4 its first draw after a background build or a prewarm (g_pipe_drawn: the pipelines drawn with, by handle; the replay thread's)
static int g_pipe_first; static nvn_hmap g_pipe_drawn = {.klen = sizeof(VkPipeline)};
static _Atomic uint64_t g_gpl_swaps;   // whole pipelines finished in the background (graphics_pipeline looks again at a memoized linked one when it moves)
static int g_pipe_fast, g_last_fast; static uint64_t g_last_swaps, g_gpl_dyn_gen;   // graphics_pipeline returned a linked pipeline (its dynamic state: gpl_dyn); the memo's; the command buffer gpl_dyn last set it in
// DM_VK_SHOTS_FAST (gfx_vk_present.c fastshot_frame): while it is armed, each draw that took a linked pipeline is counted here, by shader keys, until the next present takes the tally
int g_fastshot_on; uint32_t g_fast_draws, g_fast_nlist; struct { uint64_t vs, fs; uint32_t samples, atest; } g_fast_list[16];
void vk_fast_take(uint32_t *draws, uint32_t *n, void *list, size_t lsz) { *draws = g_fast_draws; *n = g_fast_nlist; memcpy(list, g_fast_list, lsz < sizeof g_fast_list ? lsz : sizeof g_fast_list); g_fast_draws = 0; g_fast_nlist = 0; }
static void fast_note(const pkey *k) {
    g_fast_draws++;
    for (uint32_t i = 0; i < g_fast_nlist; i++) if (g_fast_list[i].vs == k->key[0] && g_fast_list[i].fs == k->key[1] && g_fast_list[i].samples == k->samples && g_fast_list[i].atest == k->atest) return;
    if (g_fast_nlist < 16) { g_fast_list[g_fast_nlist].vs = k->key[0]; g_fast_list[g_fast_nlist].fs = k->key[1]; g_fast_list[g_fast_nlist].samples = k->samples; g_fast_list[g_fast_nlist].atest = k->atest; g_fast_nlist++; }
}
static VkPipeline gpl_link(const pkey *k, VkPipelineLayout layout); static int gpl_eligible(const pkey *k); static void gpl_opt_queue(const pkey *k); static void gpl_old_built(uint64_t ns);
static VkPipeline graphics_pipeline(const pkey *k, VkPipelineLayout layout) {
    uint64_t swaps = atomic_load(&g_gpl_swaps); g_pipe_new = 0; g_pipe_first = 0;
    if (g_last_p && !memcmp(k, &g_last_k, sizeof *k) && (!g_last_fast || g_last_swaps == swaps)) { g_pipe_fast = g_last_fast;
        if (!g_last_fast) { extern uint64_t g_tex_frame; if (g_last_seen != __atomic_load_n(&g_tex_frame, __ATOMIC_RELAXED)) pipe_seen(g_last_p); }   // (a linked pipeline never goes: vk_pipes_drop_idle takes whole ones)
        return g_last_p == (void *)1 ? VK_NULL_HANDLE : g_last_p; }
    g_pipe_fast = 0;
    pthread_mutex_lock(&g_pipe_mu); VkPipeline p = nvn_hm_get(&g_pipes, k); pthread_mutex_unlock(&g_pipe_mu);
    int opt = !p ? GPL_OPT_NONE : p == PIPE_PENDING ? GPL_OPT_PENDING : p == (void *)1 ? GPL_OPT_FAILED : GPL_OPT_OK;
    // DM_VK_GPL_LINK=0 (live, debugging: a DM_CTL env line): no linked pipelines, each new pipeline is built whole where it is drawn as without libraries,
    // while the rest of the library setup (dynamic rendering, independent-set layouts, the alpha reference as a push constant) stays: tells their artefacts apart
    // DM_VK_GPL_LINK_MS=0 (live, the same for multisampled draws only): tells a multisample-state cause from the rest
    // DM_VK_X_GPL_FAST=1 (live, diagnosis): every draw that can take a linked pipeline does, whole ones never take over, so what only linked pipelines do shows
    // in minutes instead of in the few frames after a scene change (Fire Emblem Engage's device lost)
    static int link_on = 1, link_ms = 1, fast_only = 0; static unsigned lk; if (!(lk++ & 255)) { const char *e = getenv("DM_VK_GPL_LINK"); link_on = !(e && !atoi(e));
        e = getenv("DM_VK_GPL_LINK_MS"); link_ms = !(e && !atoi(e)); e = getenv("DM_VK_X_GPL_FAST"); fast_only = e && atoi(e); }
    if (V.gpl && link_on && (link_ms || k->samples <= 1) && (fast_only ? gpl_eligible(k) : gpl_route(V.gpl, gpl_eligible(k), opt) == GPL_USE_FAST)) {
        VkPipeline f = nvn_hm_get(&g_gfast, k);
        if (!f) { f = gpl_link(k, layout); nvn_hm_put(&g_gfast, k, f ? f : (VkPipeline)1); if (f && opt == GPL_OPT_NONE) { gpl_opt_queue(k); opt = GPL_OPT_PENDING; }
                  if (f) { g_sub_links++; g_pipe_new = 1; g_pipe_first = 2; vk_ev(VKEV_LINK, k->key[0], k->key[1], (uint64_t)(uintptr_t)f); } }
        else if (f != (void *)1 && opt == GPL_OPT_NONE) gpl_opt_queue(k);   // (its queue was full before)
        if (f && f != (void *)1) { g_last_k = *k; g_last_p = f; g_last_fast = 1; g_last_swaps = swaps; g_pipe_fast = 1; return f; }
    }   // (no linked pipeline: as without libraries)
    if (p == PIPE_PENDING) return VK_NULL_HANDLE;
    if (p) { g_last_k = *k; g_last_p = p; g_last_fast = 0; pipe_seen(p);
        if (V.gpl && p != (void *)1) { VkPipeline f = nvn_hm_get(&g_gfast, k);   // the whole pipeline's first draw where a linked one drew before (vk_device_lost's report)
            if (f && f != (void *)1 && !nvn_hm_get(&g_gtook, k)) { nvn_hm_put(&g_gtook, k, (void *)1); g_sub_takeovers++; g_pipe_new = 2; g_pipe_first = 3; vk_ev(VKEV_TAKEOVER, k->key[0], k->key[1], (uint64_t)(uintptr_t)p); } }
        if (p != (void *)1 && !nvn_hm_get(&g_pipe_drawn, &p)) { nvn_hm_put(&g_pipe_drawn, &p, (void *)1); if (!g_pipe_first) g_pipe_first = 4; }
        return p == (void *)1 ? VK_NULL_HANDLE : p; }
    if (async_pipe(k)) return VK_NULL_HANDLE;
    vkshader_t *stg[3] = {S.sh[0], S.sh[1], S.sh[2]};
    uint64_t tb = tick(); p = build_pipeline(k, layout, stg); uint64_t tbn = tick() - tb; if (g_prof_on) g_prof_ns[0] += tbn; g_vkst.pipes_built++; if (V.gpl) gpl_old_built(tbn);
    pthread_mutex_lock(&g_pipe_mu);
    VkPipeline had = nvn_hm_get(&g_pipes, k);
    if (had) { if (p != (void *)1) vkDestroyPipeline(V.dev, p, NULL); p = had; }   // a prewarm thread made it meanwhile
    else { nvn_hm_put(&g_pipes, k, p); if (p != (void *)1) pipe_record(k); }
    pthread_mutex_unlock(&g_pipe_mu);
    pipe_seen(p);
    if (p != (void *)1) { nvn_hm_put(&g_pipe_drawn, &p, (void *)1); g_pipe_first = 1; }
    return p == (void *)1 ? VK_NULL_HANDLE : p;
}
// Graphics pipelines not drawn with for idle presents under memory pressure go (the replay thread, at a submission's start: gfx_vk_core.c vk_mem_tick's trim), and one met again is built
// again (in the background where async_pipe allows, its compile helped by the driver's pipeline cache). Each pipeline holds memory of the driver's own: Breath of the
// Wild's 6,561 recorded pipelines, all prewarmed, made its process 470 MB larger at the title screen (PSS) on an AYN Thor (Adreno 740, 8 GB) with PurpleVK T30, and
// 790 MB with Qualcomm's driver 512.676.53: about 70 and 120 KB a pipeline. Prewarmed pipelines never drawn with go first. Built ones stay in the table's place until they go: g_last_p, S.bound_pipe (reset at each submission) and the replay's draws hold
// none past the submission
uint64_t vk_pipes_drop_idle(uint64_t idle) {
    extern uint64_t g_tex_frame; uint64_t now = __atomic_load_n(&g_tex_frame, __ATOMIC_RELAXED) + 1, n = 0; static pkey ks[64];   // (the replay thread only)
    if (g_seen_since == ~0ull) return 0;   // (not noted yet: none has been idle under pressure)
    pthread_mutex_lock(&g_pipe_mu);
    for (int c = 64; c == 64;) { c = 0;
        for (uint32_t i = 0; i < g_pipes.cap && c < 64; i++) { VkPipeline p = g_pipes.vals[i]; if (!p || p == PIPE_PENDING || p == (void *)1) continue;
            uintptr_t seen = (uintptr_t)nvn_hm_get(&g_pipe_seen, &p); if (seen < g_seen_since + 1) seen = g_seen_since + 1; if (now - seen < idle) continue;
            memcpy(&ks[c++], g_pipes.keys + (size_t)i * g_pipes.klen, sizeof(pkey)); }
        for (int j = 0; j < c; j++) { VkPipeline p = nvn_hm_get(&g_pipes, &ks[j]); if (!p || p == PIPE_PENDING || p == (void *)1) continue;
            nvn_hm_del(&g_pipes, &ks[j]); nvn_hm_del(&g_pipe_seen, &p); nvn_hm_del(&g_pipe_drawn, &p); vk_defer_free_pipeline(p); n++; }
    }
    g_last_p = VK_NULL_HANDLE;
    pthread_mutex_unlock(&g_pipe_mu);
    return n;
}
uint64_t vk_pipes_live(void) { pthread_mutex_lock(&g_pipe_mu); uint64_t n = g_pipes.n; pthread_mutex_unlock(&g_pipe_mu); return n; }
// ---- pipeline prewarm: every pipeline the game needed is recorded (DM_ROOT/vk_pipelines.bin) and made again at the next boot on background
// threads, before the game reaches it, so a scene seen once never stalls a frame on a pipeline compile again (the driver's compile, not the
// pipeline cache's lookup, is the cost: tens of milliseconds per pipeline on Adreno). DM_VK_NOPREWARM=1 turns it off.
#define PIPE_MAGIC 0x35455050u   // "PPE5": geometry effects/rasterization have separate pipeline keys, and the alpha reference is out of the key (a push constant); the header also holds sizeof(pkey), so a changed key layout starts a new file
// debug_mode shifted the key's fields without changing its padded size; PPE1 prewarm records otherwise decoded as 76 colour targets and crashed Adreno at startup.
static FILE *g_prec;
// The records are written by a thread of their own: a flush to the app's files folder (FUSE) took tens of milliseconds, and made under g_pipe_mu it held up the replay's
// pipeline lookups (a 66 ms frame with no pipeline built in it)
enum { PREC_Q = 64 };
static pkey g_precq[PREC_Q]; static int g_precq_n, g_precw_on; static pthread_mutex_t g_precq_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_precq_cv = PTHREAD_COND_INITIALIZER;
static void *pipe_record_thread(void *arg) {
    (void)arg; pthread_setname_np(pthread_self(), "vk pipe record"); static pkey b[PREC_Q];
    for (;;) {
        pthread_mutex_lock(&g_precq_mu); while (!g_precq_n) pthread_cond_wait(&g_precq_cv, &g_precq_mu);
        int n = g_precq_n; memcpy(b, g_precq, sizeof *b * (size_t)n); g_precq_n = 0; pthread_mutex_unlock(&g_precq_mu);
        fwrite(b, sizeof *b, (size_t)n, g_prec); fflush(g_prec);
    }
    return NULL;
}
static nvn_hmap g_prec_have = {.klen = sizeof(pkey)};   // keys in the record (read at boot, or written since): a pipeline vk_pipes_drop_idle took, built again, is not written again
static void pipe_record(const pkey *k) {   // (under g_pipe_mu: only queued here)
    if (!g_prec || k->alt || nvn_hm_get(&g_prec_have, k)) return;   // DM_VK_SPV_ALT's pipelines are a debugging run's own
    nvn_hm_put(&g_prec_have, k, (void *)1);
    pthread_mutex_lock(&g_precq_mu);
    if (!g_precw_on) { g_precw_on = 1; pthread_t t; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED); pthread_create(&t, &a, pipe_record_thread, NULL); pthread_attr_destroy(&a); }
    if (g_precq_n < PREC_Q) g_precq[g_precq_n++] = *k;   // (a full queue drops the record: that pipeline is only not prewarmed next time)
    pthread_cond_signal(&g_precq_cv); pthread_mutex_unlock(&g_precq_mu);
}
static pkey *g_pw; static size_t g_pw_n; static volatile size_t g_pw_next, g_pw_done;
// The prewarm stops once the device's MemAvailable is under DM_MEM_FLOOR_MB (default 1536; 0: every recorded pipeline, as before; gfx_vk_core.c vk_mem_tick): each pipeline
// holds memory of the driver's own (vk_pipes_drop_idle), and the record keeps every pipeline any earlier run met (6,579 for Breath of the Wild after a few sessions: 470 to
// 790 MB on an AYN Thor, as above). It does not start again: the pipelines it would make are the ones vk_pipes_drop_idle takes first. A device with memory to spare still makes them all before the game meets them
static int prewarm_room(void) {
    long long floor_mb = vk_mem_floor_mb(); if (!floor_mb) return 1;
    long long a = vk_mem_avail_mb(); if (a < 0 || a >= floor_mb) return 1;
    static _Atomic int said; if (!atomic_exchange(&said, 1)) VKLOG("prewarm: stopped at %zu of %zu pipelines, MemAvailable %lld MB is under %lld MB (DM_MEM_FLOOR_MB)", g_pw_done, g_pw_n, a, floor_mb);
    return 0;
}
static void *prewarm_thread(void *arg) {
    (void)arg; pthread_setname_np(pthread_self(), "vk prewarm");
    setpriority(PRIO_PROCESS, 0, 10);   // below the game's threads: prewarm uses idle cores
    for (;;) {
        size_t i = __atomic_fetch_add(&g_pw_next, 1, __ATOMIC_RELAXED); if (i >= g_pw_n) break;
        if (!prewarm_room()) { __atomic_store_n(&g_pw_next, g_pw_n, __ATOMIC_RELAXED); break; }
        const pkey *k = &g_pw[i];
        pthread_mutex_lock(&g_pipe_mu); int have = nvn_hm_get(&g_pipes, k) != NULL; pthread_mutex_unlock(&g_pipe_mu);
        if (have) continue;
        vkshader_t *stg[3] = {0}; int ok = 1;
        for (int s = 0; s < 3; s++) if (k->key[s] && !(s == 1 && k->raster_discard)) { stg[s] = vk_shader(k->key[s], s); if (!stg[s]) ok = 0; }
        if (!ok || !stg[0]) continue;
        VkDescriptorSetLayout dsl[5] = {0}; int nsets = 0;
        for (int s = 0; s < 3; s++) if (stg[s]) { dsl[stg[s]->set] = stg[s]->dsl; if (stg[s]->set + 1 > nsets) nsets = stg[s]->set + 1; }
        VkPipeline p = build_pipeline(k, vk_pipeline_layout_gfx(dsl, nsets), stg);
        pthread_mutex_lock(&g_pipe_mu);
        if (!nvn_hm_get(&g_pipes, k)) nvn_hm_put(&g_pipes, k, p); else if (p != (void *)1) vkDestroyPipeline(V.dev, p, NULL);
        pthread_mutex_unlock(&g_pipe_mu);
        size_t d = __atomic_add_fetch(&g_pw_done, 1, __ATOMIC_RELAXED);
        if (d == g_pw_n || d % 500 == 0) VKLOG("prewarm: %zu of %zu pipelines", d, g_pw_n);
    }
    return NULL;
}
static void prewarm_start(void) {
    const char *root = getenv("DM_ROOT"); if (!root) return;
    char path[1024]; snprintf(path, sizeof path, "%s/vk_pipelines.bin", root);
    uint32_t hdr[2] = {PIPE_MAGIC, (uint32_t)sizeof(pkey)}, fh[2] = {0};
    FILE *f = fopen(path, "rb");
    if (f && fread(fh, sizeof fh, 1, f) == 1 && fh[0] == hdr[0] && fh[1] == hdr[1]) {
        fseek(f, 0, SEEK_END); long n = (ftell(f) - (long)sizeof fh) / (long)sizeof(pkey); fseek(f, sizeof fh, SEEK_SET);
        if (n > 0) { g_pw = malloc((size_t)n * sizeof(pkey)); g_pw_n = fread(g_pw, sizeof(pkey), (size_t)n, f); }
    }
    if (f) fclose(f);
    // rewrite the file with the records read (dropping a torn last record), then append to it
    g_prec = fopen(path, "wb"); if (g_prec) { fwrite(hdr, sizeof hdr, 1, g_prec); if (g_pw_n) fwrite(g_pw, sizeof(pkey), g_pw_n, g_prec); fflush(g_prec); }
    pthread_mutex_lock(&g_pipe_mu); for (size_t i = 0; i < g_pw_n; i++) nvn_hm_put(&g_prec_have, &g_pw[i], (void *)1); pthread_mutex_unlock(&g_pipe_mu);
    if (!g_pw_n || getenv("DM_VK_NOPREWARM")) { VKLOG("prewarm: %zu pipelines recorded%s", g_pw_n, g_pw_n ? ", off (DM_VK_NOPREWARM)" : ""); return; }
    long nc = sysconf(_SC_NPROCESSORS_ONLN); int nt = nc >= 8 ? 3 : nc >= 4 ? 2 : 1;
    VKLOG("prewarm: %zu recorded pipelines on %d threads", g_pw_n, nt);
    for (int i = 0; i < nt; i++) { pthread_t t; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED); pthread_create(&t, &a, prewarm_thread, NULL); pthread_attr_destroy(&a); }
}
// ---- what each thread is building, for the crash dump. Issue #64: Super Mario Bros. Wonder crashed in the Adreno 750's shader compiler (libllvm-qgl.so, driver 512.762)
// at its title scene, with no log line to say which pipeline or which thread; and a pipeline that crashes is never recorded for the prewarm, so every run met it again.
// A slot per thread in a build: the crash dump prints each (the crashing thread's marked), with the shaders and the state that pick the driver's compile
enum { NBUILD = 16 };
static struct { _Atomic int tid; volatile int phase; char what[400]; } g_build[NBUILD];   // phase 0: claimed, 1: making shader modules, 2: in the driver's pipeline compile
static int build_begin(void) {   // the slot, or -1 when all are taken (nothing noted)
    int me = gettid(); for (int i = 0; i < NBUILD; i++) { int z = 0; if (atomic_compare_exchange_strong(&g_build[i].tid, &z, me)) { g_build[i].phase = 0; return i; } }
    return -1;
}
static void build_end(int i) { if (i >= 0) { g_build[i].phase = 0; atomic_store(&g_build[i].tid, 0); } }
static void build_dump(void) {   // (the crash handler: no locks, no allocation)
    int me = gettid(), n = 0;
    for (int i = 0; i < NBUILD; i++) { int t = atomic_load(&g_build[i].tid); if (!t || !g_build[i].phase) continue; n++;
        char nm[64]; crash_thread_name((unsigned)t, nm, sizeof nm);
        fprintf(stderr, "[dm] [vk] %s thread %#x '%s' was %s: %s\n", t == me ? "this (the crashing)" : "another", (unsigned)t, nm,
                g_build[i].phase == 1 ? "making its shader modules" : "in the driver's pipeline compile", g_build[i].what); }
    if (!n) fprintf(stderr, "[dm] [vk] no pipeline was being built\n");
}
// the vertex inputs a vertex shader reads with the other integer signedness than their format's (vk_shader_module vi): a bit per location. build_pipeline and gpl_link
static uint32_t vi_flip_of(const pkey *k, const vkshader_t *vs) {
    uint32_t vi_flip = 0;
    if (vs) for (uint32_t a = 0; a < k->nattr; a++) {
        uint32_t l = k->attr[a].loc; int kind = vertex_integer_kind((VkFormat)k->attr[a].fmt);
        if ((kind == 2 && (vs->local[1] >> l & 1)) || (kind == 1 && (vs->local[2] >> l & 1))) vi_flip |= 1u << l;
    }
    return vi_flip;
}
// the fragment shader's float outputs that meet a UINT (bits 8-15) / SINT (bits 16-23) colour target, as vk_shader_module's variant bits: those outputs are retyped to
// the integer of the same bits (vk_spv_fragout.h; Maxwell writes the output register's bits, Vulkan leaves a float output into an integer attachment undefined). From the
// attachments' formats, which are in the key already; 0 for a shader with no such output, so its module and its preparation's part are the ones as before.
// When every attached float output meets a UINT target, the float outputs with no attachment are retyped too (no target reads them, so their type does not matter):
// the variant is then every float output UINT, the one the preparation makes for a shader that writes integers (gfx_vk_gpl.inc, vk_spv_fragout.h spv_int_writer),
// whatever the draw attaches of them (Luigi's Mansion 3: shaders with outputs 0-2 into its two-target R32Uint + RGBA8Uint G-buffer).
// DM_VK_INT_OUT=0 (start-up, A/B): never
static int color_int_kind(uint32_t f) {   // 1 UINT, 2 SINT, 0 otherwise
    switch ((VkFormat)f) {
    case VK_FORMAT_R8_UINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8B8_UINT: case VK_FORMAT_B8G8R8_UINT: case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_B8G8R8A8_UINT:
    case VK_FORMAT_A8B8G8R8_UINT_PACK32: case VK_FORMAT_A2R10G10B10_UINT_PACK32: case VK_FORMAT_A2B10G10R10_UINT_PACK32: case VK_FORMAT_R16_UINT: case VK_FORMAT_R16G16_UINT:
    case VK_FORMAT_R16G16B16_UINT: case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32B32_UINT: case VK_FORMAT_R32G32B32A32_UINT: return 1;
    case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8B8_SINT: case VK_FORMAT_B8G8R8_SINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_B8G8R8A8_SINT:
    case VK_FORMAT_A8B8G8R8_SINT_PACK32: case VK_FORMAT_A2R10G10B10_SINT_PACK32: case VK_FORMAT_A2B10G10R10_SINT_PACK32: case VK_FORMAT_R16_SINT: case VK_FORMAT_R16G16_SINT:
    case VK_FORMAT_R16G16B16_SINT: case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32_SINT: case VK_FORMAT_R32G32B32A32_SINT: return 2;
    default: return 0;
    }
}
static int int_out_on(void) {
    static int on = -1; if (on < 0) { const char *e = getenv("DM_VK_INT_OUT"); on = !e || atoi(e) != 0; if (!on) VKLOG("DM_VK_INT_OUT=0: float outputs into integer targets as they are"); }
    return on;
}
static uint32_t int_out_bits(const pkey *k, const vkshader_t *fs) {
    if (!int_out_on() || !fs || k->raster_discard || !fs->fout_locs) return 0;
    uint32_t u = 0, s = 0, att = 0, fl = fs->fout_locs & 0xff;
    for (uint32_t i = 0; i < k->ncol && i < 8; i++) { int t = color_int_kind(k->cfmt[i]); if (t == 1) u |= 1u << i; else if (t == 2) s |= 1u << i; if (k->cfmt[i]) att |= 1u << i; }
    u &= fl; s &= fl;
    if (u && !s && !(fl & att & ~u)) u = fl;   // (every attached float output UINT: the unattached ones too)
    return u << 8 | s << 16;
}
static VkPipeline build_pipeline(const pkey *k, VkPipelineLayout layout, vkshader_t *const stg_in[3]) {
    // DM_VK_PIPE_LOG: one line per pipeline built, with the state that decides how a material is drawn (shaders, cull/front, depth test/write/func,
    // colour formats, blend). For a game where some materials render wrong and others do not, the lines of the wrong material name the state to compare.
    { static int on = -1; if (on < 0) on = getenv("DM_VK_PIPE_LOG") ? atoi(getenv("DM_VK_PIPE_LOG")) : 0;
      if (on > 0) { static int n; if (n++ < 600)
        VKLOG("pipeline %d: shaders %016llx %016llx %016llx topo %u cull %u front %u mode %u depth test %u write %u func %u stencil %u | colour targets %u (formats %u %u %u %u) depth format %u | samples %u a2c %u blend %u logic %u/%u | attrs %u binds %u",
              n, (unsigned long long)k->key[0], (unsigned long long)k->key[1], (unsigned long long)k->key[2], k->topo, k->cull, k->front, k->mode,
              k->dt, k->dw, k->df, k->st, k->ncol, k->cfmt[0], k->cfmt[1], k->cfmt[2], k->cfmt[3], k->dfmt, k->samples, k->a2c, k->blend_en, k->logic_en, k->logic_op, k->nattr, k->nbind); } }
    VkPipeline p; vkshader_t *stg[3] = {stg_in[0], stg_in[1], stg_in[2] ? vk_shader_gs_topo(stg_in[2], (VkPrimitiveTopology)k->topo) : NULL};
    if (k->gs_split && (k->gs_split > 2 || !stg[2] || !vk_shader_gs_split_ready(stg[2]))) return (VkPipeline)1;
    int bs = build_begin();
    if (bs >= 0) { snprintf(g_build[bs].what, sizeof g_build[bs].what, "graphics pipeline, shaders %016llx %016llx %016llx; topology %u, polygon mode %u, samples %u, "
            "%u colour targets (formats %u %u %u %u), depth format %u; raster discard %u, fragment depth variant %u, dual source %u, alpha test %u, transform feedback %d, logic op %u, "
            "vertex attributes %u in %u bindings, integer-texel views %#llx/%#llx %#llx/%#llx, alternative code %u",
            (unsigned long long)k->key[0], (unsigned long long)k->key[1], (unsigned long long)k->key[2], k->topo, k->mode, k->samples, k->ncol, k->cfmt[0], k->cfmt[1], k->cfmt[2], k->cfmt[3],
            k->dfmt, k->raster_discard, k->fdz, k->dual, k->atest, stg[0] ? stg[0]->xfb : 0, k->logic_en ? k->logic_op : 0, k->nattr, k->nbind,
            (unsigned long long)k->um[0], (unsigned long long)k->im[0], (unsigned long long)k->um[1], (unsigned long long)k->im[1], k->alt); g_build[bs].phase = 1; }
    VkPipelineShaderStageCreateInfo ss[3]; uint32_t ns = 0;
    VkShaderStageFlagBits fl[3] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_GEOMETRY_BIT};
    // the outputs each stage must declare for the next (vk_shader_module): vertex → geometry or fragment, geometry → fragment
    const vkshader_t *fs = stg[1] && !k->raster_discard ? stg[1] : NULL; uint32_t xo[3] = {0};
    if (stg[0]) xo[0] = (stg[2] ? stg[2]->in_locs : fs ? fs->in_locs : 0) & ~stg[0]->out_locs;
    if (stg[2]) xo[2] = (fs ? fs->in_locs : 0) & ~stg[2]->out_locs;
    // (the alpha test's reference: a push constant of the fragment shader's variant, gfx_vk_shader.c spv_alpha_test; prepare_draw pushes it)
    uint32_t vi_flip = vi_flip_of(k, stg[0]);
    for (int i = 0; i < 3; i++) if (stg[i] && !(i == 1 && k->raster_discard)) {
        VkShaderModule mod = i == 2 && k->gs_split ? stg[i]->gs_mod[k->gs_split - 1] :
            vk_shader_module(stg[i], k->um[i], k->im[i], k->alt && !k->um[i] && !k->im[i] && !xo[i] ? 1u << 31 : xo[i], i == 1 ? (k->fdz ? 1 : 0) | (k->dual ? 2 : 0) | (int)(k->atest & 15) << 4 | (int)int_out_bits(k, stg[1]) : 0, i == 0 ? vi_flip : 0);
        ss[ns++] = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, vk_sg_pin(fl[i], NULL), 0, fl[i], mod, "main", NULL};   // (pNext: DM_VK_SUBGROUP_PIN)
    }
    int flat = (int)k->debug_mode;
    if (!fs) flat = 0;
    if (flat & 1) {   // DM_VK_X_FLAT_VS: the fragment stage becomes flatdbg.frag
        static VkShaderModule fm; if (!fm) { VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof flatdbg_frag_spv, flatdbg_frag_spv}; VKCHK(vkCreateShaderModule(V.dev, &mi, NULL, &fm)); }
        for (uint32_t i = 0; i < ns; i++) if (ss[i].stage == VK_SHADER_STAGE_FRAGMENT_BIT) ss[i].module = fm;
    }
    if (flat) { static int n; if (n++ < 12) VKLOG("debugging: vertex shader %016llx with fragment %016llx: mode %d (1 magenta, 2 no depth test, 4 no blending); %u colour targets, blending %#x (target 0: src %u dst %u op %u, alpha src %u dst %u op %u, write mask %#x), depth test %u write %u func %u",
        (unsigned long long)stg[0]->key, (unsigned long long)fs->key, flat, k->ncol, k->blend_en, k->bl[0].src, k->bl[0].dst, k->bl[0].eq, k->bl[0].srca, k->bl[0].dsta, k->bl[0].eqa, k->wmask[0], k->dt, k->dw, k->df); }
    VkVertexInputAttributeDescription ad[16]; VkVertexInputBindingDescription bd[17]; VkVertexInputBindingDivisorDescriptionEXT dd[17]; uint32_t ndd = 0;
    for (uint32_t i = 0; i < k->nattr; i++) ad[i] = (VkVertexInputAttributeDescription){k->attr[i].loc, k->attr[i].binding, (VkFormat)k->attr[i].fmt, k->attr[i].off};
    for (uint32_t i = 0; i < k->nbind; i++) { bd[i] = (VkVertexInputBindingDescription){k->bind[i].binding, k->bind[i].stride, (VkVertexInputRate)k->bind[i].rate};
        if (k->bind[i].rate && k->bind[i].divisor > 1) {
            if (V.has_divisor) dd[ndd++] = (VkVertexInputBindingDivisorDescriptionEXT){k->bind[i].binding, k->bind[i].divisor};
            else VKRED_ONCE(176, "vertex stream stepping per %u instances on a device without vertexAttributeInstanceRateDivisor: read per instance", k->bind[i].divisor); } }
    VkPipelineVertexInputDivisorStateCreateInfoEXT dvs = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT, NULL, ndd, dd};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, ndd ? &dvs : NULL, 0, k->nbind, bd, k->nattr, ad};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, (VkPrimitiveTopology)k->topo, k->restart};
    VkPipelineViewportDepthClipControlCreateInfoEXT dcc = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT, NULL, k->clip_neg1};
    VkPipelineViewportStateCreateInfo vs = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, k->clip_neg1 && V.has_depth_clip_ctl ? &dcc : NULL, 0, 1, NULL, 1, NULL};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0, k->depth_clamp, k->raster_discard,
        k->mode == 0 ? VK_POLYGON_MODE_POINT : k->mode == 1 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL, k->cull, k->front ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE,
        k->bias_en, 0, 0, 0, 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, (VkSampleCountFlagBits)k->samples, 0, 0, NULL, k->a2c, 0};
    if (flat & 8) ms.alphaToCoverageEnable = 0;
    if (flat & 16) rs.depthBiasEnable = 0;
    VkStencilOpState sf = {sop(k->sfail[0]), sop(k->szpass[0]), sop(k->szfail[0]), cmpop(k->sf[0]), 0xff, 0xff, 0}, sb = {sop(k->sfail[1]), sop(k->szpass[1]), sop(k->szfail[1]), cmpop(k->sf[1]), 0xff, 0xff, 0};
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, NULL, 0, k->dt, k->dt && k->dw, cmpop(k->df), 0, k->st, sf, sb, 0, 1};
    if (flat & 2) { ds.depthTestEnable = ds.depthWriteEnable = ds.stencilTestEnable = 0; }
    if (flat & 32) ds.depthTestEnable = ds.depthWriteEnable = 0;
    if (flat & 64) ds.stencilTestEnable = 0;
    VkPipelineColorBlendAttachmentState cba[8];
    for (uint32_t i = 0; i < k->ncol; i++) cba[i] = (VkPipelineColorBlendAttachmentState){(k->blend_en >> i) & 1, bfac(k->bl[i].src), bfac(k->bl[i].dst), bop(k->bl[i].eq),
        bfac(k->bl[i].srca), bfac(k->bl[i].dsta), bop(k->bl[i].eqa), k->wmask[i]};
    if (flat & 4) for (uint32_t i = 0; i < k->ncol; i++) { cba[i].blendEnable = 0; cba[i].colorWriteMask = 0xf; }
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, k->logic_en, (VkLogicOp)k->logic_op, k->ncol, cba, {0, 0, 0, 0}};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                            VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_LINE_WIDTH};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, sizeof dyn / sizeof *dyn, dyn};
    VkFormat cf[8]; for (uint32_t i = 0; i < k->ncol; i++) cf[i] = (VkFormat)k->cfmt[i];
    VkFormat df = (VkFormat)k->dfmt;
    int has_stencil = df == VK_FORMAT_D24_UNORM_S8_UINT || df == VK_FORMAT_D32_SFLOAT_S8_UINT || df == VK_FORMAT_S8_UINT;
    VkPipelineRenderingCreateInfo rci = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, k->ncol, cf, df == VK_FORMAT_S8_UINT ? VK_FORMAT_UNDEFINED : df, has_stencil ? df : VK_FORMAT_UNDEFINED};
    VkRenderPass prp = rp_on() ? rp_compat(k->ncol, k->cfmt, k->dfmt, k->samples) : VK_NULL_HANDLE;   // with render passes, pipelines are made against a compatible one
    if (!prp && !V.has_dynrender) { build_end(bs); return (VkPipeline)1; }   // no compatible render pass and no dynamic rendering: no valid pipeline (pass_commit skips such passes)
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, prp ? NULL : &rci, 0, ns, ss, &vi, &ia, NULL, &vs, &rs, &ms, &ds, &cb, &dy, layout, prp, 0, VK_NULL_HANDLE, -1};
    if (bs >= 0) g_build[bs].phase = 2;
    VkResult r = PSERIAL(vkCreateGraphicsPipelines(V.dev, V.pcache, 1, &ci, NULL, &p));
    vk_pipeline_cache_changed();
    build_end(bs);
    static uint64_t n; uint64_t c = __atomic_add_fetch(&n, 1, __ATOMIC_RELAXED);
    if (r) { LOG("[RED] [vk] graphics pipeline failed (%d): shaders %016llx %016llx %016llx", r, (unsigned long long)k->key[0], (unsigned long long)k->key[1], (unsigned long long)k->key[2]); p = (VkPipeline)1; }
    else if (c % 200 == 0) VKLOG("%llu graphics pipelines", (unsigned long long)c);
    __atomic_add_fetch(&g_vkst.pipes_built, 1, __ATOMIC_RELAXED);
    return p;   // (VkPipeline)1 = failed
}
#include "gfx_vk_gpl.inc"

