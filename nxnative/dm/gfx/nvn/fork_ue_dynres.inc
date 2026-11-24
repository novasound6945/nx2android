// This fork's: an Unreal Engine game's own dynamic resolution, steered by the host (DM_UE_DYNRES). Included by hle_nvn.c after engine_dyn_tick.
//
//   DM_UE_DYNRES=<main's build ID, 32 hex>:<min>,<max>,<throttle>:<lo>-<hi>      (start-up; a game's android.tsv env row)
//
// <min>, <max>, <throttle>: hex offsets in main of the value pointers of r.DynamicRes.MinScreenPercentage, r.DynamicRes.MaxScreenPercentage and
// r.DynamicRes.ThrottlingMaxScreenPercentage (a TAutoConsoleVariable's third word: it points to the value's two copies, the game thread's and the render
// thread's). <lo>-<hi>: the screen percentages the host moves between, in steps of 5.
//
// FINAL FANTASY RESONANCE (Unreal Engine 5.6.1) runs the engine's dynamic resolution between 60% and 80% of its 1728x972 picture, but in the field it sets the
// minimum to 80 as well, and the engine's heuristic never lowered the size on this host, whatever its time budget (16.7 ms, the GPU at 18.4): its heaviest field
// stayed at 1384x780 and 54 fps on the Retroid Pocket Nova, and held 60 at 1040x584. So the host decides: every 30 presents the 90th percentile of the frames'
// GPU time against the frame time; over 92% of it the percentage drops 5, under 82% for four such windows in a row it rises 5, but not within 600 presents of a
// drop (there 60% ran the GPU 14.0 ms a frame and 65% 15.1-15.5: without the pause it would go up and down every two seconds). A rise taken back within 900
// presents doubles that pause, up to 7200 presents (the field's far end read 12.8 ms at 70% and 15.8 at 75%, and went between them every 10 s); a rise that
// holds 1800 presents sets it back to 600. The value is written as the
// minimum and as the throttling maximum, which bounds the heuristic without changing the size the scene's targets are allocated at (the maximum does that, and
// is left to the game: while it loads it sets 60, and the host's value never passes the game's maximum).
static struct { uint8_t *at[3]; float lo, hi, cur; int on; } g_udr;
static void ue_dynres_init(void) {
    const char *e = getenv("DM_UE_DYNRES"); if (!e || !*e) return;
    const char *colon = strchr(e, ':'), *build = dm_main_build(); module_t *m = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) m = &g_mod[i];
    if (!m || !colon || colon - e != 32 || strncmp(e, build, 32)) { LOG("[RED] [nvn] DM_UE_DYNRES is for main %.32s, this main is %s: off", e, build); return; }
    unsigned long long o[3]; float lo, hi;
    if (sscanf(colon + 1, "%llx,%llx,%llx:%f-%f", &o[0], &o[1], &o[2], &lo, &hi) != 5 || !(lo >= 10 && lo <= hi && hi <= 100)) { LOG("[RED] [nvn] DM_UE_DYNRES=%s: not <build>:<min>,<max>,<throttle>:<lo>-<hi>: off", e); return; }
    for (int i = 0; i < 3; i++) { if (o[i] + 8 > m->size) { LOG("[RED] [nvn] DM_UE_DYNRES: %#llx is outside main: off", o[i]); return; } g_udr.at[i] = m->base + o[i]; }
    g_udr.lo = lo; g_udr.hi = hi; g_udr.cur = hi; g_udr.on = 1;
    LOG("[nvn] the engine's dynamic resolution is steered by the host between %g%% and %g%% (DM_UE_DYNRES)", (double)lo, (double)hi);
}
static void ue_dynres_tick(uint64_t presents) {
    static int init; if (!init) { init = 1; ue_dynres_init(); }
    if (!g_udr.on) return;
    float *v[3]; for (int i = 0; i < 3; i++) { uint8_t *p; memcpy(&p, g_udr.at[i], sizeof p); if (!p) return; v[i] = (float *)p; }   // (the variables are registered when the engine starts)
    static double win[30]; static int n, under; static uint64_t dropped_at, raised_at, hold = 600;
    double g = -1; for (uint64_t f = g_frame; f-- > 0 && g_frame - f <= 8;) { g = mtl_gpu_ms(f); if (g >= 0) break; }
    if (g >= 0) win[n++] = g;
    if (n == 30) { n = 0;
        double s[30]; memcpy(s, win, sizeof s);
        for (int i = 1; i < 30; i++) { double x = s[i]; int j = i; while (j && s[j - 1] > x) { s[j] = s[j - 1]; j--; } s[j] = x; }
        int fps = dm_pacing_ceiling() == 30 ? 30 : nvn_game_fps(); double p90 = s[26], period = 1000.0 / (fps > 0 ? fps : 60); float was = g_udr.cur;
        if (raised_at && presents - raised_at >= 1800) { raised_at = 0; hold = 600; }   // the last rise held
        if (p90 > 0.92 * period) { under = 0;
            if (g_udr.cur > g_udr.lo) { g_udr.cur = g_udr.cur - 5 > g_udr.lo ? g_udr.cur - 5 : g_udr.lo; dropped_at = presents;
                if (raised_at && presents - raised_at < 900) hold = hold * 2 < 7200 ? hold * 2 : 7200;   // the rise was too much here
                raised_at = 0; } }
        else if (p90 < 0.82 * period) { if (++under >= 4 && (!dropped_at || presents - dropped_at >= hold)) { under = 0; if (g_udr.cur < g_udr.hi) { g_udr.cur = g_udr.cur + 5 < g_udr.hi ? g_udr.cur + 5 : g_udr.hi; raised_at = presents; } } }
        else under = 0;
        if (g_udr.cur != was) { static int nlog; if (nlog++ < 200) LOG("[nvn] engine dynamic resolution: %g%% → %g%% at present %llu (GPU p90 %.1f ms of %.1f)", (double)was, (double)g_udr.cur, (unsigned long long)presents, p90, period); }
    }
    float gmax = v[1][0], t = gmax >= 10 && gmax <= 100 && gmax < g_udr.cur ? gmax : g_udr.cur;   // never past the game's own maximum
    for (int k = 0; k < 2; k++) { if (v[0][k] != t) v[0][k] = t; if (v[2][k] != t) v[2][k] = t; }
}
