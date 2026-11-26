// This fork's: FNA3D's OpenGL driver with a main thread that waits instead of presenting (DM_FNA3D_GL_PUMP).
//
// FNA3D's OpenGL driver makes every GL call on the thread that created the device. A call from another thread (a content loader making a texture) is put on a list and that
// thread waits on a semaphore; the device's thread runs the list inside SwapBuffers, once a frame. MARVEL Cosmic Invasion (FNA 25.3 on SDL 3, main 4b8064a4…) renders with
// SDL_GPU on Vulkan on the console, where any thread makes a texture. On the hosts SDL_GPU finds no backend and FNA3D falls back to OpenGL, and the game stopped before its first
// frame: a preload thread loads content under the content manager's one lock (main+0x162a750), a texture in it waits for the main thread's next SwapBuffers, and the main
// thread, still in LoadContent ("Stages\StageData"), waits for that lock.
//
//   DM_FNA3D_GL_PUMP=<SwapBuffers offset in main, hex>,<offset of the command list in the renderer, hex>      (start-up; a game's android.tsv env row)
//
// The value starts with a nonzero number because the switch is also the gate of the two take-overs below (hle.h HLE_FORCE_IF: set and nonzero). The offsets are a
// build's: the row is good for the main whose patches.py has the renderer gate, and without that gate in the running main the pump stays off. (The first release's
// rows had main's build ID and a colon in front; that form is still read, and then the ID must be the running main's.)
//
// With it the thread that holds the GL context runs the list when it would sleep with the list not empty: its pthread_cond_wait (the .NET runtime's lock and event waits) calls
// the driver's SwapBuffers with no window. SDL refuses that swap ("Invalid window"), so nothing is presented, and the rest of the function runs the list and frees the
// disposed resources as at the end of a frame. The wait then returns as a spurious wake-up, which a caller of a condition variable handles. A thread that has put a
// call on the list and goes to its sem_wait wakes the context's thread when that one already sleeps.
// The renderer comes from an observation gate at the head of the driver's CreateTexture2D (OBS_FNA3D_RENDERER in the game's patches.py: x19).
#include "dm.h"
#include "hle.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

__attribute__((weak)) int dm_gl_has_context(void);   // dm/android/gl_android.c: this thread holds the game's GL context

static _Atomic(char *) g_renderer;
static void obs_renderer(const uint64_t *regs) { if (!atomic_load(&g_renderer)) atomic_store(&g_renderer, (char *)regs[19]); }
__attribute__((constructor)) static void pump_gate(void) { gate_obs_set(OBS_FNA3D_RENDERER, obs_renderer); }

static int g_on = -1; static uint64_t g_swap_off, g_cmd_off;
static int pump_on(void) {   // (read at the first import binding: the app's env.txt is in the environment by then)
    if (g_on >= 0) return g_on;
    const char *e = getenv("DM_FNA3D_GL_PUMP"); char *end;
    if (!e || !*e) return g_on = 0;
    const char *v = strlen(e) > 33 && e[32] == ':' ? e + 33 : e;   // (after the old form's build ID)
    if (!(g_swap_off = strtoull(v, &end, 16)) || *end != ',' || !(g_cmd_off = strtoull(end + 1, &end, 16)) || *end) {
        LOG("[RED] FNA3D GL pump: DM_FNA3D_GL_PUMP is not <SwapBuffers offset>,<command list offset>: off"); return g_on = 0; }
    return g_on = 1;
}
static void (*g_swap)(void *, void *, void *, void *);
static int g_ready;
static void pump_ready_once(void) {   // once main is loaded: the row must be for this main
    const char *e = getenv("DM_FNA3D_GL_PUMP"), *build = dm_main_build(); module_t *m = NULL;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) m = &g_mod[i];
    if (!m || g_swap_off >= m->size) { LOG("[RED] FNA3D GL pump: DM_FNA3D_GL_PUMP's SwapBuffers offset is outside main: off"); return; }
    if (strlen(e) > 33 && e[32] == ':' && strncmp(e, build, 32)) { LOG("[RED] FNA3D GL pump: DM_FNA3D_GL_PUMP is for main %.32s, this main is %s: off", e, build); return; }
    if (!gate_obs_present(OBS_FNA3D_RENDERER)) { LOG("[RED] FNA3D GL pump: main is rewritten without the renderer gate (OBS of its patches.py): off"); return; }
    g_swap = (void (*)(void *, void *, void *, void *))(m->base + g_swap_off);
    LOG("FNA3D GL pump: the GL context's thread runs the driver's command list when it waits (SwapBuffers main+%#llx, list at renderer+%#llx)", (unsigned long long)g_swap_off, (unsigned long long)g_cmd_off);
    g_ready = 1;
}
static int pump_ready(void) { static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, pump_ready_once); return g_ready; }   // (the main thread's wait and a loader's sem_wait reach here together)
static int pending(void) { char *r = atomic_load(&g_renderer); void *c; if (!r) return 0; memcpy(&c, r + g_cmd_off, sizeof c); return c != NULL; }

static __thread int t_in_pump;
static _Atomic unsigned g_pumps;
static void pump(void) {
    t_in_pump = 1; g_swap(atomic_load(&g_renderer), NULL, NULL, NULL); t_in_pump = 0;
    unsigned n = atomic_fetch_add(&g_pumps, 1) + 1;
    if (n <= 3 || (n & (n - 1)) == 0) LOG("FNA3D GL pump: command list run from a wait of the GL context's thread (%u so far)", n);
}

// The context's thread asleep in a condition wait: what a thread with a call on the list signals. Both stay valid memory after the wait (the runtime's per-thread and global wait objects)
static pthread_mutex_t g_pm = PTHREAD_MUTEX_INITIALIZER; static void *g_park_cond, *g_park_mutex;
static int (*o_cond_wait)(void *, void *), (*o_cond_broadcast)(void *), (*o_mutex_lock)(void *), (*o_mutex_unlock)(void *), (*o_sem_wait)(void *);
static int origs(void) {
    if (o_sem_wait) return 1;
    o_cond_wait = guest_sym("pthread_cond_wait"); o_cond_broadcast = guest_sym("pthread_cond_broadcast"); o_mutex_lock = guest_sym("pthread_mutex_lock");
    o_mutex_unlock = guest_sym("pthread_mutex_unlock"); void *s = guest_sym("sem_wait");
    if (!o_cond_wait || !o_cond_broadcast || !o_mutex_lock || !o_mutex_unlock || !s) die("FNA3D GL pump: the SDK lacks a pthread function the pump calls");
    o_sem_wait = s; return 1;
}
HLE_FORCE_IF("DM_FNA3D_GL_PUMP", "pthread_cond_wait", "FNA3D's OpenGL driver: the GL context's thread runs the driver's command list when it would sleep with the list not empty", int, pump_cond_wait, void *cond, void *mutex) {
    origs();
    if (!pump_on() || t_in_pump || !dm_gl_has_context || !dm_gl_has_context() || !pump_ready()) return o_cond_wait(cond, mutex);
    if (pending()) { o_mutex_unlock(mutex); pump(); o_mutex_lock(mutex); return 0; }   // (a spurious wake-up: the caller tests its condition and waits again)
    pthread_mutex_lock(&g_pm); g_park_cond = cond; g_park_mutex = mutex; pthread_mutex_unlock(&g_pm);
    int r = o_cond_wait(cond, mutex);
    pthread_mutex_lock(&g_pm); g_park_cond = g_park_mutex = NULL; pthread_mutex_unlock(&g_pm);
    if (pending()) { o_mutex_unlock(mutex); pump(); o_mutex_lock(mutex); }
    return r;
}
HLE_FORCE_IF("DM_FNA3D_GL_PUMP", "sem_wait", "FNA3D's OpenGL driver: a thread with a call on the driver's list wakes the GL context's thread when that one sleeps", int, pump_sem_wait, void *sem) {
    origs();
    if (pump_on() && !t_in_pump && pending() && pump_ready()) {
        pthread_mutex_lock(&g_pm); void *c = g_park_cond, *m = g_park_mutex; pthread_mutex_unlock(&g_pm);
        // with the sleeper's mutex held, so the wake-up cannot fall between its last test and its wait
        if (c) { o_mutex_lock(m); o_cond_broadcast(c); o_mutex_unlock(m); }
    }
    return o_sem_wait(sem);
}
