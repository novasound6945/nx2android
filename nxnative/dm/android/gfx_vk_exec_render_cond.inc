// ---------------- conditional rendering (profile render_cond) ----------------
// As nvn_mtl.m (OP_RENDER_COND): SetRenderEnableConditional draws while the 64-bit counters of the reports at addr and addr + 16 are equal / differ, until SetRenderEnable(true).
// Evaluated on the CPU at replay from the records in memory: a report replayed earlier in this submission is not written yet (that happens when the GPU completes), so a timestamp
// one counts with its known counter 0 (vk_sub_complete writes 0 there under the key) and any other one is unknown, which keeps drawing. Inside a block evaluated false, draws and
// clears are dropped. Mario vs. Donkey Kong (gsys, as Tears of the Kingdom and Animal Crossing) clears its HDR scene target under such a condition after lighting it: drawn
// anyway, its levels were black under the HUD
static int g_rc_skip;   // a render-enable block evaluated false is active
#define RC_PEND 256
static struct { uint64_t addr; int kind; } g_rc_pend[RC_PEND]; static int g_rc_npend;   // reports replayed in this submission: kind 1 = timestamp (counter 0), 2 = another counter (unknown),
                                                                                       // 3 = samples passed, counted (DM_VK_OCCLUSION: its address's entry decides)
// Addresses ever written by a non-timestamp report: conditions on them keep drawing. A set that grows: the fixed 4096-slot table this was filled up in The Legend of
// Zelda: Ocarina of Time (its reports move through their buffer as the camera turns), after which every report and every condition walked all 4096 slots (0.9 s of the
// replay thread's 4.4 s over 8 s of camera turns, Odin 3) and new addresses were no longer recorded. Replay thread only
static nvn_hmap g_rc_occ = {.klen = 8};
static void rc_occ_add(uint64_t a) {
    if (nvn_hm_get(&g_rc_occ, &a)) return;
    nvn_hm_put(&g_rc_occ, &a, (void *)1);
    if (g_rc_occ.n >= 4096 && !(g_rc_occ.n & (g_rc_occ.n - 1))) VKLOG("render conditions: %u addresses written by occlusion reports so far", g_rc_occ.n);
}
static int rc_occ_has(uint64_t a) { return nvn_hm_get(&g_rc_occ, &a) != NULL; }

// ---------------- occlusion queries (DM_VK_OCCLUSION) ----------------
// NVN's samples-passed counter is counted with Vulkan occlusion queries (vk_occ.h: one query per stretch of draws in a pass, summed in command order by the completion
// thread, gfx_vk_core.c), and a report writes the real count where it used to write 1 << 20, when its submission completes. A condition on such a report is judged on the
// CPU at replay from its address's last completed result (occ_ent): exact when the last report recorded there has completed; when it has not (replayed earlier in
// this submission, or in one the GPU has not finished), DM_VK_OCCLUSION=2 (the default) uses the result before it, a frame or more late, if it was recorded at most
// DM_VK_OCCLUSION_AGE presents ago (6), and DM_VK_OCCLUSION=1 draws. DM_VK_OCCLUSION=0: as before (1 << 20, no queries, such conditions draw). Read at each submission.
// Only for games with the profile key render_cond (the only ones that record conditions): other games kept getting 1 << 20 and would now see real counts, which
// may make them cull on the CPU; DM_VK_OCCLUSION_ANY=1 gives them the real counts too (tests). Without a judgement (pending, too old, lost draws in between) a block draws.
// Counting begins at a samples-passed reset and stops after 240 presents with no reset or report (each pass with draws while counting holds a query)
static int g_occ_mode, g_occ_age = 6;
static occ_rec g_occ = {0, -1, 0, 0, 0}; static uint64_t g_occ_used;   // g_occ_used: the present of the last reset or report while counting
enum { OCC_NENT = 4096 };
occ_ent g_occ_ent[OCC_NENT];   // report addresses (open addressing; the replay thread inserts, gfx_vk_core.c's completion thread writes results by index)
static int g_occ_nent;
int vk_oq_begin(void); void vk_oq_end(int q); void vk_oq_post(int kind, int q); void vk_oq_report(uint64_t addr, int ent, uint32_t seq, uint64_t frame); extern int g_oq_want; extern uint64_t g_occ_done[6];
static occ_ent *occ_find(uint64_t a, int add, int *idx) {
    for (uint32_t i = (uint32_t)(a >> 4) & (OCC_NENT - 1), k = 0; k < OCC_NENT; k++, i = (i + 1) & (OCC_NENT - 1)) {
        if (g_occ_ent[i].addr == a) { if (idx) *idx = (int)i; return &g_occ_ent[i]; }
        if (!g_occ_ent[i].addr) { if (!add || g_occ_nent >= OCC_NENT * 3 / 4) return NULL; g_occ_ent[i].addr = a; g_occ_nent++; if (idx) *idx = (int)i; return &g_occ_ent[i]; }
    }
    return NULL;
}
static int occ_io_begin(void *u) { (void)u; return vk_oq_begin(); }
static void occ_io_end(void *u, int q) { (void)u; vk_oq_end(q); }
static void occ_io_post(void *u, int kind, int q) { (void)u; vk_oq_post(kind, q); }
static const occ_io g_occ_io = {occ_io_begin, occ_io_end, occ_io_post, NULL};
static void occ_draw(int made) { if (g_occ.counting) occ_rec_draw(&g_occ, &g_occ_io, made && S.pass_open == 1); }   // every draw at its place, made or not

static void rc_note(uint64_t a, int kind) {
    if (kind == 2) rc_occ_add(a);
    if (kind != 3 && g_occ_mode) { occ_ent *e = occ_find(a, 0, NULL); if (e) e->dead = 1; }   // another type's report over a samples-passed one: its entry no longer says
    for (int i = 0; i < g_rc_npend; i++) if (g_rc_pend[i].addr == a) { g_rc_pend[i].kind = kind; return; }
    if (kind == 2) return;   // (g_rc_occ holds it. Appended here too, a submission's occlusion reports filled the table and a timestamp report after them was not noted, so a
                             // condition on it read memory, not yet written, instead of its known counter 0. Ocarina of Time (DeathKhan/nx2android) clears its 768x432 target under
                             // such a condition at each bind and reports once: the later clears wiped the scene copy, opaque water)
    if (g_rc_npend < RC_PEND) { g_rc_pend[g_rc_npend].addr = a; g_rc_pend[g_rc_npend++].kind = kind; }
    else { static int said; if (!said) { said = 1; VKLOG("render conditions: more than %d timestamp reports in a submission, the rest read memory", RC_PEND); } }
}
// A condition's side at replay time (vk_occ.h occ_side); *from: where a samples-passed result came from (OCC_FROM_*, -1 not one), *age its presents
static occ_side rc_side(uint64_t a, int *from, uint64_t *age) {
    occ_side s = {OCC_S_NONE, 0, {0}}; *from = -1; *age = 0;
    if (rc_occ_has(a)) return s;
    int k = 0; for (int i = g_rc_npend - 1; i >= 0; i--) if (g_rc_pend[i].addr == a) { k = g_rc_pend[i].kind; break; }
    if (k == 1) { s.kind = OCC_S_MEM; return s; }   // a timestamp report: its counter 0
    if (k == 2) return s;
    occ_ent *e = g_occ_mode ? occ_find(a, 0, NULL) : NULL;
    if (e && !e->dead) {
        uint32_t ds = 0; occ_val v; uint64_t fr = 0; int has = occ_ent_get(e, &ds, &v, &fr);
        *from = occ_pick(e->rec_seq, has, ds, fr, g_tex_frame, g_occ_mode == 1, (uint32_t)g_occ_age);
        if (*from == OCC_FROM_EXACT || *from == OCC_FROM_LAG) { s.kind = OCC_S_REP; s.r = v; *age = g_tex_frame > fr ? g_tex_frame - fr : 0; }
        return s;
    }
    if (k == 3) return s;
    s.kind = OCC_S_MEM; s.mem = *(const volatile uint64_t *)(uintptr_t)a; return s;
}
// Diagnostics (no effect on the replay): which conditions a game uses, logged every 600 presents while it evaluates any. Counts since the last line: conditions by mode, known /
// unknown at replay, judged false (skipped); SetRenderEnable(false); the counter reports by type (NVN_COUNTER_TYPE_*: TIMESTAMP 0 / TIMESTAMP_TOP 0x10, SAMPLES_PASSED 1, any other).
// With DM_VK_OCCLUSION a second line: the samples-passed reports and their results, the conditions on them by where the result came from (completed / the one before
// and how late / drawn: pending, too old, not comparable), what false blocks skipped, and the queries
static struct { uint64_t cond[2], known, unknown, unk_occ, skip, off, rep_ts, rep_sp, rep_other, last; uint32_t other_types;
                uint64_t oc, oc_exact, oc_lag, oc_pend, oc_old, oc_unk, oc_skip, lag_age[4], skip_draws, skip_clears, rep_occ, rep_idle, rep_noent; } g_rcs;
static void rc_count_report(uint64_t t) {
    if (t == 0 || t == 0x10) g_rcs.rep_ts++; else if (t == 1) g_rcs.rep_sp++; else { g_rcs.rep_other++; if (t < 32) g_rcs.other_types |= 1u << t; }
}
static void rc_stats_log(void) {
    if (g_tex_frame < g_rcs.last + 600) return;
    if (g_rcs.cond[0] + g_rcs.cond[1]) {
        VKLOG("render_cond: %llu conditions (draw if equal %llu / if different %llu) in %llu presents: %llu known, %llu unknown (%llu on a non-timestamp report's address), %llu judged false (skipped); "
              "%llu SetRenderEnable(false); counter reports: %llu timestamp, %llu SAMPLES_PASSED, %llu other (types mask %#x)",
              (unsigned long long)(g_rcs.cond[0] + g_rcs.cond[1]), (unsigned long long)g_rcs.cond[0], (unsigned long long)g_rcs.cond[1], (unsigned long long)(g_tex_frame - g_rcs.last),
              (unsigned long long)g_rcs.known, (unsigned long long)g_rcs.unknown, (unsigned long long)g_rcs.unk_occ, (unsigned long long)g_rcs.skip, (unsigned long long)g_rcs.off,
              (unsigned long long)g_rcs.rep_ts, (unsigned long long)g_rcs.rep_sp, (unsigned long long)g_rcs.rep_other, g_rcs.other_types);
        static uint64_t d0[6], f0, l0, n0;   // the completion thread's and the queries' counts at the last line
        uint64_t d[6]; for (int i = 0; i < 6; i++) d[i] = __atomic_load_n(&g_occ_done[i], __ATOMIC_RELAXED);
        if (g_occ_mode || g_rcs.rep_occ || d[0] != d0[0])
            VKLOG("render_cond occlusion (DM_VK_OCCLUSION=%d, age limit %d, queries %s): %llu samples-passed reports recorded (%llu while not counting, %llu with no entry left); completed %llu: "
                  "%llu zero, %llu not zero, %llu known up to lost draws, %llu unknown; conditions on them %llu: %llu on completed results, %llu on the result before (%llu / %llu / %llu / %llu "
                  "1 / 2 / 3 / 4+ presents late), drawn: %llu pending, %llu too old, %llu not comparable; %llu judged false; false blocks skipped %llu draws, %llu clears; "
                  "%llu query fragments, %llu draws not counted (%llu with no query left), %llu query results missing",
                  g_occ_mode, g_occ_age, V.has_oq_precise ? "precise" : "not precise", (unsigned long long)g_rcs.rep_occ, (unsigned long long)g_rcs.rep_idle, (unsigned long long)g_rcs.rep_noent,
                  (unsigned long long)(d[0] - d0[0]), (unsigned long long)(d[1] - d0[1]), (unsigned long long)(d[2] - d0[2]), (unsigned long long)(d[5] - d0[5]), (unsigned long long)(d[3] - d0[3]),
                  (unsigned long long)g_rcs.oc, (unsigned long long)g_rcs.oc_exact, (unsigned long long)g_rcs.oc_lag, (unsigned long long)g_rcs.lag_age[0], (unsigned long long)g_rcs.lag_age[1],
                  (unsigned long long)g_rcs.lag_age[2], (unsigned long long)g_rcs.lag_age[3], (unsigned long long)g_rcs.oc_pend, (unsigned long long)g_rcs.oc_old, (unsigned long long)g_rcs.oc_unk,
                  (unsigned long long)g_rcs.oc_skip, (unsigned long long)g_rcs.skip_draws, (unsigned long long)g_rcs.skip_clears, (unsigned long long)(g_occ.frags - f0),
                  (unsigned long long)(g_occ.lost - l0), (unsigned long long)(g_occ.nopool - n0), (unsigned long long)(d[4] - d0[4]));
        memcpy(d0, d, sizeof d0); f0 = g_occ.frags; l0 = g_occ.lost; n0 = g_occ.nopool;
        memset(&g_rcs, 0, sizeof g_rcs);
    }
    g_rcs.last = g_tex_frame;
}
static void render_cond(const p_render_cond *rc) {
    rc_stats_log();
    if (rc->mode >= 2) { g_rc_skip = rc->mode == 3; g_rcs.off += rc->mode == 3; return; }
    int fa, fb; uint64_t aa, ab; occ_side x = rc_side(rc->addr, &fa, &aa), y = rc_side(rc->addr + 16, &fb, &ab);
    int eq = 0, known = occ_eq(&x, &y, &eq);
    g_rc_skip = known && (rc->mode ? eq : !eq);
    g_rcs.cond[rc->mode != 0]++; if (known) g_rcs.known++; else { g_rcs.unknown++; g_rcs.unk_occ += rc_occ_has(rc->addr) || rc_occ_has(rc->addr + 16); }
    g_rcs.skip += g_rc_skip;
    if (fa >= 0 || fb >= 0) {   // on a samples-passed result: from where (the worse side)
        int f = fa == OCC_FROM_PENDING || fb == OCC_FROM_PENDING ? OCC_FROM_PENDING : fa == OCC_FROM_OLD || fb == OCC_FROM_OLD ? OCC_FROM_OLD : fa == OCC_FROM_LAG || fb == OCC_FROM_LAG ? OCC_FROM_LAG : OCC_FROM_EXACT;
        g_rcs.oc++; g_rcs.oc_skip += g_rc_skip;
        if (f == OCC_FROM_PENDING) g_rcs.oc_pend++; else if (f == OCC_FROM_OLD) g_rcs.oc_old++; else if (!known) g_rcs.oc_unk++;
        else if (f == OCC_FROM_EXACT) g_rcs.oc_exact++; else { uint64_t age = aa > ab ? aa : ab; g_rcs.oc_lag++; g_rcs.lag_age[age >= 4 ? 3 : age ? age - 1 : 0]++; }
    }
    if (g_x_tracepass == -2) { int k0 = -1, k1 = -1; for (int i = 0; i < g_rc_npend; i++) { if (g_rc_pend[i].addr == rc->addr) k0 = g_rc_pend[i].kind; if (g_rc_pend[i].addr == rc->addr + 16) k1 = g_rc_pend[i].kind; }
        VKLOG("--- render condition mode %d at %#llx: %s (%s), memory %llu / %llu, occlusion-written %d / %d, pending kind %d / %d", rc->mode, (unsigned long long)rc->addr, g_rc_skip ? "skip" : "draw", known ? "known" : "unknown",
            (unsigned long long)*(const volatile uint64_t *)(uintptr_t)rc->addr, (unsigned long long)*(const volatile uint64_t *)(uintptr_t)(rc->addr + 16), rc_occ_has(rc->addr), rc_occ_has(rc->addr + 16), k0, k1); }
}
// a samples-passed report while DM_VK_OCCLUSION is on: the stretch so far ends here; its address's entry (render_cond games) counts it as recorded, not completed
static void occ_report(uint64_t a) {
    occ_rec_close(&g_occ, &g_occ_io);
    int idx = -1; uint32_t seq = 0; occ_ent *e = DM_GAME_RENDER_COND ? occ_find(a, 1, &idx) : NULL;
    if (e) { seq = ++e->rec_seq; if (!seq) seq = e->rec_seq = 1; e->dead = 0; }   // (0: none completed)
    else if (DM_GAME_RENDER_COND) { g_rcs.rep_noent++; VKRED_ONCE(120, "occlusion queries: more report addresses than entries (%d): conditions on the rest are drawn", OCC_NENT * 3 / 4); }
    if (DM_GAME_RENDER_COND) { rc_note(a, e ? 3 : 2); rc_count_report(NVN_COUNTER_TYPE_SAMPLES_PASSED); }
    g_rcs.rep_occ++; if (!g_occ.counting) g_rcs.rep_idle++; else g_occ_used = g_tex_frame;
    vk_oq_report(a, idx, seq, g_tex_frame);
}
static void occ_live(void) {   // each submission (vk_exec_reset; no pass is open): DM_VK_OCCLUSION, DM_VK_OCCLUSION_AGE, DM_VK_OCCLUSION_ANY; counting idle
    const char *e = getenv("DM_VK_OCCLUSION"); int m = e && *e ? atoi(e) : 2; if (m < 0 || m > 2) m = 2;
    if (!DM_GAME_RENDER_COND && !((e = getenv("DM_VK_OCCLUSION_ANY")) && atoi(e))) m = 0;
    g_occ_age = (e = getenv("DM_VK_OCCLUSION_AGE")) && atoi(e) > 0 ? atoi(e) : 6;
    g_occ.q = -1;   // (the last submission's end closed its pass and query)
    if (m != g_occ_mode) { VKLOG("occlusion queries: DM_VK_OCCLUSION %d -> %d (%s)", g_occ_mode, m, m == 2 ? "real counts, conditions on the result before when pending" : m ? "real counts, conditions only on completed results" : "off: reports 1 << 20, conditions on them drawn");
        if (!m) occ_rec_stop(&g_occ, &g_occ_io); else if (!g_occ_mode) nvn_hm_clear(&g_rc_occ, NULL);   // (on again: the samples-passed addresses mode 0 made unknown, as the others, until reported again)
        g_occ_mode = m; }
    if (g_occ.counting && g_tex_frame > g_occ_used + 240) { occ_rec_stop(&g_occ, &g_occ_io); static int n; if (n++ < 4) VKLOG("occlusion queries: no samples-passed reset or report for 240 presents: counting stops until the next reset"); }
    __atomic_store_n(&g_oq_want, m != 0, __ATOMIC_RELAXED);
}

static void exec_begin(int continuing) {   // internal segments retain the current logical NVN and conditional state
    static int inited; if (!inited) { inited = 1; state_reset(); crash_add_note_hook(build_dump); prewarm_start(); compute_warm_start(); }
    if (!continuing) { g_rc_npend = 0; g_rc_skip = 0; }
    { void vk_tex_release_drain(void); vk_tex_release_drain(); }   // Finalized textures' CPU-written marks that no queued job needs any more (gfx_vk_tex.c)
    S.pass_open = 0; S.bound_pipe = VK_NULL_HANDLE; g_cb_draws = 0; g_ord_parts = 0; g_psplit_now = 0; g_set_epoch++; x_read(); occ_live();
    if (g_prof_on) { extern uint64_t g_pkg_ns, g_pkg_n; memset(g_prof_ns, 0, sizeof g_prof_ns); g_prof_t0 = tick(); g_prof_pkg0 = g_pkg_ns; g_prof_npkg0 = g_pkg_n; g_prof_pipes0 = g_vkst.pipes_built; }
    { extern volatile int g_vk_samp_log; if (g_vk_samp_log > 0) VKLOG("submission begins (present %llu, thread %d)", (unsigned long long)g_tex_frame, (int)gettid()); }
}

void vk_exec_reset(void) { exec_begin(0); }
void vk_exec_resume(void) { exec_begin(1); }

