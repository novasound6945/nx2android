// This fork's per-game settings.txt keys, front_android.c's part (included there once, before settings_load): the frame rate limit (limit=), the frame step's target
// (rate=) and the render scale's step (res=). The launcher's App settings change them before the game starts (datasync_android.c gopt_*), and settings_load reads them
// at the game's start (game_run), so a change there holds from that start on; the pause menu has no rows for them (it is upstream's). settings_save writes back what
// settings_load read, so the launcher's choices stay in the file.
//
// The Resolution: for a game with a render-scale stub whose profile gives the three steps (android.tsv: env DM_MENU_RES=<low>:<auto>:<high>, e.g. 0.5:1.0:0.9). The
// steps are render scales the host hands the stub (DM_RENDER_SCALE, read again every 30 presents: hle_nvn.c); what a stub makes of a scale is its game's
static char g_res_scales[3][16];
static int res_steps(void) {   // the profile's three scales, read once (env.txt is loaded before the settings)
    static int ok = -1;
    if (ok < 0) { const char *e = getenv("DM_MENU_RES"); float f[3]; ok = e && sscanf(e, "%f:%f:%f", &f[0], &f[1], &f[2]) == 3 && f[0] > 0 && f[1] > 0 && f[2] > 0;
        for (int i = 0; ok && i < 3; i++) snprintf(g_res_scales[i], sizeof g_res_scales[i], "%g", (double)f[i]); }
    return ok;
}
static void res_apply(int step) { if (res_steps()) setenv("DM_RENDER_SCALE", g_res_scales[step < 0 ? 0 : step > 2 ? 2 : step], 1); }
static _Atomic int g_set_res = -1;      // the Resolution's step the player chose: 0 low, 1 auto, 2 high (settings.txt res=); -1: env.txt's DM_RENDER_SCALE
static _Atomic int g_set_rate = -1;     // the frame step's target the player chose (30 or 60; settings.txt rate=, only with the profile's DM_MENU_RATE); -1: the profile's or env.txt's
// The frame rate limit (settings.txt limit=: 0 Auto, 60, 40, 30), held through DM_FPS_PIN, which perf_android.c fps_pin reads every 30 frames: 60, 40 or 30 hold that step
// (40 asks the display for 120 Hz, three vsyncs a frame), any other value lets the adaptive rate decide. Auto gives back DM_FPS_PIN as the env files set it at the start
// (env.txt, then a variant's env-4x3.txt: a profile's pin), or "auto" (no pin) when they set none. The variable is set from the start on, before the game's threads
#define GOPT_LIMITS 4
static const int LIMIT_STEPS[GOPT_LIMITS] = {0, 60, 40, 30};   // as the launcher steps through them (datasync_android.c GOPT_LIMIT)
static _Atomic int g_set_limit;   // 0 Auto, else the frame rate held
static char g_limit_env[32]; static int g_limit_env_set;   // DM_FPS_PIN as the env files set it at the start, and whether they set it
static int limit_step(int v) { for (int i = 0; i < GOPT_LIMITS; i++) if (LIMIT_STEPS[i] == v) return i; return -1; }   // v's index in LIMIT_STEPS, -1 a value there is not
static void limit_apply(int start) {   // start: game_run, before the game's threads (the env files' DM_FPS_PIN is kept for Auto); 0: a change while it runs
    int v = atomic_load(&g_set_limit); char n[8]; snprintf(n, sizeof n, "%d", v);
    if (start) { const char *e = getenv("DM_FPS_PIN"); g_limit_env_set = e != NULL; snprintf(g_limit_env, sizeof g_limit_env, "%s", e ? e : ""); }
    setenv("DM_FPS_PIN", v ? n : g_limit_env_set ? g_limit_env : "auto", 1);
    if (v && start) LOG("[menu] frame rate limit: %d fps (settings.txt limit=%d, over the env files' DM_FPS_PIN %s)", v, v, g_limit_env_set ? g_limit_env : "(none)");
    else if (v) LOG("[menu] frame rate limit: %d fps (DM_FPS_PIN=%d)", v, v);
    else if (g_limit_env_set) LOG("[menu] frame rate limit: auto (DM_FPS_PIN=%s, as the env files set it)", g_limit_env);
    else LOG("[menu] frame rate limit: auto (the adaptive rate: the env files set no DM_FPS_PIN)");
}
static int game_opt_parse(const char *line) {   // one settings.txt line (settings_load): 1 when it was one of these keys
    int v;
    if (sscanf(line, "limit=%d", &v) == 1) { if (limit_step(v) >= 0) atomic_store(&g_set_limit, v); else LOG("[RED] settings.txt: limit=%d is not 0 (auto), 60, 40 or 30: auto", v); }   // limit_apply
    else if (sscanf(line, "res=%d", &v) == 1) {   // over env.txt's: the loader reads it when the game starts
        if (v >= 0 && v <= 2 && res_steps()) { atomic_store(&g_set_res, v); res_apply(v); LOG("[menu] resolution: step %d (settings.txt res=%d: DM_RENDER_SCALE=%s)", v, v, g_res_scales[v]); }
    }
    else if (sscanf(line, "rate=%d", &v) == 1) {   // over env.txt's: the loader reads it when the game starts
        if (getenv("DM_MENU_RATE") && (v == 30 || v == 60)) { atomic_store(&g_set_rate, v); setenv("DM_FPS_TARGET", v == 60 ? "60" : "30", 1); LOG("[menu] frame rate: %d fps (settings.txt rate=%d)", v, v); }
    }
    else return 0;
    return 1;
}
static void game_opt_write(FILE *f) {   // (settings_save) limit= always; rate= and res= only once chosen: until then env.txt decides
    fprintf(f, "limit=%d\n", atomic_load(&g_set_limit));
    if (atomic_load(&g_set_rate) > 0) fprintf(f, "rate=%d\n", atomic_load(&g_set_rate));
    if (atomic_load(&g_set_res) >= 0) fprintf(f, "res=%d\n", atomic_load(&g_set_res));
}
