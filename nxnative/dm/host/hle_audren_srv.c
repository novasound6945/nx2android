// audren:u served by the host, for SDKs whose audio renderer exists only in the audio service. SDK 1.x (Disgaea 5: SDK 1.7.1) links the renderer's IPC client and nothing
// of the renderer itself (no nn::audio::server, no CPU command processor), so hle_audren.c's CPU + manual takeover has nothing to call: GetAudioRendererWorkBufferSize and
// OpenAudioRenderer connect to audren:u (CreateAudioRendererManagerByHipc), which the host used to refuse (an abort before the title). Here the host answers it:
//   IAudioRendererManager (audren:u): 0 OpenAudioRenderer (raw: the 0x34-byte parameter, +0x38 work buffer size, +0x40 applet resource user ID; copied handles: the work buffer's
//     transfer memory and the process, neither used) → IAudioRenderer; 1 GetWorkBufferSize (raw: the parameter) → u64; 2 GetAudioDeviceService (raw: the user ID) → IAudioDevice
//   IAudioRenderer (audren:r): 0 GetSampleRate, 1 GetSampleCount, 2 GetMixBufferCount, 3 GetState (0 started, 1 stopped), 4 RequestUpdate (A: the update's input; B: its
//     output, then the performance buffer), 5 Start, 6 Stop, 7 QuerySystemEvent (a copied event handle), 8 / 9 Set / GetRenderingTimeLimit
//   IAudioDevice (audren:d): 0 ListAudioDeviceName (B: names of 0x100; → u32 count), 1 / 2 Set / GetAudioDeviceOutputVolume (A: the name), 3 GetActiveAudioDeviceName (B),
//     4 QueryAudioDeviceSystemEvent (a copied event, never signalled), 5 GetActiveChannelCount
// (the command IDs of SDK 1.7.1's client proxies, from their vtables). The update protocol and the mixing are hle_audren_upd.c. Each renderer has a host thread that renders one
// audio frame (sample_count / sample_rate, 5 ms) whenever the output hands a buffer back, appends it, and then signals the events QuerySystemEvent handed out, as the audio service
// does after each rendered frame: the game's audio thread waits on that event, updates, and takes back the wave buffers the frames finished. Pacing follows the sound card's clock
// as hle_audren.c's render thread does (four buffers of one frame in flight); the monotonic clock paces the frames only without an output (DM_AUDREN_OUT=0) or while the output
// hands nothing back.
// Diagnostics: DM_AUDREN_WAV=<seconds> writes the first seconds of the output (16-bit stereo 48 kHz) to $DM_ROOT/audren.wav; DM_AUDREN_OUT=0 renders without an output;
// DM_AUDREN_TRACE logs the wave buffers queued and the mix buffers' peaks (trace_voices).
#include "hle.h"
#include "hle_sf.h"
#include "audren_upd.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

int svc_thread(int num, uint64_t *regs);
void *dm_pcmout_open(int ch);   // hle_audio.c: the host's own AudioOut (real output, or the silent sink), keyed by a host object
void dm_pcmout_close(void *k);
void dm_pcmout_start(void *k);
int dm_pcmout_append(void *k, void *b);
void *dm_pcmout_released(void *k);
void dm_pcmout_wait(void *k, int ms);
void dm_host_helper_qos(qos_class_t q);   // hle_thread.c

// The in-process renderer's entry point, which hle_audren.c's CPU + manual mode drives: without it the SDK has the IPC client only
int dm_audren_served(void) {
    static int s = -1;
    if (s < 0) {
        s = guest_sym("_ZN2nn5audio29ExecuteAudioRendererRenderingENS0_19AudioRendererHandleE") == NULL;
        if (s) LOG("audren: the SDK has no in-process renderer (no ExecuteAudioRendererRendering): the host serves audren:u, the game's own client talks to it");
    }
    return s;
}

enum { EV_MAX = 4, NBUF = 4 };
typedef struct { void *ptr; uint64_t cap, size; } gbuf;   // the front of hle_audio.c's AudioOutBuffer, as dm_pcmout_append takes it
typedef struct {
    aru_renderer r;
    pthread_mutex_t mu;            // r, ev: the game's requests and the frame thread
    pthread_t thr; int thr_on;
    _Atomic int quit;
    uint32_t ev[EV_MAX]; int nev;  // write ends of the events QuerySystemEvent handed out
    uint32_t time_limit;
    unsigned id;
    // the frame thread's own
    uint64_t wb_consumed, starved, unmapped, frames_logged, nonsilent, clipped, t_sum, t_max, t_n;
    uint32_t peak, warned, fx_warned;
    FILE *wav; uint64_t wav_frames, wav_max; char wav_path[512];
} ren_t;
static _Atomic unsigned g_nren;
static int trace_on(void);
static void trace_mixbufs(ren_t *s, uint64_t f);

static uint64_t mono_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static void ev_signal(uint32_t h) { uint64_t r[32] = {0}; r[0] = h; svc_thread(0x11, r); }   // SignalEvent (any thread: the kernel object table takes its own lock)
static void ev_close(uint32_t h) { uint64_t r[32] = {0}; r[0] = h; svc_thread(0x16, r); }
static uint32_t ev_create(uint32_t *read) { uint64_t r[32] = {0}; svc_thread(0x45, r); *read = (uint32_t)r[2]; return (uint32_t)r[1]; }   // CreateEvent → write end
static void sleep_until(uint64_t t) {   // (relative sleeps toward the deadline: macOS has no clock_nanosleep)
    for (uint64_t now; (now = mono_ns()) < t; ) { struct timespec ts = {(time_t)((t - now) / 1000000000ull), (long)((t - now) % 1000000000ull)}; nanosleep(&ts, NULL); }
}

// DM_AUDREN_WAV=<seconds>: the first seconds of what goes to the output, 16-bit stereo 48 kHz, for checking the sound off the device. The header's sizes are rewritten every
// 5 s and at the end, so a run that ends early leaves a readable file
static void wav_header(ren_t *s) {
    uint32_t bytes = (uint32_t)(s->wav_frames * 4), h[11] = {0x46464952, 36 + bytes, 0x45564157, 0x20746d66, 16, 0x00020001, 48000, 48000 * 4, 0x00100004, 0x61746164, bytes};
    fseek(s->wav, 0, SEEK_SET); fwrite(h, 4, 11, s->wav); fseek(s->wav, 0, SEEK_END); fflush(s->wav);
}
static void wav_open(ren_t *s) {
    const char *e = getenv("DM_AUDREN_WAV"), *root = getenv("DM_ROOT");
    if (!e || atof(e) <= 0) return;
    if (s->id) snprintf(s->wav_path, sizeof s->wav_path, "%s/audren%u.wav", root ? root : ".", s->id);
    else snprintf(s->wav_path, sizeof s->wav_path, "%s/audren.wav", root ? root : ".");
    if (!(s->wav = fopen(s->wav_path, "wb"))) { LOG("[RED] audren: DM_AUDREN_WAV cannot open %s (errno %d)", s->wav_path, errno); return; }
    setvbuf(s->wav, NULL, _IOFBF, 1 << 20);
    s->wav_max = (uint64_t)(atof(e) * 48000); wav_header(s);
    LOG("audren: renderer %u: writing the first %.1f s of its output to %s", s->id, atof(e), s->wav_path);
}
static void wav_close(ren_t *s) {
    if (!s->wav) return;
    wav_header(s); fclose(s->wav); s->wav = NULL;
    LOG("audren: renderer %u: %.2f s of output written to %s", s->id, s->wav_frames / 48000.0, s->wav_path);
}
static void wav_put(ren_t *s, const int16_t *pcm, uint32_t n) {
    if (!s->wav) return;
    if (s->wav_frames + n > s->wav_max) n = (uint32_t)(s->wav_max - s->wav_frames);
    fwrite(pcm, 4, n, s->wav); s->wav_frames += n;
    if (s->wav_frames >= s->wav_max) wav_close(s);
    else if (s->wav_frames % (48000 * 5) < n) wav_header(s);
}

// After each frame: totals for the periodic line, and one line the first time a frame meets something this renderer leaves out
static void frame_stats(ren_t *s, const aru_frame_report *fr, uint64_t f, uint64_t dt, int on) {
    s->wb_consumed += fr->wb_consumed; s->starved += fr->voices_starved; s->unmapped += fr->unmapped; s->clipped += fr->clipped;
    if (fr->peak > s->peak) s->peak = fr->peak;
    if (fr->peak) s->nonsilent++;
    s->t_sum += dt; s->t_n++; if (dt > s->t_max) s->t_max = dt;
    uint32_t w = fr->warn & ~s->warned; s->warned |= fr->warn;
    if (fr->unmapped && !(s->warned & 0x80000000u)) {
        s->warned |= 0x80000000u; unsigned att = 0; uint64_t fl;
        pthread_mutex_lock(&s->mu); for (uint32_t i = 0; i < s->r.npool; i++) att += s->r.pool[i].attached; fl = s->r.behaviour_flags; pthread_mutex_unlock(&s->mu);
        LOG("[RED] audren: renderer %u: guest memory at %#llx (a wave buffer, ADPCM table or context) lies outside every attached memory pool (%u attached, behaviour flags %#llx): "
            "it plays silence", s->id, (unsigned long long)fr->unmapped_addr, att, (unsigned long long)fl);
    }
    if (w & ARU_W_FORMAT) LOG("[RED] audren: renderer %u: a voice in a sample format the host does not decode (PCM8, PCM16, float and DSP-ADPCM are): it plays silence", s->id);
    if (w & ARU_W_SPLITTER) LOG("[RED] audren: renderer %u: a voice sends to a splitter, which the host does not route: it is not heard", s->id);
    if (w & ARU_W_MIXBUF) LOG("[RED] audren: renderer %u: the mixes in use need more than the renderer's %u mix buffers: the ones that do not fit are left out", s->id, s->r.mix_buffers);
    if (w & ARU_W_CIRCULAR) LOG("audren: renderer %u: a circular buffer sink is in use: the frames write the final mix into it", s->id);
    uint32_t fx = fr->fx_skipped & ~s->fx_warned; s->fx_warned |= fr->fx_skipped;
    if (fx) LOG("[RED] audren: renderer %u: effect types %#x in use are not processed (bit 2 aux, 3 delay, 4 reverb, 5 I3DL2 reverb, 6 biquad): their output buffers keep what the "
                "mix has (an effect in place passes its input through)", s->id, fx);
    if (on && (f == 200 || f == 2000 || (f && f % 12000 == 0)) && f != s->frames_logged) {   // after 1 s and 10 s, then every minute
        s->frames_logged = f;
        LOG("audren: renderer %u: rendered %llu frames, %llu of them not silent; peak %u, %llu samples clipped since the last line; this frame %u voices playing (%u mixed), %u starved; "
            "%llu wave buffers finished, %llu voice frames starved, %llu reads outside the attached memory pools so far; frame %.3f ms mean / %.3f ms max (%.1f ms frames, %llu updates)",
            s->id, (unsigned long long)f, (unsigned long long)s->nonsilent, s->peak, (unsigned long long)s->clipped, fr->voices_played, fr->voices_mixed, fr->voices_starved,
            (unsigned long long)s->wb_consumed, (unsigned long long)s->starved, (unsigned long long)s->unmapped, s->t_sum / (double)s->t_n / 1e6, s->t_max / 1e6,
            s->r.sample_count * 1000.0 / s->r.sample_rate, (unsigned long long)s->r.updates);
        s->peak = 0; s->clipped = 0; s->t_sum = s->t_max = s->t_n = 0;
    }
}

// The frame thread: a frame whenever the output hands a buffer back (it plays one every 5 ms; four are in flight). An output that hands nothing back for 100 ms (it stopped
// consuming) leaves the frames to the clock until it does again, so the game's audio keeps its pace; so does a renderer without an output (DM_AUDREN_OUT=0). On the clock,
// absolute deadlines every frame period; a host that stalls (more than 20 frames behind) restarts the clock instead of running the missed frames in a burst
static void *ren_main(void *arg) {
    ren_t *s = arg;
    dm_host_helper_qos(QOS_CLASS_USER_INTERACTIVE);   // the game's audio thread waits on the event it signals
    const uint64_t period = (uint64_t)s->r.sample_count * 1000000000ull / s->r.sample_rate;
    const uint32_t nsil = s->r.sample_count * 48000 / s->r.sample_rate;   // output frames of one frame (also while the renderer is stopped: silence)
    const char *oe = getenv("DM_AUDREN_OUT");
    void *out = oe && !atoi(oe) ? NULL : dm_pcmout_open(2);
    int16_t *data = calloc(NBUF + 1, 2 * ARU_OUT_MAX * sizeof *data), *scratch = data + NBUF * 2 * ARU_OUT_MAX;
    gbuf bufs[NBUF], *free_list[NBUF]; int nfree = 0, stalled = 0;
    if (!data) die("audren: no memory for the output buffers");
    for (int i = 0; i < NBUF; i++) { bufs[i] = (gbuf){data + i * 2 * ARU_OUT_MAX, 2 * ARU_OUT_MAX * sizeof *data, 0}; free_list[nfree++] = &bufs[i]; }
    if (out) dm_pcmout_start(out);
    wav_open(s);
    LOG("audren: renderer %u: frame thread started, %s", s->id, out ? "paced by the output (4 buffers of one frame in flight)" : "paced by the clock (DM_AUDREN_OUT=0: no output)");
    uint64_t next = mono_ns() + period, wait0 = 0;
    while (!atomic_load(&s->quit)) {
        gbuf *b = NULL;
        if (out) {
            for (gbuf *g; nfree < NBUF && (g = dm_pcmout_released(out)); ) free_list[nfree++] = g;
            if (nfree) {
                b = free_list[--nfree]; wait0 = 0;
                if (stalled) { stalled = 0; LOG("audren: renderer %u: the output hands buffers back again: frames follow it", s->id); }
            } else {
                uint64_t now = mono_ns();
                if (!wait0) wait0 = now;
                if (!stalled && now - wait0 < 100000000ull) { dm_pcmout_wait(out, 10); continue; }
                if (!stalled) { stalled = 1; next = now; LOG("[RED] audren: renderer %u: the output handed no buffer back for 100 ms: frames run on the clock (not heard) until it does", s->id); }
                sleep_until(next); next += period; if (mono_ns() > next + 20 * period) next = mono_ns() + period;
            }
        } else { sleep_until(next); next += period; uint64_t now = mono_ns(); if (now > next + 20 * period) next = now + period; }
        if (atomic_load(&s->quit)) { if (b) free_list[nfree++] = b; break; }
        int16_t *pcm = b ? (int16_t *)b->ptr : scratch;
        aru_frame_report fr; uint32_t ev[EV_MAX]; int nev, on; uint32_t n;
        pthread_mutex_lock(&s->mu);
        uint64_t t0 = mono_ns();
        on = s->r.started; n = aru_frame(&s->r, pcm, &fr); nev = s->nev; memcpy(ev, s->ev, sizeof ev);
        uint64_t f = s->r.frames, dt = mono_ns() - t0;
        if (on && f % 200 == 0 && trace_on()) trace_mixbufs(s, f);
        pthread_mutex_unlock(&s->mu);
        if (!n) { n = nsil; memset(pcm, 0, (size_t)n * 4); }
        if (b) { b->size = (uint64_t)n * 4; if (!dm_pcmout_append(out, b)) free_list[nfree++] = b; }
        if (on) for (int k = 0; k < nev; k++) ev_signal(ev[k]);
        wav_put(s, pcm, n);
        if (on) frame_stats(s, &fr, f, dt, on);
    }
    wav_close(s);
    if (out) dm_pcmout_close(out);   // the output would otherwise keep its sound card callback or silent sink running and hold one of the AudioOut slots
    free(data);
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

// What the game routes where, after 3 s of updates and every minute: memory pools attached, the behaviour flags, each mix in use (buffers, volume, destination), the effects
// (type, mix, order, enabled; a buffer mixer's pairs), the sinks' inputs, and the voices playing by sample format
static void routing_log(ren_t *s) {
    char line[1536]; size_t o = 0; aru_renderer *r = &s->r;
#define ADD(...) do { if (o < sizeof line) o += (size_t)snprintf(line + o, sizeof line - o, __VA_ARGS__); } while (0)
    pthread_mutex_lock(&s->mu);
    unsigned att = 0, fmt[8] = {0}; for (uint32_t i = 0; i < r->npool; i++) att += r->pool[i].attached;
    for (uint32_t i = 0; i < r->voices; i++) if (r->voice[i].in_use && r->voice[i].state == ARU_STARTED) fmt[r->voice[i].fmt & 7]++;
    ADD("%u memory pools attached, behaviour %.4s flags %#llx; voices started: %u PCM16, %u ADPCM, %u other;", att, (const char *)&r->behaviour_rev, (unsigned long long)r->behaviour_flags,
        fmt[ARU_PCM16], fmt[ARU_ADPCM], fmt[1] + fmt[3] + fmt[4] + fmt[5] + fmt[7]);
    for (uint32_t m = 0; m <= r->submixes; m++) {
        const uint8_t *p = r->mix + (size_t)ARU_IN_MIX * m; uint32_t nb; int32_t to; float vol;
        memcpy(&vol, p, 4); memcpy(&nb, p + 8, 4); memcpy(&to, p + 0x924, 4);
        if (!p[0xc]) continue;
        if (m) { ADD(" mix %u: %u buffers, volume %.3f → %d", m, nb, vol, to); for (int k = 0; k < ARU_MIXBUF * ARU_MIXBUF; k++) { float g; memcpy(&g, p + 0x24 + 4 * k, 4); if (g != 0) ADD(" %d→%d×%.3f", k / ARU_MIXBUF, k % ARU_MIXBUF, g); } ADD(";"); }
        else ADD(" final mix: %u buffers, volume %.3f;", nb, vol);
    }
    for (uint32_t i = 0; i < r->effects; i++) {
        const aru_effect *e = &r->effect[i]; uint32_t n;
        if (!e->type) continue;
        ADD(" effect %u: type %u on mix %d, order %u, %s", i, e->type, e->mix_id, e->order, e->enabled ? "enabled" : "disabled");
        memcpy(&n, e->param + 0x90, 4);
        if (e->type == ARU_FX_BUFMIX) { ADD(", %u pairs", n); for (uint32_t k = 0; k < n && k < 6; k++) { float g; memcpy(&g, e->param + 0x30 + 4 * k, 4); ADD(" %u→%u×%.3f", e->param[k], e->param[0x18 + k], g); } }
        ADD(";");
    }
    for (uint32_t i = 0; i < r->sinks; i++) {
        const aru_sink *k = &r->sink[i];
        if (!k->used) continue;
        ADD(" sink %u: %s, inputs", i, k->type == 1 ? "device" : k->type == 2 ? "circular buffer" : "?");
        for (unsigned c = 0; c < k->inputs; c++) ADD(" %u", k->input[c]);
        if (k->type == 1 && k->downmix_on) ADD(", own downmix %.3f %.3f %.3f %.3f", k->downmix[0], k->downmix[1], k->downmix[2], k->downmix[3]);
    }
    pthread_mutex_unlock(&s->mu);
#undef ADD
    LOG("audren: renderer %u routing: %s", s->id, line);
}

// DM_AUDREN_TRACE (a DM_CTL env line too): the wave buffers each update queues, with the voice's parameters, its channel resources' mix volumes and the buffer's first samples
// (the first 200 lines); the frame thread adds each mix buffer's peak once a second. Called under s->mu after an update
static int trace_on(void) { return getenv("DM_AUDREN_TRACE") != NULL; }
static void trace_voices(ren_t *s) {
    static _Atomic int nlines; aru_renderer *r = &s->r;
    for (uint32_t i = 0; i < r->voices && atomic_load(&nlines) < 200; i++) {
        aru_voice *v = &r->voice[i];
        if (!v->in_use) continue;
        for (int k = 0; k < ARU_WB; k++) {
            const aru_wb *w = &v->wb[k];
            if (w->sent) continue;
            char line[768]; size_t o = 0;
#define ADD(...) do { if (o < sizeof line) o += (size_t)snprintf(line + o, sizeof line - o, __VA_ARGS__); } while (0)
            ADD("voice %u%s state %u fmt %u %u Hz ×%u pitch %.3f volume %.3f mix %d; wave buffer %d %#llx+%#llx [%u, %u) loop %u eos %u ctx %#llx, %s", i, v->is_new ? " (new)" : "",
                v->state, v->fmt, v->rate, v->chans, v->pitch, v->volume, v->mix_id, k, (unsigned long long)w->addr, (unsigned long long)w->size, w->start, w->end, w->loop, w->eos,
                (unsigned long long)w->ctx, aru_readable(r, w->addr, w->size) ? "readable" : "NOT readable");
            if (aru_readable(r, w->addr, w->size)) {
                const uint8_t *d = (const uint8_t *)(uintptr_t)w->addr; uint64_t nb = w->size < 64 ? w->size : 64, nz = 0;
                for (uint64_t b = 0; b < w->size && b < 65536; b++) nz += d[b] != 0;
                ADD(", %llu of its first %llu bytes non-zero, first bytes", (unsigned long long)nz, (unsigned long long)(w->size < 65536 ? w->size : 65536));
                for (uint64_t b = 0; b < nb && b < 16; b++) ADD(" %02x", d[b]);
            }
            for (unsigned c = 0; c < v->chans; c++) {
                int32_t id = v->chres[c]; ADD("; channel %u resource %d", c, id);
                if (id < 0 || (uint32_t)id >= r->voices) continue;
                const uint8_t *cr = r->chres + (size_t)ARU_IN_CHRES * id; ADD(" (in use %u)", cr[0x64]);
                for (int j = 0; j < ARU_MIXBUF; j++) { float g; memcpy(&g, cr + 4 + 4 * j, 4); if (g != 0) ADD(" →%d×%.3f", j, g); }
            }
#undef ADD
            atomic_fetch_add(&nlines, 1);
            LOG("audren: renderer %u trace: %s", s->id, line);
        }
    }
}
// The update's own bytes, for a layout question: the channel resources with anything past their ID, and the voices that queue a buffer, as words (the first 40 lines)
static void trace_raw(ren_t *s, const uint8_t *in, size_t n) {
    static _Atomic int nlines; uint32_t h[10];
    if (n < 0x40 || atomic_load(&nlines) >= 40) return;
    memcpy(h, in, sizeof h);
    const uint8_t *chres = in + 0x40 + h[1] + h[2], *vo = chres + h[4];
    if ((size_t)(vo - in) + h[3] > n) return;
    for (uint32_t i = 0; i < h[4] / ARU_IN_CHRES && atomic_load(&nlines) < 40; i++) {
        const uint8_t *c = chres + ARU_IN_CHRES * i; int any = 0;
        for (int b = 4; b < ARU_IN_CHRES; b++) any |= c[b];
        if (!any) continue;
        char line[1024]; size_t o = 0;
        for (int b = 0; b < ARU_IN_CHRES && o < sizeof line; b += 4) { uint32_t w; memcpy(&w, c + b, 4); o += (size_t)snprintf(line + o, sizeof line - o, " %x", w); }
        atomic_fetch_add(&nlines, 1); LOG("audren: renderer %u trace: channel resource entry %u:%s", s->id, i, line);
    }
    for (uint32_t i = 0; i < h[3] / ARU_IN_VOICE && atomic_load(&nlines) < 40; i++) {
        const uint8_t *v = vo + ARU_IN_VOICE * i; int q = 0;
        if (!v[9]) continue;
        for (int k = 0; k < ARU_WB; k++) q |= !v[0x60 + 0x38 * k + 0x1a];
        if (!q) continue;
        char line[1536]; size_t o = 0;
        for (int b = 0; b < ARU_IN_VOICE && o < sizeof line; b += 4) { uint32_t w; memcpy(&w, v + b, 4); if (b < 0x60 || b >= 0x138) o += (size_t)snprintf(line + o, sizeof line - o, " %x:%x", b, w); }
        atomic_fetch_add(&nlines, 1); LOG("audren: renderer %u trace: voice entry %u:%s", s->id, i, line);
    }
}
static void trace_mixbufs(ren_t *s, uint64_t f) {   // under s->mu, after a frame
    aru_renderer *r = &s->r; char line[512]; size_t o = 0; const uint32_t N = r->sample_count;
    for (uint32_t b = 0; b < r->mix_buffers && o < sizeof line; b++) {
        float pk = 0; for (uint32_t i = 0; i < N; i++) { float a = r->mixbuf[(size_t)b * N + i]; a = a < 0 ? -a : a; if (a > pk) pk = a; }
        o += (size_t)snprintf(line + o, sizeof line - o, " %.0f", pk);
    }
    LOG("audren: renderer %u trace: frame %llu, mix buffer peaks:%s", s->id, (unsigned long long)f, line);
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
        if (!res && rep.wb_new && trace_on()) { trace_voices(s); trace_raw(s, (const uint8_t *)(uintptr_t)c->a[0].addr, (size_t)c->a[0].size); }
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
        if (!res && (u == 600 || u == 2000 || u % 12000 == 0)) routing_log(s);
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
