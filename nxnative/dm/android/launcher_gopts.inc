// The launcher's App settings: the game's own settings, changed before the game starts (datasync_android.c, included there once after lnr_t). They are settings.txt's
// limit=, rate= and res= in the data's root, which front_android.c (game_opts_android.inc) reads when the game starts (game_run, after the launcher's Start), so a change
// here holds from this start on; the game's settings_save writes back what it read, so these stay in the file. The pause menu has no rows for them (it is upstream's).
//  - the frame rate limit (every game): Auto, 60, 40, 30 fps (limit= 0, 60, 40, 30). Auto is the profile's DM_FPS_PIN (env.txt), or the adaptive rate without one
//  - the frame rate (a profile with DM_MENU_RATE=1): the profile's own, 30 or 60 fps (no rate=, rate=30, rate=60)
//  - the resolution (a profile with DM_MENU_RES=<low>:<auto>:<high>): Low, Auto, High (res= 0, 1, 2); before one is chosen, the step nearest env.txt's DM_RENDER_SCALE
// The launcher reads env.txt itself (the start may not have read it yet, and the host caches what it reads of the environment)
enum { GOPT_LIMIT, GOPT_RATE, GOPT_RES };
static const int GOPT_LIMITS[4] = {0, 60, 40, 30};
static void gopt_path(char *o, size_t n, const char *name) { const char *r = getenv("DM_ROOT"); if (r && r[0]) snprintf(o, n, "%s/%s", r, name); else if (S.root[0]) snprintf(o, n, "%s/%s", S.root, name); else o[0] = 0; }
static int gopt_val(const char *text, const char *key, char *out, size_t n) {   // the last "key=value" line of text: 1 when there is one (value without its line end)
    int found = 0; size_t k = strlen(key);
    for (const char *l = text; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL) {
        if (strncmp(l, key, k) || l[k] != '=') continue;
        const char *v = l + k + 1; size_t m = strcspn(v, "\r\n"); if (m >= n) m = n - 1;
        memcpy(out, v, m); out[m] = 0; found = 1;
    }
    return found;
}
static void gopt_load_locked(gopt_t *g) {   // (S.mu held) the rows and their values, from settings.txt and env.txt
    char p[700], v[64]; size_t n = 0; memset(g, 0, sizeof *g); g->loaded = 1;
    gopt_path(p, sizeof p, "env.txt"); char *env = p[0] ? ds_read(p, &n, 1 << 20) : NULL;
    gopt_path(p, sizeof p, "settings.txt"); char *set = p[0] ? ds_read(p, &n, 1 << 20) : NULL;
    float sc[3] = {0, 0, 0}, cur = -1;
    if (env && gopt_val(env, "DM_FPS_PIN", v, sizeof v)) snprintf(g->pin, sizeof g->pin, "%s", v);
    if (env && gopt_val(env, "DM_FPS_TARGET", v, sizeof v)) g->env_rate = atoi(v);
    g->rate_on = env && gopt_val(env, "DM_MENU_RATE", v, sizeof v) && atoi(v) > 0;
    g->res_on = env && gopt_val(env, "DM_MENU_RES", v, sizeof v) && sscanf(v, "%f:%f:%f", &sc[0], &sc[1], &sc[2]) == 3 && sc[0] > 0 && sc[1] > 0 && sc[2] > 0;
    if (env && gopt_val(env, "DM_RENDER_SCALE", v, sizeof v)) cur = strtof(v, NULL);
    g->limit = set && gopt_val(set, "limit", v, sizeof v) ? atoi(v) : 0; { int ok = 0; for (int i = 0; i < 4; i++) ok |= GOPT_LIMITS[i] == g->limit; if (!ok) g->limit = 0; }
    g->rate = set && gopt_val(set, "rate", v, sizeof v) ? atoi(v) : 0; if (g->rate != 30 && g->rate != 60) g->rate = 0;
    g->res = set && gopt_val(set, "res", v, sizeof v) ? atoi(v) : -1;
    if (g->res < 0 || g->res > 2) {   // (front_android.c's rule: the step nearest env.txt's DM_RENDER_SCALE, Auto without one)
        g->res = 1;
        if (cur > 0) { static const int order[3] = {1, 0, 2}; float best = 1e9f; for (int i = 0; i < 3; i++) { float d = fabsf(sc[order[i]] - cur); if (d < best) { best = d; g->res = order[i]; } } }
    }
    g->id[g->n++] = GOPT_LIMIT; if (g->rate_on) g->id[g->n++] = GOPT_RATE; if (g->res_on) g->id[g->n++] = GOPT_RES;
    free(env); free(set);
}
static int gopt_write(const char *key, int v) {   // settings.txt's key= set to v (v < 0: taken out), every other line kept; 0 when written
    char p[700]; size_t n = 0; gopt_path(p, sizeof p, "settings.txt"); if (!p[0]) return -1;
    char *old = ds_read(p, &n, 1 << 20); size_t k = strlen(key), cap = (old ? strlen(old) : 0) + 64, o = 0; char *t = malloc(cap); if (!t) { free(old); return -1; }
    for (const char *l = old; l && *l; ) {
        const char *e = strchr(l, '\n'); size_t m = e ? (size_t)(e - l) + 1 : strlen(l);
        if (!(strncmp(l, key, k) == 0 && l[k] == '=')) { memcpy(t + o, l, m); o += m; if (!e) t[o++] = '\n'; }
        l += m;
    }
    if (v >= 0) o += (size_t)snprintf(t + o, cap - o, "%s=%d\n", key, v);
    int rc = ds_write(p, t, o);
    if (rc) LOG("[RED] launcher: cannot write %s (errno %d)", p, errno);
    free(t); free(old); return rc;
}
static void gopt_step_locked(gopt_t *g, int row) {   // (S.mu held) A or a tap on the row: its next value, written at once
    if (row < 0 || row >= g->n) return;
    int id = g->id[row];
    if (id == GOPT_LIMIT) {
        int k = 0; for (int i = 0; i < 4; i++) if (GOPT_LIMITS[i] == g->limit) k = i;
        g->limit = GOPT_LIMITS[(k + 1) % 4]; gopt_write("limit", g->limit);
        LOG("launcher: frame rate limit %s (settings.txt limit=%d, from the game's start)", g->limit ? "held" : "auto", g->limit);
    } else if (id == GOPT_RATE) {
        g->rate = g->rate == 0 ? 30 : g->rate == 30 ? 60 : 0; gopt_write("rate", g->rate ? g->rate : -1);
        LOG("launcher: frame rate %d (settings.txt rate=%s, from the game's start)", g->rate, g->rate ? (g->rate == 60 ? "60" : "30") : "(none: the profile's)");
    } else if (id == GOPT_RES) {
        g->res = (g->res + 1) % 3; gopt_write("res", g->res);
        LOG("launcher: resolution step %d (settings.txt res=%d, from the game's start)", g->res, g->res);
    }
}
static void gopt_view(stv_apppage *p, const gopt_t *g) {   // the App page's rows
    int ko = korean(); p->ngame = g->loaded ? g->n : 0;
    snprintf(p->glabel, sizeof p->glabel, "%s", L("게임 · 시작할 때 적용돼요", "Game · applied as it starts"));
    for (int i = 0; i < p->ngame; i++) {
        char *nm = p->gname[i], *v = p->gval[i]; size_t nn = sizeof p->gname[i], vn = sizeof p->gval[i];
        if (g->id[i] == GOPT_LIMIT) {
            snprintf(nm, nn, "%s", L("프레임 제한", "Frame rate limit")); int pin = atoi(g->pin);
            if (g->limit) snprintf(v, vn, "%d fps", g->limit);
            else if (pin == 60 || pin == 40 || pin == 30) snprintf(v, vn, ko ? "자동 · %d fps" : "Auto · %d fps", pin);
            else snprintf(v, vn, "%s", L("자동", "Auto"));
        } else if (g->id[i] == GOPT_RATE) {
            snprintf(nm, nn, "%s", L("프레임 레이트", "Frame rate"));
            if (g->rate) snprintf(v, vn, "%d fps", g->rate);
            else if (g->env_rate == 30 || g->env_rate == 60) snprintf(v, vn, ko ? "기본 · %d fps" : "Default · %d fps", g->env_rate);
            else snprintf(v, vn, "%s", L("기본", "Default"));
        } else {
            static const char *const KO[3] = {"낮음", "자동", "높음"}, *const EN[3] = {"Low", "Auto", "High"};
            snprintf(nm, nn, "%s", L("해상도", "Resolution")); snprintf(v, vn, "%s", ko ? KO[g->res] : EN[g->res]);
        }
    }
}
