// This fork's: bytes written into a loaded module's image before any guest code runs (DM_GUEST_POKE), to turn on a switch of the game's own that its Switch
// build leaves off. Called from main.c once the modules are loaded and relocated, beside dm_ue_malloc_install.
//
//   DM_GUEST_POKE=<main's build ID, 32 hex>:<module>+<hex offset>[><hex offset>]=<hex bytes>[,...]     (start-up; a game's android.tsv env row)
//
// With ><offset> the place is not in the module: the module's word at +<hex offset> is a pointer the game sets when it starts (an object it allocates), and the
// bytes go <offset> past where it points, once it points somewhere. Unreal Engine's console variables of the TAutoConsoleVariable kind keep their value so:
// the static object's third word (+0x10) points to the two copies of the value (the game thread's and the render thread's).
//
// The build ID must be the running main's, so a row written for one build never lands in another. A place the game's own start-up writes over afterwards (a
// static constructor, an init function) is written again, and the log says so: the places are looked at every 250 ms for the first 60 s.
//
// FINAL FANTASY RESONANCE (Unreal Engine 5.6.1, main b7f965b9d7227db8e29f04e84ea7331b): the engine's PSO precaching is compiled in and r.PSOPrecaching is 1 (the
// variable at main+0x779c65c), but the nine inlined IsPSOPrecachingEnabled checks (main+0x147a360, +0x147d33c, ...) also read the byte at main+0x8adade2, the
// RHI's "PSO precaching supported" flag, which no code of this build was found to write: the NVN RHI does not declare it, so the engine makes each pipeline's program
// only when the first draw needs it (measured: half of them within 250 ms of that draw). With the byte set the engine asks for a component's pipelines when the
// component is loaded, as on the platforms that compile pipelines slowly, and the host makes their parts then (gfx_vk_gpl.inc vk_gpl_announce).
#include "dm.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { NPOKE = 8, POKE_MAX = 16 };
static struct { uint8_t *at; uint8_t b[POKE_MAX]; int n, deref, said; uint64_t off2; char what[64]; } g_poke[NPOKE]; static int g_npoke;
static uint8_t *poke_at(int i) {   // where poke i's bytes go now: NULL while its pointer is not set yet
    if (!g_poke[i].deref) return g_poke[i].at;
    uint8_t *p; memcpy(&p, g_poke[i].at, sizeof p); return p ? p + g_poke[i].off2 : NULL;
}

static void *poke_watch(void *arg) {
    (void)arg; int said = 0;
    for (int t = 0;; t++) { usleep(t < 240 ? 250000 : 500000);   // (as long as the game runs: an engine sets some of these again at every level load)
        for (int i = 0; i < g_npoke; i++) { uint8_t *at = poke_at(i); if (!at || !memcmp(at, g_poke[i].b, (size_t)g_poke[i].n)) continue;
            int first = g_poke[i].deref && !g_poke[i].said; g_poke[i].said = 1;
            char was[POKE_MAX * 2 + 1] = ""; for (int k = 0; k < g_poke[i].n; k++) snprintf(was + k * 2, 3, "%02x", at[k]);
            double sec = t < 240 ? (t + 1) * 0.25 : 60 + (t - 239) * 0.5;
            memcpy(at, g_poke[i].b, (size_t)g_poke[i].n);
            if (first) LOG("guest poke: %s: its pointer is set %.1f s in: it held %s", g_poke[i].what, sec, was);
            else if (said++ < 24) LOG("guest poke: %s was written over by the game %.1f s in (it held %s): written again", g_poke[i].what, sec, was); } }
    return NULL;
}
// DM_GUEST_PEEK=<main's build ID>:<module>+<hex offset>[><hex offset>]?<bytes>[,...] (start-up, a diagnosis): the same places, only read. Each place's bytes are
// logged when first readable and whenever they have changed (looked at every 250 ms, up to 40 lines a place): what the game sets a switch to, and when.
enum { NPEEK = 24 };
static struct { uint8_t *at; int n, deref, have, said; uint64_t off2; uint8_t last[POKE_MAX]; char what[64]; } g_peek[NPEEK]; static int g_npeek;
static void *peek_watch(void *arg) {
    (void)arg;
    for (int t = 0;; t++) { usleep(250000);
        for (int i = 0; i < g_npeek; i++) { uint8_t *at = g_peek[i].at; if (g_peek[i].deref) { uint8_t *p; memcpy(&p, at, sizeof p); at = p ? p + g_peek[i].off2 : NULL; }
            if (!at || (g_peek[i].have && !memcmp(at, g_peek[i].last, (size_t)g_peek[i].n)) || g_peek[i].said >= 40) continue;
            memcpy(g_peek[i].last, at, (size_t)g_peek[i].n); g_peek[i].have = 1; g_peek[i].said++;
            char hx[POKE_MAX * 2 + 1] = ""; for (int k = 0; k < g_peek[i].n; k++) snprintf(hx + k * 2, 3, "%02x", g_peek[i].last[k]);
            float f = 0; int32_t v = 0; if (g_peek[i].n >= 4) { memcpy(&f, g_peek[i].last, 4); memcpy(&v, g_peek[i].last, 4); }
            LOG("guest peek: %s = %s (as float %g, as int %d) %.1f s in", g_peek[i].what, hx, (double)f, v, (t + 1) * 0.25); } }
    return NULL;
}
static void guest_peek(void) {
    const char *e = getenv("DM_GUEST_PEEK"); if (!e || !*e) return;
    const char *colon = strchr(e, ':'), *build = dm_main_build();
    if (!colon || colon - e != 32 || strncmp(e, build, 32)) { LOG("[RED] guest peek: DM_GUEST_PEEK is for main %.32s, this main is %s: nothing read", e, build); return; }
    for (const char *p = colon + 1; *p && g_npeek < NPEEK;) {
        const char *plus = strchr(p, '+'), *q = plus ? strchr(plus, '?') : NULL; if (!plus || !q || plus - p >= 16) break;
        char name[16] = ""; memcpy(name, p, (size_t)(plus - p)); char *end; uint64_t off = strtoull(plus + 1, &end, 16), off2 = 0; int deref = 0;
        if (*end == '>') { deref = 1; off2 = strtoull(end + 1, &end, 16); }
        module_t *m = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, name)) m = &g_mod[i];
        char *end2; long n = strtol(q + 1, &end2, 10);
        if (!m || end != q || n < 1 || n > POKE_MAX || off + (uint64_t)(deref ? 8 : n) > m->size) { LOG("[RED] guest peek: %.*s is not <module>+<offset>[><offset>]?<bytes> inside a loaded module: left out", (int)(end2 - p), p); }
        else { if (deref) snprintf(g_peek[g_npeek].what, sizeof g_peek[g_npeek].what, "[%s+%#llx]+%#llx", name, (unsigned long long)off, (unsigned long long)off2);
               else snprintf(g_peek[g_npeek].what, sizeof g_peek[g_npeek].what, "%s+%#llx", name, (unsigned long long)off);
               g_peek[g_npeek].at = m->base + off; g_peek[g_npeek].deref = deref; g_peek[g_npeek].off2 = off2; g_peek[g_npeek].n = (int)n; g_npeek++; }
        p = *end2 == ',' ? end2 + 1 : end2; if (*end2 && *end2 != ',') break;
    }
    if (g_npeek) { pthread_t t; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED); pthread_create(&t, &a, peek_watch, NULL); pthread_attr_destroy(&a); }
}
void dm_fork_guest_poke(void);
void dm_fork_guest_poke(void) {
    guest_peek();
    const char *e = getenv("DM_GUEST_POKE"); if (!e || !*e) return;
    const char *colon = strchr(e, ':'), *build = dm_main_build();
    if (!colon || colon - e != 32 || strncmp(e, build, 32)) { LOG("[RED] guest poke: DM_GUEST_POKE is for main %.32s, this main is %s: nothing written", e, build); return; }
    for (const char *p = colon + 1; *p && g_npoke < NPOKE;) {
        const char *plus = strchr(p, '+'), *eq = plus ? strchr(plus, '=') : NULL; if (!plus || !eq || plus - p >= 16) break;
        char name[16] = ""; memcpy(name, p, (size_t)(plus - p)); char *end; uint64_t off = strtoull(plus + 1, &end, 16), off2 = 0; int deref = 0;
        if (*end == '>') { deref = 1; off2 = strtoull(end + 1, &end, 16); }
        module_t *m = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, name)) m = &g_mod[i];
        int n = 0; uint8_t b[POKE_MAX]; const char *h = eq + 1;
        while (n < POKE_MAX && h[0] && h[1] && h[0] != ',' && h[1] != ',') { char x[3] = {h[0], h[1], 0}; b[n++] = (uint8_t)strtoul(x, NULL, 16); h += 2; }
        if (!m || end != eq || !n || off + (uint64_t)(deref ? 8 : n) > m->size) { LOG("[RED] guest poke: %.*s is not <module>+<offset>[><offset>]=<bytes> inside a loaded module: left out", (int)(h - p), p); }
        else if (deref) { snprintf(g_poke[g_npoke].what, sizeof g_poke[g_npoke].what, "[%s+%#llx]+%#llx (%d bytes)", name, (unsigned long long)off, (unsigned long long)off2, n);
               g_poke[g_npoke].at = m->base + off; g_poke[g_npoke].deref = 1; g_poke[g_npoke].off2 = off2; memcpy(g_poke[g_npoke].b, b, (size_t)n); g_poke[g_npoke].n = n;
               LOG("guest poke: %s: written once the game has set the pointer", g_poke[g_npoke].what); g_npoke++; }
        else { snprintf(g_poke[g_npoke].what, sizeof g_poke[g_npoke].what, "%s+%#llx (%d bytes)", name, (unsigned long long)off, n);
               g_poke[g_npoke].at = m->base + off; memcpy(g_poke[g_npoke].b, b, (size_t)n); g_poke[g_npoke].n = n;
               LOG("guest poke: %s: %02x%s -> %02x%s", g_poke[g_npoke].what, m->base[off], n > 1 ? ".." : "", b[0], n > 1 ? ".." : "");
               memcpy(m->base + off, b, (size_t)n); g_npoke++; }
        p = *h == ',' ? h + 1 : h; if (*h && *h != ',') break;
    }
    if (g_npoke) { pthread_t t; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED); pthread_create(&t, &a, poke_watch, NULL); pthread_attr_destroy(&a); }
}
