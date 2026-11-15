// The audio renderer's update protocol, server side (audren_upd.h): pure state and buffers, no host calls (the IPC is hle_audren_srv.c).
// The "DSP" of a frame: wave buffers are taken at the rate a real renderer plays them (each voice's pitch and sample rate against the renderer's; the out status the game reads
// follows that count alone), decoded, resampled to the renderer's rate, run through the voice's biquad filters, ramped to the voice and channel volumes into the destination
// mix, the submixes deepest first into the final mix, and the device sink's inputs down to stereo int16 at 48 kHz.
//
// The mixer (sample decoding, the 4-point resampler, the biquad and ramp loops, the submix order, the sink's downmix) is ported from LAS Recomp's runtime/services/audio.c
// (MIT License, Copyright (c) 2026 Peiikai), itself modelled on Ryujinx's AudioRenderer (VoiceState / DataSourceHelper / CommandGenerator; MIT License, Copyright (c) Ryujinx Team
// and Contributors). The license texts are in dm/third_party/lasrecomp/LICENSE and dm/third_party/ryujinx/LICENSE.txt (dm/third_party/README.md lists the piece).
#include "audren_upd.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static int16_t rd16s(const uint8_t *p) { int16_t v; memcpy(&v, p, 2); return v; }
static float rdf(const uint8_t *p) { float v; memcpy(&v, p, 4); return v; }
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void wr64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
// "REVn" read as a little-endian word: n, or 0 for anything else
static unsigned rev_num(uint32_t rev) { unsigned n = (rev >> 24) - '0'; return (rev & 0xffffffu) == 0x564552u && n <= 9 ? n : 0; }

// The 3:2 upsampler of a 32 kHz renderer's output (hle_audren.c up_3_2, the same filter with the state per renderer): a windowed sinc (Blackman, 16 taps, cutoff 0.9 of the
// input's Nyquist) in three phases, evaluated 8 input samples late so every tap is known, with the last 15 samples kept across frames
static float g_up_h[3][ARU_UP_TAPS];
static void up_init(void) {
    if (g_up_h[0][ARU_UP_TAPS / 2 - 1] != 0) return;
    for (int p = 0; p < 3; p++) {
        double sum = 0, w[ARU_UP_TAPS];
        for (int k = 0; k < ARU_UP_TAPS; k++) {
            double x = p / 3.0 + ARU_UP_TAPS / 2 - 1 - k, fc = 0.9, s = x == 0 ? fc : sin(M_PI * fc * x) / (M_PI * x);
            double t = (x + ARU_UP_TAPS / 2) / ARU_UP_TAPS, win = 0.42 - 0.5 * cos(2 * M_PI * t) + 0.08 * cos(4 * M_PI * t);
            w[k] = s * win; sum += w[k];
        }
        for (int k = 0; k < ARU_UP_TAPS; k++) g_up_h[p][k] = (float)(w[k] / sum);
    }
}
static uint32_t up_3_2(aru_renderer *r, const float *in, uint32_t n, float *out) {   // stereo; out may be in
    float *buf = r->up_buf;
    memcpy(buf + 2 * (ARU_UP_TAPS - 1), in, (size_t)n * 8);
    uint32_t m = 0;
    for (; r->up_pos < 3 * n; r->up_pos += 2, m++) {
        const float *x = buf + 2 * (r->up_pos / 3), *c = g_up_h[r->up_pos % 3]; float l = 0, rr = 0;
        for (int k = 0; k < ARU_UP_TAPS; k++) { l += c[k] * x[2 * k]; rr += c[k] * x[2 * k + 1]; }
        out[2 * m] = l; out[2 * m + 1] = rr;
    }
    r->up_pos -= 3 * n;
    memmove(buf, buf + 2 * n, (ARU_UP_TAPS - 1) * 8);
    return m;
}

uint32_t aru_open(aru_renderer *r, const uint8_t *p) {
    memset(r, 0, sizeof *r);
    r->sample_rate = rd32(p); r->sample_count = rd32(p + 4); r->mix_buffers = rd32(p + 8); r->submixes = rd32(p + 0xc); r->voices = rd32(p + 0x10);
    r->sinks = rd32(p + 0x14); r->effects = rd32(p + 0x18); r->perf_frames = rd32(p + 0x1c); r->voice_drop = p[0x20]; r->splitters = rd32(p + 0x24);
    r->splitter_dests = rd32(p + 0x28); r->revision = rd32(p + 0x30);
    unsigned n = rev_num(r->revision);
    if (n < 1 || n > 4) return ARU_R_REVISION;   // REV5 (6.0.0) on changes the formats (render info, performance v2); those SDKs render in process (hle_audren.c)
    if ((r->sample_rate != 48000 && r->sample_rate != 32000) || !r->sample_count || r->sample_count > 1024 || !r->mix_buffers || r->mix_buffers > 256 || r->submixes > 256 ||
        r->voices > 1024 || r->sinks > 32 || r->effects > 256)
        return ARU_R_PARAM;
    uint32_t nv = r->voices ? r->voices : 1, N = r->sample_count;
    r->voice = calloc(nv, sizeof *r->voice); r->effect = calloc(r->effects ? r->effects : 1, sizeof *r->effect);
    r->sink = calloc(r->sinks ? r->sinks : 1, sizeof *r->sink); r->mix = calloc(r->submixes + 1, ARU_IN_MIX); r->chres = calloc(nv, ARU_IN_CHRES);
    r->chres_prev = calloc(nv, sizeof *r->chres_prev);
    r->npool = r->effects + r->voices * ARU_WB;   // the client's memory pool count for this parameter (Disgaea 5: 5 effects + 128 voices × 4 = 517); more grow it
    r->pool = calloc(r->npool ? r->npool : 1, sizeof *r->pool);
    r->mixbuf = calloc((size_t)r->mix_buffers * N, sizeof(float));
    r->src = calloc(ARU_HIST + 64 * (size_t)N + 2, sizeof(float));   // a frame takes at most 64 × sample_count + 1 samples (aru_frame_samples)
    r->vbuf = calloc(N, sizeof(float)); r->stereo = calloc(2 * (size_t)(N * 3 / 2 + 2), sizeof(float));
    r->mix_off = calloc(r->submixes + 1, sizeof *r->mix_off);
    if (!r->voice || !r->effect || !r->sink || !r->mix || !r->chres || !r->chres_prev || !r->pool || !r->mixbuf || !r->src || !r->vbuf || !r->stereo || !r->mix_off) {
        aru_close(r); return ARU_R_PARAM; }
    up_init();
    return 0;
}
void aru_close(aru_renderer *r) {
    free(r->voice); free(r->effect); free(r->sink); free(r->mix); free(r->chres); free(r->chres_prev); free(r->pool); free(r->mixbuf); free(r->src); free(r->vbuf);
    free(r->stereo); free(r->mix_off);
    memset(r, 0, sizeof *r);
}

// One voice's in parameter (0x170): +8 new, +9 in use, +0xa play state, +0xb sample format, +0xc sample rate, +0x18 channels, +0x1c pitch, +0x20 volume, +0x24 two biquad
// filters, +0x3c wave buffers queued, +0x48 / +0x50 data source state (ADPCM coefficients), +0x58 mix, +0x5c splitter, +0x60 four wave buffers of 0x38, +0x140 six channel
// resource IDs, +0x159 wave buffers to flush
static void voice_update(aru_renderer *r, aru_voice *v, const uint8_t *p, aru_report *rep) {
    if (p[8] || !v->in_use) {   // new (and a slot that comes into use without the flag): nothing queued, nothing played
        memset(v, 0, sizeof *v);
        v->state = ARU_STOPPED; v->is_new = 1;
        for (int k = 0; k < ARU_WB; k++) v->wb[k].sent = 1;
    }
    v->in_use = 1;
    switch (p[0xa]) {
    case 0: v->state = ARU_STARTED; break;
    case 1: if (v->state != ARU_STOPPED) v->state = ARU_STOPPING; break;
    case 2: v->state = ARU_PAUSED; break;
    }
    uint32_t ch = rd32(p + 0x18);
    v->fmt = p[0xb]; v->rate = rd32(p + 0xc); v->chans = ch < 1 ? 1 : ch > ARU_CH ? ARU_CH : (uint8_t)ch;
    v->pitch = rdf(p + 0x1c); v->volume = rdf(p + 0x20); v->wb_count = rd32(p + 0x3c);
    for (int f = 0; f < 2; f++)   // a filter turned on starts from rest (Ryujinx: BiquadFilterNeedInitialization)
        if (p[0x24 + 12 * f] && !v->biquad[f][0]) for (int c = 0; c < ARU_CH; c++) memset(v->ch[c].bq[f], 0, sizeof v->ch[c].bq[f]);
    memcpy(v->biquad, p + 0x24, sizeof v->biquad);
    v->adpcm_coef = rd64(p + 0x48); v->adpcm_coef_size = rd64(p + 0x50);
    v->mix_id = (int32_t)rd32(p + 0x58); v->splitter_id = (int32_t)rd32(p + 0x5c);
    for (int c = 0; c < ARU_CH; c++) v->chres[c] = (int32_t)rd32(p + 0x140 + 4 * c);
    v->flush += p[0x159];
    for (int k = 0; k < ARU_WB; k++) {
        const uint8_t *w = p + 0x60 + 0x38 * k;
        if (w[0x1a]) continue;   // sent in an earlier update
        aru_wb *b = &v->wb[k];   // a buffer the client appended since: playable from the next frame (if the voice is started then)
        b->addr = rd64(w); b->size = rd64(w + 8); b->start = rd32(w + 0x10); b->end = rd32(w + 0x14); b->loop = w[0x18]; b->eos = w[0x19];
        b->ctx = rd64(w + 0x20); b->ctx_size = rd64(w + 0x28); b->sent = 0;
        rep->wb_new++;
    }
    (void)r;
}

uint32_t aru_update(aru_renderer *r, const uint8_t *in, size_t in_n, uint8_t *out, size_t out_n, aru_report *rep) {
    memset(rep, 0, sizeof *rep);
    if (in_n < ARU_IN_HDR) { rep->error = "the input is shorter than its header"; return ARU_R_INVALID_UPDATE; }
    uint32_t rev = rd32(in);
    rep->revision = rev;
    unsigned rn = rev_num(rev);
    if (rn < 1 || rn > 4) { rep->error = "an update revision this service does not take (REV1-REV4)"; return ARU_R_REVISION; }
    uint32_t bsz = rd32(in + 4), psz = rd32(in + 8), vsz = rd32(in + 0xc), csz = rd32(in + 0x10), esz = rd32(in + 0x14), msz = rd32(in + 0x18), ssz = rd32(in + 0x1c),
             fsz = rd32(in + 0x20), spsz = rd32(in + 0x24);
    uint64_t sum = (uint64_t)ARU_IN_HDR + bsz + psz + csz + vsz + esz + spsz + msz + ssz + fsz;
    if (sum > in_n) { rep->error = "the sections run past the input buffer"; return ARU_R_INVALID_UPDATE; }
    if (bsz != ARU_IN_BEHAVIOUR || fsz != ARU_IN_PERF || psz % ARU_IN_POOL || csz % ARU_IN_CHRES || vsz % ARU_IN_VOICE || esz % ARU_IN_EFFECT || msz % ARU_IN_MIX ||
        ssz % ARU_IN_SINK) { rep->error = "a section is not a whole number of entries"; return ARU_R_INVALID_UPDATE; }
    uint32_t np = psz / ARU_IN_POOL, nc = csz / ARU_IN_CHRES, nv = vsz / ARU_IN_VOICE, ne = esz / ARU_IN_EFFECT, nm = msz / ARU_IN_MIX, ns = ssz / ARU_IN_SINK;
    rep->npools = np; rep->nchres = nc; rep->nvoices = nv; rep->neffects = ne; rep->nmixes = nm; rep->nsinks = ns; rep->splitter_bytes = spsz;
    if (nv > r->voices || nc > r->voices || ne > r->effects || ns > r->sinks || nm > r->submixes + 1) { rep->error = "more entries than the renderer was opened with"; return ARU_R_INVALID_UPDATE; }
    uint32_t total = ARU_IN_HDR + np * ARU_OUT_POOL + nv * ARU_OUT_VOICE + ne * ARU_OUT_EFFECT + ns * ARU_OUT_SINK + ARU_OUT_PERF + ARU_OUT_BEHAVIOUR;
    rep->out_total = total;
    if (total > out_n) { rep->error = "the output buffer is smaller than the answer"; return ARU_R_INVALID_UPDATE; }
    if (np > r->npool) {   // more pools than the parameter implies: the table grows (the new ones detached)
        aru_pool *g = realloc(r->pool, (size_t)np * sizeof *g);
        if (!g) { rep->error = "no memory for the memory pool table"; return ARU_R_INVALID_UPDATE; }
        memset(g + r->npool, 0, (size_t)(np - r->npool) * sizeof *g); r->pool = g; r->npool = np;
    }
    memset(out, 0, total);
    const uint8_t *q = in + ARU_IN_HDR; uint8_t *o = out + ARU_IN_HDR;
    // behaviour: the client's revision and flags (bit 0: memory pool force mapping); no errors to report back (its output section stays zero)
    r->behaviour_rev = rd32(q); r->behaviour_flags = rd64(q + 8); q += bsz;
    // memory pools (+0 address, +8 size, +0x10 the client's state): a requested attach is attached, a requested detach detached; any other state is left as it is (0). The
    // frames read guest memory only inside the attached ones
    for (uint32_t i = 0; i < np; i++) {
        const uint8_t *mp = q + ARU_IN_POOL * i; uint32_t st = rd32(mp + 0x10);
        if (st == 4) { wr32(o + ARU_OUT_POOL * i, 5); r->pool[i].addr = rd64(mp); r->pool[i].size = rd64(mp + 8); r->pool[i].attached = 1; rep->pools_attached++; }
        else if (st == 2) { wr32(o + ARU_OUT_POOL * i, 3); r->pool[i].attached = 0; rep->pools_detached++; }
    }
    q += psz; o += np * ARU_OUT_POOL;
    // voice channel resources: kept by their place in the section, which is what a voice's channel resource IDs name (Ryujinx's UpdateVoiceChannelResources). Not by the ID
    // field: Disgaea 5 sends 128 entries of which only the ones in use carry theirs, the rest read ID 0, and kept by ID they wrote zero volumes over resource 0's (silence)
    for (uint32_t i = 0; i < nc; i++) memcpy(r->chres + (size_t)ARU_IN_CHRES * i, q + ARU_IN_CHRES * i, ARU_IN_CHRES);
    q += csz;
    // voices: out status of every slot, played samples and finished wave buffers as the frames left them
    for (uint32_t i = 0; i < nv; i++) {
        const uint8_t *p = q + ARU_IN_VOICE * i; uint8_t *vo = o + ARU_OUT_VOICE * i; aru_voice *v = &r->voice[i];
        if (!p[9]) { if (v->in_use) memset(v, 0, sizeof *v); continue; }
        voice_update(r, v, p, rep);
        rep->voices_in_use++; if (v->state == ARU_STARTED) rep->voices_started++;
        wr64(vo, v->ps.played); wr32(vo + 8, v->ps.consumed); vo[0xc] = 0;   // voices are never dropped
    }
    q += vsz; o += nv * ARU_OUT_VOICE;
    // effects (+0 type, +1 new, +2 enabled, +4 mix, +0x18 processing order, +0x20 the type's parameter): state 3 enabled / 4 disabled, as the server reports them (disabled while
    // the renderer is stopped)
    for (uint32_t i = 0; i < ne; i++) {
        const uint8_t *e = q + ARU_IN_EFFECT * i; aru_effect *x = &r->effect[i];
        if (e[0] != x->type) { x->type = e[0]; x->usage = ARU_FX_INVALID; }
        if (e[1]) x->usage = ARU_FX_NEW;
        x->enabled = e[2]; x->mix_id = (int32_t)rd32(e + 4); x->order = rd32(e + 0x18); memcpy(x->param, e + 0x20, sizeof x->param);
        o[ARU_OUT_EFFECT * i] = r->started && x->usage != ARU_FX_DISABLED ? 3 : 4;
        if (x->type && x->type < 32) rep->effect_types |= 1u << x->type;
    }
    q += esz; o += ne * ARU_OUT_EFFECT;
    q += spsz;   // splitter: not routed
    for (uint32_t i = 0; i < nm; i++) memcpy(r->mix + (size_t)ARU_IN_MIX * i, q + (size_t)ARU_IN_MIX * i, ARU_IN_MIX);
    q += msz;
    // sinks (+0 type 1 device / 2 circular buffer, +1 in use, +0x20 the type's parameter). A device: +0x120 input count, +0x124 inputs, +0x12b its own downmix coefficients on,
    // +0x12c the four of them. A circular buffer: +0x20 address, +0x28 size, +0x2c input count, +0x40 inputs
    for (uint32_t i = 0; i < ns; i++) {
        const uint8_t *s = q + ARU_IN_SINK * i; aru_sink *k = &r->sink[i];
        k->type = s[0]; k->used = s[1];
        if (k->type == 1) {
            uint32_t n = rd32(s + 0x120); k->inputs = n > ARU_CH ? ARU_CH : (uint8_t)n; memcpy(k->input, s + 0x124, ARU_CH);
            k->downmix_on = s[0x12b]; for (int c = 0; c < 4; c++) k->downmix[c] = rdf(s + 0x12c + 4 * c);
        } else if (k->type == 2) {
            uint32_t n = rd32(s + 0x2c);
            k->buf_addr = rd64(s + 0x20); k->buf_size = rd32(s + 0x28); k->inputs = n > ARU_CH ? ARU_CH : (uint8_t)n; memcpy(k->input, s + 0x40, ARU_CH);
            wr32(o + ARU_OUT_SINK * i, k->last_written);
        }
        if (k->used && k->type < 32) rep->sink_types |= 1u << k->type;
    }
    q += ssz; o += ns * ARU_OUT_SINK;
    o += ARU_OUT_PERF;        // performance: nothing written into the performance buffer
    o += ARU_OUT_BEHAVIOUR;   // behaviour: no errors
    wr32(out, rev); wr32(out + 4, ARU_OUT_BEHAVIOUR); wr32(out + 8, np * ARU_OUT_POOL); wr32(out + 0xc, nv * ARU_OUT_VOICE); wr32(out + 0x14, ne * ARU_OUT_EFFECT);
    wr32(out + 0x1c, ns * ARU_OUT_SINK); wr32(out + 0x20, ARU_OUT_PERF); wr32(out + 0x3c, total);
    r->updates++;
    return 0;
}

// The source-to-renderer rate ratio a voice plays at: its sample rate against the renderer's, times its pitch; 0 for a voice that stands still (pitch 0, not a number), at most 64
static double voice_ratio(const aru_renderer *r, const aru_voice *v) {
    double ratio = r->sample_rate ? (double)v->rate / r->sample_rate * v->pitch : 0;
    if (!(ratio > 0)) return 0;
    return ratio > 64 ? 64 : ratio;
}
uint32_t aru_frame_samples(const aru_renderer *r, const aru_voice *v, double *frac) {
    double ratio = voice_ratio(r, v);
    if (!(ratio > 0)) return 0;   // pitch 0 (or not a number): the voice stands still
    double t = *frac + ratio * r->sample_count;
    uint32_t n = (uint32_t)t; *frac = t - n;
    return n;
}

// Guest memory the "DSP" may read: inside an attached memory pool, as the DSP sees only those (Ryujinx's PoolMapper), or anywhere when the client turned force mapping on
// (behaviour flag bit 0). A read that is refused plays silence and is counted for the caller's log
static int mem_ok(aru_renderer *r, uint64_t a, uint64_t n, aru_frame_report *f) {
    if (a && n && a + n > a) {
        for (uint32_t k = 0; k <= r->npool; k++) {   // the pool of the last hit first, then all of them
            uint32_t i = k ? k - 1 : r->pool_hit; if (i >= r->npool) continue;
            const aru_pool *p = &r->pool[i];
            if (p->attached && a >= p->addr && a - p->addr < p->size && n <= p->size - (a - p->addr)) { r->pool_hit = i; return 1; }
        }
        if (r->behaviour_flags & 1) return 1;
    }
    if (f && !f->unmapped++) f->unmapped_addr = a;
    return 0;
}
int aru_readable(aru_renderer *r, uint64_t a, uint64_t n) { return mem_ok(r, a, n, NULL); }

// DSP-ADPCM: frames of 8 bytes, a predictor / scale byte (coefficient pair in the high nibble, scale in the low) and 14 four-bit samples, high nibble first; each sample is
// (nibble << scale << 11) + c0 × the last sample + c1 × the one before, rounded down from Q11 and clamped. Decodes n samples from sample s on, carrying the context in ch
static void adpcm(const uint8_t *data, uint64_t size, const uint8_t *coef, aru_chan *ch, uint64_t s, uint32_t n, float *dst) {
    for (uint32_t i = 0; i < n; i++, s++) {
        uint64_t hb = s / 14 * 8; uint32_t j = (uint32_t)(s % 14);
        if (!j) { if (hb >= size) { memset(dst + i, 0, (size_t)(n - i) * sizeof *dst); return; } ch->ps = data[hb]; }
        uint64_t b = hb + 1 + j / 2;
        if (b >= size) { memset(dst + i, 0, (size_t)(n - i) * sizeof *dst); return; }
        int nib = j & 1 ? (int8_t)(uint8_t)(data[b] << 4) >> 4 : (int8_t)data[b] >> 4;
        int scale = ch->ps & 15, ci = (ch->ps >> 4) & 7;
        int64_t v = (int64_t)nib * (1 << scale) * 2048 + 1024 + (int64_t)rd16s(coef + 4 * ci) * ch->h0 + (int64_t)rd16s(coef + 4 * ci + 2) * ch->h1;
        v >>= 11; v = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
        ch->h1 = ch->h0; ch->h0 = (int16_t)v; dst[i] = (float)v;
    }
}

// n samples of channel c from a wave buffer, off samples into it, as floats in int16 scale; silence for what cannot be read. PCM is interleaved (each channel its own index in
// a frame of all channels) and read where the sample offsets say, as the DSP does (Ryujinx's PcmHelper takes the span from the offsets): Disgaea 5 streams its stereo
// music in buffers of [0, 30720) frames 0x1e000 bytes apart whose size field says 0xf000, and read only up to the size, half of every buffer was silence. ADPCM is mono, a
// voice's further channels have nothing of their own, and is read within its size. An ADPCM buffer starting over (its first sample, also each time a loop comes back to it)
// takes the context the client gave it, from REV2 on (Ryujinx: IsAdpcmLoopContextBugFixed); before, and without one, the decoder runs on from where it was
static void wb_decode(aru_renderer *r, aru_voice *v, const aru_wb *w, unsigned c, uint32_t off, uint32_t n, float *dst, aru_frame_report *f) {
    uint64_t s0 = (uint64_t)w->start + off;
    const uint8_t *d = (const uint8_t *)(uintptr_t)w->addr;
    switch (v->fmt) {
    case ARU_PCM8: case ARU_PCM16: case ARU_PCMF: {
        unsigned bps = v->fmt == ARU_PCM8 ? 1 : v->fmt == ARU_PCM16 ? 2 : 4; uint64_t fsz = (uint64_t)bps * v->chans;
        if (!w->addr || !mem_ok(r, w->addr + s0 * fsz, n * fsz, f)) break;
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t *x = d + (s0 + i) * fsz + (uint64_t)c * bps;
            if (bps == 2) dst[i] = rd16s(x);
            else if (bps == 1) dst[i] = (float)((int8_t)x[0] * 256);
            else { float y = rdf(x) * 32767.f; dst[i] = y > 32767.f ? 32767.f : y < -32768.f ? -32768.f : y == y ? y : 0; }
        }
        return;
    }
    case ARU_ADPCM: {
        aru_chan *ch = &v->ch[c];
        if (c) break;
        if (!off && w->ctx && rev_num(r->revision) >= 2 && mem_ok(r, w->ctx, 6, f)) {
            const uint8_t *x = (const uint8_t *)(uintptr_t)w->ctx; ch->ps = rd16s(x); ch->h0 = rd16s(x + 2); ch->h1 = rd16s(x + 4); }
        if (!mem_ok(r, w->addr, w->size, f) || !mem_ok(r, v->adpcm_coef, 0x20, f)) break;
        adpcm(d, w->size, (const uint8_t *)(uintptr_t)v->adpcm_coef, ch, s0, n, dst);
        return;
    }
    default: f->warn |= ARU_W_FORMAT; break;
    }
    memset(dst, 0, (size_t)n * sizeof *dst);
}

// A started voice takes its frame's samples from the queued wave buffers in order. A buffer played to its end is finished (the out status counts it, the client takes it back),
// unless it loops: WaveBuffer version 1 (REV1-REV4) loops over [start, end) until the voice stops. An end-of-stream buffer resets the played sample count when it finishes.
// With nothing queued the voice starves (plays silence) and stays started. Channel c's samples go to dst; *ps and the counts in f (when count is set) advance by the walk,
// which is the same for every channel of the voice. Returns the samples taken (the rest of need was starved)
static uint32_t voice_take(aru_renderer *r, aru_voice *v, aru_play *ps, unsigned c, float *dst, uint32_t need, int count, aru_frame_report *f) {
    uint32_t got = 0;
    for (int guard = 0; need && guard <= ARU_WB; guard++) {   // each turn finishes a buffer or takes all that is left
        uint32_t k = ps->index % ARU_WB;
        if (!ps->valid[k]) break;
        aru_wb *w = &v->wb[k];
        uint32_t len = w->end > w->start ? w->end - w->start : 0;
        if (w->loop && len) {
            ps->played += need; ps->offset %= len;   // (a flush leaves the offset as it was: the next buffer starts there, as on the DSP)
            while (need) { uint32_t m = len - ps->offset < need ? len - ps->offset : need; wb_decode(r, v, w, c, ps->offset, m, dst + got, f); ps->offset = (ps->offset + m) % len; got += m; need -= m; }
            break;
        }
        uint32_t avail = len > ps->offset ? len - ps->offset : 0, take = need < avail ? need : avail;
        if (take) wb_decode(r, v, w, c, ps->offset, take, dst + got, f);
        ps->offset += take; ps->played += take; need -= take; got += take;
        if (ps->offset >= len) {
            ps->offset = 0; ps->valid[k] = 0; ps->index = (k + 1) % ARU_WB; ps->consumed++; if (count) f->wb_consumed++;
            if (w->eos) ps->played = 0;
        }
    }
    return got;
}

// 4-point (Catmull-Rom) interpolation of n output samples at frac + i × ratio source samples into s, s[0, ARU_HIST) being the last frame's last samples: output i lies between
// s[k + 1] and s[k + 2] (k its whole part), a fixed delay of three source samples. At the renderer's own rate the samples pass unchanged
static void resample(const float *s, float *o, uint32_t n, double frac, double ratio) {
    if (ratio == 1.0 && frac == 0) { memcpy(o, s + 1, (size_t)n * sizeof *o); return; }
    for (uint32_t i = 0; i < n; i++) {
        double pos = frac + ratio * i; uint32_t k = (uint32_t)pos; float t = (float)(pos - k);
        const float *y = s + k;
        float c1 = 0.5f * (y[2] - y[0]), c2 = y[0] - 2.5f * y[1] + 2 * y[2] - 0.5f * y[3], c3 = 0.5f * (y[3] - y[0]) + 1.5f * (y[1] - y[2]);
        o[i] = ((c3 * t + c2) * t + c1) * t + y[1];
    }
}

// A voice biquad filter as sent (+2 b0 b1 b2, +8 a1 a2, s16 in Q14; the denominator already negated, as the DSP adds it): direct form II transposed, the state in st
static void biquad(const uint8_t *b, float *st, float *x, uint32_t n) {
    const float q = 1.f / 16384, b0 = rd16s(b + 2) * q, b1 = rd16s(b + 4) * q, b2 = rd16s(b + 6) * q, a1 = rd16s(b + 8) * q, a2 = rd16s(b + 10) * q;
    float s0 = st[0], s1 = st[1];
    for (uint32_t i = 0; i < n; i++) { float in = x[i], o = in * b0 + s0; s0 = in * b1 + o * a1 + s1; s1 = in * b2 + o * a2; x[i] = o; }
    st[0] = fabsf(s0) < 1e-15f ? 0 : s0; st[1] = fabsf(s1) < 1e-15f ? 0 : s1;   // (no denormals from a filter ringing out)
}

static uint32_t mix_bufs(const aru_renderer *r, uint32_t m) { uint32_t n = rd32(r->mix + (size_t)ARU_IN_MIX * m + 8); return n > ARU_MIXBUF ? ARU_MIXBUF : n; }
static int mix_used(const aru_renderer *r, int32_t m) { return m >= 0 && (uint32_t)m <= r->submixes && r->mix_off[m] >= 0; }

// The mix buffers of each mix in use, in mix order from the first (the final mix, 0) on, each taking its buffer count; a mix that does not fit is left out
static void mix_layout(aru_renderer *r, aru_frame_report *f) {
    uint32_t off = 0;
    for (uint32_t m = 0; m <= r->submixes; m++) {
        const uint8_t *p = r->mix + (size_t)ARU_IN_MIX * m; uint32_t n = mix_bufs(r, m);
        r->mix_off[m] = -1;
        if (!p[0xc] || !n) continue;
        if (off + n > r->mix_buffers) { f->warn |= ARU_W_MIXBUF; continue; }
        r->mix_off[m] = (int32_t)off; off += n;
    }
}

// One channel of a voice (x, sample_count samples) into its destination mix: the voice volume ramped from the last frame's to this one's, then each of the channel resource's
// mix volumes ramped likewise onto the destination's buffers (Ryujinx's VolumeRamp and MixRamp commands)
static void mix_channel(aru_renderer *r, const aru_voice *v, unsigned c, float *x) {
    const uint32_t N = r->sample_count; int32_t id = v->chres[c];
    if (id < 0 || (uint32_t)id >= r->voices) return;
    const uint8_t *cr = r->chres + (size_t)ARU_IN_CHRES * id; const float *prev = r->chres_prev[id];
    float v0 = v->prev_volume, dv = (v->volume - v0) / N;
    if (v0 != 1.f || dv != 0) for (uint32_t i = 0; i < N; i++) x[i] *= v0 + dv * i;
    uint32_t off = (uint32_t)r->mix_off[v->mix_id], nb = mix_bufs(r, (uint32_t)v->mix_id);
    for (uint32_t j = 0; j < nb; j++) {
        float g0 = prev[j], g1 = rdf(cr + 4 + 4 * j);
        if (g0 == 0 && g1 == 0) continue;
        float *o = r->mixbuf + (size_t)(off + j) * N, dg = (g1 - g0) / N;
        if (dg == 0) for (uint32_t i = 0; i < N; i++) o[i] += x[i] * g0;
        else for (uint32_t i = 0; i < N; i++) o[i] += x[i] * (g0 + dg * i);
    }
}

// A started voice's frame: the samples its rate takes (the out status follows this walk alone), each channel decoded after the last frame's history, resampled to the frame,
// filtered and mixed
static void voice_play(aru_renderer *r, aru_voice *v, aru_frame_report *f) {
    const uint32_t N = r->sample_count;
    aru_play *ps = &v->ps;
    double ratio = voice_ratio(r, v), frac0 = ps->frac;
    uint32_t need = aru_frame_samples(r, v, &ps->frac), left = need;   // (the fraction the next frame starts into is carried from here)
    aru_play start = *ps, end = start;
    int to_mix = mix_used(r, v->mix_id);
    if (!to_mix && (uint32_t)v->mix_id > r->submixes && v->splitter_id != -1 && v->splitter_id != 0x7fffffff) f->warn |= ARU_W_SPLITTER;
    if (to_mix && !v->mixed) {   // its first mixed frame: nothing to ramp from
        v->prev_volume = v->volume;
        for (unsigned c = 0; c < v->chans; c++) { int32_t id = v->chres[c]; if (id >= 0 && (uint32_t)id < r->voices) for (int j = 0; j < ARU_MIXBUF; j++) r->chres_prev[id][j] = rdf(r->chres + (size_t)ARU_IN_CHRES * id + 4 + 4 * j); }
    }
    for (unsigned c = 0; c < v->chans; c++) {
        aru_play p = start; aru_chan *ch = &v->ch[c]; float *s = r->src;
        memcpy(s, ch->hist, sizeof ch->hist);
        uint32_t got = voice_take(r, v, &p, c, s + ARU_HIST, need, !c, f);
        if (got < need) memset(s + ARU_HIST + got, 0, (size_t)(need - got) * sizeof *s);
        if (!c) { end = p; left = need - got; }
        if (!got && !ch->hist[0] && !ch->hist[1] && !ch->hist[2] && !ch->hist[3] && !ch->bq[0][0] && !ch->bq[0][1] && !ch->bq[1][0] && !ch->bq[1][1]) continue;   // silent: nothing to add
        if (ratio > 0) resample(s, r->vbuf, N, frac0, ratio); else memset(r->vbuf, 0, (size_t)N * sizeof *r->vbuf);
        memcpy(ch->hist, s + need, sizeof ch->hist);
        for (int b = 0; b < 2; b++) if (v->biquad[b][0]) biquad(v->biquad[b], ch->bq[b], r->vbuf, N);
        if (to_mix) mix_channel(r, v, c, r->vbuf);
    }
    *ps = end;
    if (left) f->voices_starved++; else f->voices_played++;
    if (to_mix) {
        v->prev_volume = v->volume; v->mixed = 1; f->voices_mixed++;
        for (unsigned c = 0; c < v->chans; c++) { int32_t id = v->chres[c]; if (id >= 0 && (uint32_t)id < r->voices) for (int j = 0; j < ARU_MIXBUF; j++) r->chres_prev[id][j] = rdf(r->chres + (size_t)ARU_IN_CHRES * id + 4 + 4 * j); }
    }
}

// The effects of mix m in their processing order (an effect's order is its place in the mix's list; the list ends at the first place nothing takes). The buffer mixer adds
// input buffer × volume onto its output buffer for each of its pairs, the buffer indices counted from the mix's first buffer; the other types are not processed, which leaves
// their output buffers as the mix has them (an effect working in place passes its input through)
static void mix_effects(aru_renderer *r, uint32_t m, aru_frame_report *f) {
    const uint32_t N = r->sample_count, off = (uint32_t)r->mix_off[m];
    for (uint32_t o = 0; o < r->effects; o++) {
        aru_effect *e = NULL;
        for (uint32_t i = 0; i < r->effects; i++) if (r->effect[i].type && r->effect[i].mix_id == (int32_t)m && r->effect[i].order == o) e = &r->effect[i];
        if (!e) break;
        if (e->type != ARU_FX_BUFMIX) { if (e->type < 32) f->fx_skipped |= 1u << e->type; continue; }
        if (!e->enabled) continue;
        uint32_t n = rd32(e->param + 0x90); if (n > ARU_MIXBUF) n = ARU_MIXBUF;
        for (uint32_t k = 0; k < n; k++) {
            float g = rdf(e->param + 0x30 + 4 * k); uint32_t a = off + e->param[k], b = off + e->param[0x18 + k];
            if (g == 0 || a >= r->mix_buffers || b >= r->mix_buffers) continue;
            const float *x = r->mixbuf + (size_t)a * N; float *y = r->mixbuf + (size_t)b * N;
            for (uint32_t i = 0; i < N; i++) y[i] += x[i] * g;
        }
    }
}

// The submixes, deepest first (the most steps from the final mix along their destinations), each through its effects and then its buffers × mix volume × its 24 × 24 matrix
// (+0x24, source buffer major) onto the destination's; a submix that does not reach the final mix is left out
static void submixes(aru_renderer *r, aru_frame_report *f) {
    const uint32_t N = r->sample_count, nm = r->submixes + 1; uint32_t maxd = 0;
    uint16_t depth[257] = {0};
    for (uint32_t m = 1; m < nm; m++) {
        if (r->mix_off[m] < 0) continue;
        uint32_t d = 0, x = m;
        while (x && d <= nm) { int32_t to = (int32_t)rd32(r->mix + (size_t)ARU_IN_MIX * x + 0x924); if (!mix_used(r, to)) break; x = (uint32_t)to; d++; }
        depth[m] = x ? 0 : (uint16_t)d; if (depth[m] > maxd) maxd = depth[m];
    }
    for (uint32_t d = maxd; d >= 1; d--)
        for (uint32_t m = 1; m < nm; m++) {
            if (depth[m] != d) continue;
            mix_effects(r, m, f);
            const uint8_t *p = r->mix + (size_t)ARU_IN_MIX * m; uint32_t to = rd32(p + 0x924), ns = mix_bufs(r, m), nd = mix_bufs(r, to);
            float vol = rdf(p);
            for (uint32_t i = 0; i < ns; i++) for (uint32_t j = 0; j < nd; j++) {
                float g = vol * rdf(p + 0x24 + 4 * (i * ARU_MIXBUF + j));
                if (g == 0) continue;
                const float *x = r->mixbuf + (size_t)(r->mix_off[m] + i) * N; float *y = r->mixbuf + (size_t)(r->mix_off[to] + j) * N;
                for (uint32_t k = 0; k < N; k++) y[k] += x[k] * g;
            }
        }
}

// The final mix's buffer an input index of a sink names (its buffers count from its own first one), scaled by the final mix's volume; NULL for none
static const float *final_buf(const aru_renderer *r, uint8_t idx) {
    if (r->mix_off[0] < 0 || idx >= mix_bufs(r, 0)) return NULL;
    return r->mixbuf + (size_t)(r->mix_off[0] + idx) * r->sample_count;
}
static int16_t clamp16(float x, aru_frame_report *f) {
    float a = x < 0 ? -x : x;
    if (a > f->peak) f->peak = a > 65535.f ? 65535u : (uint32_t)a;
    if (x >= 32767.5f) { f->clipped++; return 32767; }
    if (x <= -32768.5f) { f->clipped++; return -32768; }
    return (int16_t)(x < 0 ? x - 0.5f : x + 0.5f);
}
// A circular buffer sink: each input's frame as int16 after the last, wrapping at the buffer's end (Ryujinx's CircularBufferSinkCommand); the out status reports the offset two
// frames back, as the service does
static void circular(aru_renderer *r, aru_sink *k, aru_frame_report *f) {
    const uint32_t N = r->sample_count, fsz = 2 * N * k->inputs;
    f->warn |= ARU_W_CIRCULAR;
    if (k->buf_size && mem_ok(r, k->buf_addr, k->buf_size, f)) {
        uint32_t o = k->write_off; uint8_t *d = (uint8_t *)(uintptr_t)k->buf_addr; float fv = rdf(r->mix);
        for (unsigned c = 0; c < k->inputs; c++) {
            const float *x = final_buf(r, k->input[c]);
            for (uint32_t i = 0; i < N && o + 2 <= k->buf_size; i++, o += 2) { int16_t s = clamp16(x ? x[i] * fv : 0, f); memcpy(d + o, &s, 2); }
            if (o >= k->buf_size) o = 0;
        }
    }
    k->last_written = k->old_written; k->old_written = k->write_off; k->write_off = k->buf_size ? (k->write_off + fsz) % k->buf_size : 0;
}

uint32_t aru_frame(aru_renderer *r, int16_t *out, aru_frame_report *fr) {
    aru_frame_report z = {0}, *f = fr ? fr : &z;
    memset(f, 0, sizeof *f);
    if (!r->started) return 0;
    const uint32_t N = r->sample_count;
    mix_layout(r, f);
    memset(r->mixbuf, 0, (size_t)r->mix_buffers * N * sizeof *r->mixbuf);
    for (uint32_t i = 0; i < r->voices; i++) {
        aru_voice *v = &r->voice[i]; aru_play *ps = &v->ps;
        if (!v->in_use) continue;
        v->is_new = 0;
        for (; v->flush; v->flush--) {   // FlushWaveBufferCount: the client drops the oldest queued buffers
            uint32_t k = ps->index % ARU_WB;
            v->wb[k].sent = 1; ps->valid[k] = 0; ps->index = (k + 1) % ARU_WB; ps->consumed++; f->wb_consumed++;
        }
        if (v->state == ARU_STARTED) {
            for (int k = 0; k < ARU_WB; k++) if (!v->wb[k].sent) { ps->valid[k] = 1; v->wb[k].sent = 1; }
            voice_play(r, v, f);
        } else if (v->state == ARU_STOPPING) {   // what is queued is dropped (and counted finished), and playback starts over
            for (int k = 0; k < ARU_WB; k++) {
                v->wb[k].sent = 1;
                if (ps->valid[k]) { ps->index = (ps->index + 1) % ARU_WB; ps->consumed++; f->wb_consumed++; }
                ps->valid[k] = 0;
            }
            ps->offset = 0; ps->played = 0; ps->frac = 0;
            memset(v->ch, 0, sizeof v->ch);   // the decoder, resampler and filters start from rest
            v->state = ARU_STOPPED; f->voices_stopped++;
        }
    }
    for (uint32_t i = 0; i < r->effects; i++) if (r->effect[i].type) r->effect[i].usage = r->effect[i].enabled ? ARU_FX_ENABLED : ARU_FX_DISABLED;
    submixes(r, f);
    if (r->mix_off[0] >= 0) mix_effects(r, 0, f);
    // the sinks: the first device in use is the output (front-left front-right center LFE back-left back-right; 6 → 2 with the device's own coefficients when it sends them,
    // else the system's: front 1.0, center 0.707, LFE 0.251, back 0.707, as hle_audren.c capture() takes them in Q15), a circular buffer gets the final mix's buffers
    float *st = r->stereo, fv = rdf(r->mix); int dev = 0;
    memset(st, 0, (size_t)N * 2 * sizeof *st);
    for (uint32_t i = 0; i < r->sinks; i++) {
        aru_sink *k = &r->sink[i];
        if (!k->used) continue;
        if (k->type == 2) { circular(r, k, f); continue; }
        if (k->type != 1 || dev || !k->inputs) continue;
        dev = 1;
        const float *x[ARU_CH] = {0}; float cf[4] = {1.f, 0.707f, 0.251f, 0.707f};
        for (unsigned c = 0; c < k->inputs; c++) x[c] = final_buf(r, k->input[c]);
        if (k->downmix_on) { int ok = 1; for (int c = 0; c < 4; c++) ok &= k->downmix[c] >= 0 && k->downmix[c] <= 4; if (ok) memcpy(cf, k->downmix, sizeof cf); }
        for (uint32_t s = 0; s < N; s++) {
            float y[ARU_CH];
            for (unsigned c = 0; c < ARU_CH; c++) y[c] = x[c] ? x[c][s] * fv : 0;
            if (k->inputs >= 6) { float m = y[2] * cf[1] + y[3] * cf[2]; st[2 * s] = y[0] * cf[0] + m + y[4] * cf[3]; st[2 * s + 1] = y[1] * cf[0] + m + y[5] * cf[3]; }
            else { st[2 * s] = y[0]; st[2 * s + 1] = k->inputs > 1 ? y[1] : y[0]; }
        }
    }
    uint32_t nout = r->sample_rate == 48000 ? N : up_3_2(r, st, N, st);
    if (out) for (uint32_t s = 0; s < 2 * nout; s++) out[s] = clamp16(st[s], f);
    r->frames++;
    return nout;
}

// Not the service's exact sum (that follows its internal layout, which this host does not have), but of its order: mix buffers, voices with their channel states, mixes
// with their effect lists and the node graph, effects, sinks, memory pools, splitters, performance frames and a command buffer; page aligned, as transfer memory must be
static uint64_t al(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }
uint64_t aru_work_buffer_size(const uint8_t *p) {
    uint64_t n = rd32(p + 4), mixbuf = rd32(p + 8), mixes = (uint64_t)rd32(p + 0xc) + 1, voices = rd32(p + 0x10), sinks = rd32(p + 0x14), effects = rd32(p + 0x18),
             perf = rd32(p + 0x1c), split = rd32(p + 0x24), dests = rd32(p + 0x28), nodes = mixes + voices + sinks + effects;
    uint64_t s = al((mixbuf + ARU_CH) * n * 4 * 2, 0x40);                         // mix buffers and their depop state
    s += al(voices * (0x220 + ARU_IN_CHRES + 0x100 * ARU_CH), 0x40);              // voice infos, channel resources, per channel play state
    s += al(mixes * (0x940 + effects * 4) + nodes * nodes / 8 + nodes * 0x10, 0x40);
    s += al(effects * 0x100 + sinks * 0x170 + (effects + voices * ARU_WB) * 0x20, 0x40);
    s += al(split * 0x40 + dests * (0x20 + ARU_MIXBUF * 4), 0x40);
    if (perf) s += al((perf + 1) * (0x20 + (voices + mixes + sinks + effects) * 0x10 + 0x400), 0x40);
    s += 0x18000;                                                                 // command buffer
    return al(s, 0x1000);
}
