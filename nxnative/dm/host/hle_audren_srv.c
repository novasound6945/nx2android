// audren:u served by the host, for SDKs whose audio renderer exists only in the audio service. SDK 1.x (Disgaea 5: SDK 1.7.1) links the renderer's IPC client and nothing
// of the renderer itself (no nn::audio::server, no CPU command processor), so hle_audren.c's CPU + manual takeover has nothing to call: GetAudioRendererWorkBufferSize and
// OpenAudioRenderer connect to audren:u (CreateAudioRendererManagerByHipc), which the host used to refuse (an abort before the title). Here the host answers it:
//   IAudioRendererManager (audren:u): 0 OpenAudioRenderer (raw: the 0x34-byte parameter, +0x38 work buffer size, +0x40 applet resource user ID; copied handles: the work buffer's
//     transfer memory and the process, neither used) → IAudioRenderer; 1 GetWorkBufferSize (raw: the parameter) → u64; 2 GetAudioDeviceService (raw: the user ID) → IAudioDevice
//   IAudioRenderer (audren:r): 0 GetSampleRate, 1 GetSampleCount, 2 GetMixBufferCount, 3 GetState (0 started, 1 stopped), 4 RequestUpdate (A: the update's input; B: its
//     output, then the performance buffer), 5 Start, 6 Stop, 7 QuerySystemEvent (a copied event handle), 8 / 9 Set / GetRenderingTimeLimit
//   IAudioDevice (audren:d): 0 ListAudioDeviceName (B: names of 0x100; → u32 count), 1 / 2 Set / GetAudioDeviceOutputVolume (A: the name), 3 GetActiveAudioDeviceName (B),
//     4 QueryAudioDeviceSystemEvent (a copied event, never signalled), 5 GetActiveChannelCount
// (the command IDs of SDK 1.7.1's client proxies, from their vtables). The update protocol itself is hle_audren_upd.c. Each renderer has a host thread that runs one audio frame
// every sample_count / sample_rate (5 ms) while it is started and then signals the events QuerySystemEvent handed out, as the audio service does after each rendered frame: the
// game's audio thread waits on that event, updates, and takes back the wave buffers the frames finished. Phase 1 renders no sound (the frames only take samples at the rate
// playback would); the PCM path is phase 2.
#include "hle.h"
#include "hle_sf.h"
#include "audren_upd.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

int svc_thread(int num, uint64_t *regs);

// The in-process renderer's entry point, which hle_audren.c's CPU + manual mode drives: without it the SDK has the IPC client only
int dm_audren_served(void) {
    static int s = -1;
    if (s < 0) {
        s = guest_sym("_ZN2nn5audio29ExecuteAudioRendererRenderingENS0_19AudioRendererHandleE") == NULL;
        if (s) LOG("audren: the SDK has no in-process renderer (no ExecuteAudioRendererRendering): the host serves audren:u, the game's own client talks to it");
    }
    return s;
}

enum { EV_MAX = 4 };
typedef struct {
    aru_renderer r;
    pthread_mutex_t mu;            // r, ev: the game's requests and the frame thread
    pthread_t thr; int thr_on;
    _Atomic int quit;
    uint32_t ev[EV_MAX]; int nev;  // write ends of the events QuerySystemEvent handed out
    uint32_t time_limit;
    unsigned id;
    uint64_t wb_consumed, starved, frames_logged;
} ren_t;
static _Atomic unsigned g_nren;

static uint64_t mono_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static void ev_signal(uint32_t h) { uint64_t r[32] = {0}; r[0] = h; svc_thread(0x11, r); }   // SignalEvent (any thread: the kernel object table takes its own lock)
static void ev_close(uint32_t h) { uint64_t r[32] = {0}; r[0] = h; svc_thread(0x16, r); }
static uint32_t ev_create(uint32_t *read) { uint64_t r[32] = {0}; svc_thread(0x45, r); *read = (uint32_t)r[2]; return (uint32_t)r[1]; }   // CreateEvent → write end

// The frame clock: absolute deadlines every frame period. A host that stalls (more than 20 frames behind) restarts the clock instead of running the missed frames in a burst
static void *ren_main(void *arg) {
    ren_t *s = arg;
    const uint64_t period = (uint64_t)s->r.sample_count * 1000000000ull / s->r.sample_rate;
    uint64_t next = mono_ns() + period;
    while (!atomic_load(&s->quit)) {
        for (uint64_t now; (now = mono_ns()) < next; ) {   // (relative sleeps toward the deadline: macOS has no clock_nanosleep)
            struct timespec ts = {(time_t)((next - now) / 1000000000ull), (long)((next - now) % 1000000000ull)};
            nanosleep(&ts, NULL);
        }
        if (atomic_load(&s->quit)) break;
        aru_frame_report fr; uint32_t ev[EV_MAX]; int nev, on;
        pthread_mutex_lock(&s->mu);
        on = s->r.started; aru_frame(&s->r, &fr); nev = s->nev; memcpy(ev, s->ev, sizeof ev);
        s->wb_consumed += fr.wb_consumed; s->starved += fr.voices_starved;
        uint64_t f = s->r.frames;
        pthread_mutex_unlock(&s->mu);
        if (on) for (int k = 0; k < nev; k++) ev_signal(ev[k]);
        if (on && (f == 200 || f == 2000 || (f && f % 12000 == 0)) && f != s->frames_logged) {   // after 1 s and 10 s, then every minute
            s->frames_logged = f;
            LOG("audren: renderer %u: %llu frames, %llu updates; this frame %u voices playing, %u starved; %llu wave buffers finished, %llu voice frames starved so far (silent: no PCM "
                "output yet)", s->id, (unsigned long long)f, (unsigned long long)s->r.updates, fr.voices_played, fr.voices_starved, (unsigned long long)s->wb_consumed,
                (unsigned long long)s->starved);
        }
        next += period;
        uint64_t now = mono_ns();
        if (now > next + 20 * period) next = now + period;
    }
    return NULL;
}

static void out_u32(sf_call *c, uint32_t v) { memcpy(c->out, &v, 4); c->out_n = 4; }
static void out_u64(sf_call *c, uint64_t v) { memcpy(c->out, &v, 8); c->out_n = 8; }
static int once_cmd(const char *iface, unsigned cmd) {   // first time this interface's command is seen
    static uint64_t seen[3]; int k = iface[7] == 'u' ? 0 : iface[7] == 'r' ? 1 : 2;
    uint64_t bit = 1ull << (cmd & 63); int first = !(seen[k] & bit); seen[k] |= bit; return first;
}

static Result manager(sf_call *c) {
    switch (c->cmd) {
    case 0: {   // OpenAudioRenderer
        if (c->in_n < 0x48) { LOG("[RED] audren: OpenAudioRenderer with %zu bytes of arguments (0x48 expected)", c->in_n); return ARU_R_PARAM; }
        ren_t *s = calloc(1, sizeof *s);
        if (!s) return ARU_R_PARAM;
        uint32_t res = aru_open(&s->r, c->in), p[ARU_PARAM / 4]; uint64_t work;
        memcpy(p, c->in, sizeof p); memcpy(&work, c->in + 0x38, 8);
        LOG("audren: OpenAudioRenderer (host service) rate %u, %u samples per frame, %u mix buffers, %u submixes, %u voices, %u sinks, %u effects, %u performance frames, "
            "%u splitters / %u destinations, revision %.4s, work buffer %#llx (transfer memory %#x, not used): %#x", p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[9], p[10],
            (const char *)&p[12], (unsigned long long)work, c->ncopy_in ? c->copy_in[0] : 0, res);
        if (res) { LOG("[RED] audren: the renderer's parameter is not one this host takes (revision %.4s): %#x", (const char *)&p[12], res); free(s); return res; }
        pthread_mutex_init(&s->mu, NULL); s->time_limit = 100; s->id = atomic_fetch_add(&g_nren, 1);
        if (pthread_create(&s->thr, NULL, ren_main, s)) { LOG("[RED] audren: no frame thread for the renderer (errno %d)", errno); aru_close(&s->r); free(s); return ARU_R_PARAM; }
        s->thr_on = 1;
        c->objs[c->nobjs].iface = "audren:r"; c->objs[c->nobjs++].obj = s;
        return RES_OK;
    }
    case 1: {   // GetWorkBufferSize
        if (c->in_n < ARU_PARAM) return ARU_R_PARAM;
        uint64_t n = aru_work_buffer_size(c->in); out_u64(c, n);
        { static int once; if (!once++) LOG("audren: GetWorkBufferSize (host service): %#llx", (unsigned long long)n); }
        return RES_OK;
    }
    case 2:   // GetAudioDeviceService
        c->objs[c->nobjs].iface = "audren:d"; c->objs[c->nobjs++].obj = NULL;
        return RES_OK;
    }
    if (once_cmd(c->iface, c->cmd)) LOG("[RED] audren: IAudioRendererManager command %u, which the host does not serve", c->cmd);
    return RES_FAIL;
}

static Result renderer(sf_call *c, ren_t *s) {
    aru_renderer *r = &s->r;
    switch (c->cmd) {
    case 0: out_u32(c, r->sample_rate); return RES_OK;
    case 1: out_u32(c, r->sample_count); return RES_OK;
    case 2: out_u32(c, r->mix_buffers); return RES_OK;
    case 3: pthread_mutex_lock(&s->mu); out_u32(c, r->started ? 0 : 1); pthread_mutex_unlock(&s->mu); return RES_OK;
    case 4: {   // RequestUpdate
        if (c->na < 1 || c->nb < 1 || !c->a[0].addr || !c->b[0].addr) {
            LOG("[RED] audren: RequestUpdate with %u A / %u B buffers (an input and an output expected)", c->na, c->nb); return ARU_R_INVALID_UPDATE; }
        aru_report rep;
        pthread_mutex_lock(&s->mu);
        uint32_t res = aru_update(r, (const uint8_t *)(uintptr_t)c->a[0].addr, (size_t)c->a[0].size, (uint8_t *)(uintptr_t)c->b[0].addr, (size_t)c->b[0].size, &rep);
        uint64_t u = r->updates;
        pthread_mutex_unlock(&s->mu);
        static _Atomic int nerr;
        if (res) { if (atomic_fetch_add(&nerr, 1) < 8) LOG("[RED] audren: RequestUpdate refused (%#x): %s (revision %.4s, input %#llx bytes, output %#llx bytes, answer %#x)", res, rep.error,
                                                         (const char *)&rep.revision, (unsigned long long)c->a[0].size, (unsigned long long)c->b[0].size, rep.out_total); }
        else if (u == 1 || u == 600 || u % 12000 == 0) {
            LOG("audren: RequestUpdate #%llu (revision %.4s): %u memory pools (%u attached, %u detached now), %u voices (%u in use, %u started, %u new wave buffers), "
                "%u channel resources, %u effects (types %#x), %u mixes, %u sinks (types %#x), %u splitter bytes; answer %#x bytes in a %#llx-byte buffer",
                (unsigned long long)u, (const char *)&rep.revision, rep.npools, rep.pools_attached, rep.pools_detached, rep.nvoices, rep.voices_in_use, rep.voices_started, rep.wb_new,
                rep.nchres, rep.neffects, rep.effect_types, rep.nmixes, rep.nsinks, rep.sink_types, rep.splitter_bytes, rep.out_total, (unsigned long long)c->b[0].size);
            if (rep.out_total != c->b[0].size) LOG("[RED] audren: the answer's size %#x is not the output buffer's %#llx: the client aborts on that", rep.out_total, (unsigned long long)c->b[0].size);
        }
        { static int fx_once, sink_once;
          if (rep.effect_types & ~(1u << 1) && !fx_once++) LOG("audren: effect types %#x in use (bit 1 buffer mixer, 2 aux, 3 delay, 4 reverb, 5 I3DL2 reverb, 6 biquad): no effects are processed yet", rep.effect_types);
          if (rep.sink_types & (1u << 2) && !sink_once++) LOG("[RED] audren: a circular buffer sink is in use: it receives nothing (no rendering yet)"); }
        return res;
    }
    case 5: pthread_mutex_lock(&s->mu); r->started = 1; pthread_mutex_unlock(&s->mu); LOG("audren: renderer %u started", s->id); return RES_OK;
    case 6: pthread_mutex_lock(&s->mu); r->started = 0; pthread_mutex_unlock(&s->mu); LOG("audren: renderer %u stopped", s->id); return RES_OK;
    case 7: {   // QuerySystemEvent: the read end is the client's; the write end is signalled every frame
        uint32_t rd, w = ev_create(&rd); int kept;
        pthread_mutex_lock(&s->mu); kept = s->nev < EV_MAX; if (kept) s->ev[s->nev++] = w; pthread_mutex_unlock(&s->mu);
        if (!kept) { LOG("[RED] audren: more than %d renderer events asked for: this one is never signalled", EV_MAX); ev_close(w); }
        c->copy_out[c->ncopy_out++] = rd;
        return RES_OK;
    }
    case 8: if (c->in_n >= 4) memcpy(&s->time_limit, c->in, 4); return RES_OK;
    case 9: out_u32(c, s->time_limit); return RES_OK;
    }
    if (once_cmd(c->iface, c->cmd)) LOG("[RED] audren: IAudioRenderer command %u, which the host does not serve", c->cmd);
    return RES_FAIL;
}

// One output device, the built-in speaker, stereo (hle_audren.c answers the SDK's device functions the same way; they rarely reach this service)
static Result device(sf_call *c) {
    static const char name[] = "AudioBuiltInSpeakerOutput";
    static _Atomic float vol = 1.0f;
    switch (c->cmd) {
    case 0:   // ListAudioDeviceName
        if (c->nb && c->b[0].addr && c->b[0].size >= 0x100) { memset((void *)(uintptr_t)c->b[0].addr, 0, 0x100); memcpy((void *)(uintptr_t)c->b[0].addr, name, sizeof name); out_u32(c, 1); }
        else out_u32(c, 0);
        return RES_OK;
    case 1: if (c->in_n >= 4) { float v; memcpy(&v, c->in, 4); atomic_store(&vol, v); } return RES_OK;
    case 2: { float v = atomic_load(&vol); memcpy(c->out, &v, 4); c->out_n = 4; return RES_OK; }
    case 3:   // GetActiveAudioDeviceName
        if (c->nb && c->b[0].addr && c->b[0].size) { size_t n = c->b[0].size < 0x100 ? (size_t)c->b[0].size : 0x100; memset((void *)(uintptr_t)c->b[0].addr, 0, n); memcpy((void *)(uintptr_t)c->b[0].addr, name, n < sizeof name ? n : sizeof name); }
        return RES_OK;
    case 4: { uint32_t rd, w = ev_create(&rd); ev_close(w); c->copy_out[c->ncopy_out++] = rd; return RES_OK; }   // no device changes: nothing signals it
    case 5: out_u32(c, 2); return RES_OK;
    }
    if (once_cmd(c->iface, c->cmd)) LOG("[RED] audren: IAudioDevice command %u, which the host does not serve", c->cmd);
    return RES_FAIL;
}

Result sf_audren(sf_call *c) {
    if (!strcmp(c->iface, "audren:u")) return manager(c);
    if (!strcmp(c->iface, "audren:r") && c->obj) return renderer(c, c->obj);
    if (!strcmp(c->iface, "audren:d")) return device(c);
    LOG("[RED] audren: a request on %s, an interface the host does not know", c->iface);
    return RES_FAIL;
}

// The renderer's last session closed (CloseAudioRenderer releases its proxy): the frame thread is joined before the state goes. The read ends stay the game's (its
// SystemEvent closes them); the write ends are the service's
void sf_audren_close(const char *iface, void *obj) {
    if (strcmp(iface, "audren:r")) return;
    ren_t *s = obj;
    atomic_store(&s->quit, 1);
    if (s->thr_on) pthread_join(s->thr, NULL);
    for (int k = 0; k < s->nev; k++) ev_close(s->ev[k]);
    LOG("audren: renderer %u closed after %llu frames and %llu updates", s->id, (unsigned long long)s->r.frames, (unsigned long long)s->r.updates);
    aru_close(&s->r); pthread_mutex_destroy(&s->mu); free(s);
}
