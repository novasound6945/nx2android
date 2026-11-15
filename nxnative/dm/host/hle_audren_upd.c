// The audio renderer's update protocol, server side (audren_upd.h): pure state and buffers, no host calls (the IPC is hle_audren_srv.c).
// Phase 1 is silent: wave buffers are taken at the rate a real renderer plays them (each voice's pitch and sample rate against the renderer's), so a game that streams keeps
// getting its buffers back, but no PCM is produced yet. The voice, mix and sink state needed for mixing is kept as it arrives.
#include "audren_upd.h"
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static float rdf(const uint8_t *p) { float v; memcpy(&v, p, 4); return v; }
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void wr64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
// "REVn" read as a little-endian word: n, or 0 for anything else
static unsigned rev_num(uint32_t rev) { unsigned n = (rev >> 24) - '0'; return (rev & 0xffffffu) == 0x564552u && n <= 9 ? n : 0; }

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
    r->voice = calloc(r->voices ? r->voices : 1, sizeof *r->voice); r->effect = calloc(r->effects ? r->effects : 1, sizeof *r->effect);
    r->sink = calloc(r->sinks ? r->sinks : 1, sizeof *r->sink); r->mix = calloc(r->submixes + 1, ARU_IN_MIX); r->chres = calloc(r->voices ? r->voices : 1, ARU_IN_CHRES);
    if (!r->voice || !r->effect || !r->sink || !r->mix || !r->chres) { aru_close(r); return ARU_R_PARAM; }
    return 0;
}
void aru_close(aru_renderer *r) {
    free(r->voice); free(r->effect); free(r->sink); free(r->mix); free(r->chres);
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
    memset(out, 0, total);
    const uint8_t *q = in + ARU_IN_HDR; uint8_t *o = out + ARU_IN_HDR;
    // behaviour: the client's revision and flags (memory pool error checks); no errors to report back (its output section stays zero)
    r->behaviour_rev = rd32(q); r->behaviour_flags = rd64(q + 8); q += bsz;
    // memory pools (+0 address, +8 size, +0x10 the client's state): a requested attach is attached, a requested detach detached; any other state is left as it is (0)
    for (uint32_t i = 0; i < np; i++) {
        uint32_t st = rd32(q + ARU_IN_POOL * i + 0x10);
        if (st == 4) { wr32(o + ARU_OUT_POOL * i, 5); rep->pools_attached++; }
        else if (st == 2) { wr32(o + ARU_OUT_POOL * i, 3); rep->pools_detached++; }
    }
    q += psz; o += np * ARU_OUT_POOL;
    // voice channel resources: kept by their ID for the mixing to come
    for (uint32_t i = 0; i < nc; i++) { const uint8_t *c = q + ARU_IN_CHRES * i; uint32_t id = rd32(c); if (id < r->voices) memcpy(r->chres + (size_t)ARU_IN_CHRES * id, c, ARU_IN_CHRES); }
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
    // effects (+0 type, +1 new, +2 enabled, +4 mix): state 3 enabled / 4 disabled, as the server reports them (disabled while the renderer is stopped)
    for (uint32_t i = 0; i < ne; i++) {
        const uint8_t *e = q + ARU_IN_EFFECT * i; aru_effect *x = &r->effect[i];
        if (e[0] != x->type) { x->type = e[0]; x->usage = ARU_FX_INVALID; }
        if (e[1]) x->usage = ARU_FX_NEW;
        x->enabled = e[2]; x->mix_id = (int32_t)rd32(e + 4);
        o[ARU_OUT_EFFECT * i] = r->started && x->usage != ARU_FX_DISABLED ? 3 : 4;
        if (x->type && x->type < 32) rep->effect_types |= 1u << x->type;
    }
    q += esz; o += ne * ARU_OUT_EFFECT;
    q += spsz;   // splitter: not routed yet
    for (uint32_t i = 0; i < nm; i++) memcpy(r->mix + (size_t)ARU_IN_MIX * i, q + (size_t)ARU_IN_MIX * i, ARU_IN_MIX);
    q += msz;
    // sinks (+0 type 1 device / 2 circular buffer, +1 in use, +0x20 the type's parameter: a device's input count at +0x120 and inputs at +0x124)
    for (uint32_t i = 0; i < ns; i++) {
        const uint8_t *s = q + ARU_IN_SINK * i; aru_sink *k = &r->sink[i];
        k->type = s[0]; k->used = s[1];
        if (k->type == 1) { uint32_t n = rd32(s + 0x120); k->inputs = n > ARU_CH ? ARU_CH : (uint8_t)n; memcpy(k->input, s + 0x124, ARU_CH); }
        else if (k->type == 2) { k->buf_addr = rd64(s + 0x20); k->buf_size = rd32(s + 0x28); }
        if (k->type == 2) wr32(o + ARU_OUT_SINK * i, k->last_written);
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

uint32_t aru_frame_samples(const aru_renderer *r, const aru_voice *v, double *frac) {
    double ratio = r->sample_rate ? (double)v->rate / r->sample_rate * v->pitch : 0;
    if (!(ratio > 0)) return 0;   // pitch 0 (or not a number): the voice stands still
    if (ratio > 64) ratio = 64;
    double t = *frac + ratio * r->sample_count;
    uint32_t n = (uint32_t)t; *frac = t - n;
    return n;
}

// A started voice takes its frame's samples from the queued wave buffers in order. A buffer played to its end is finished (the out status counts it, the client takes it back),
// unless it loops: WaveBuffer version 1 (REV1-REV4) loops over [start, end) until the voice stops. An end-of-stream buffer resets the played sample count when it finishes.
// With nothing queued the voice starves (plays silence) and stays started
static void voice_consume(const aru_renderer *r, aru_voice *v, aru_frame_report *fr) {
    aru_play *ps = &v->ps;
    uint32_t need = aru_frame_samples(r, v, &ps->frac);
    for (int guard = 0; need && guard <= ARU_WB; guard++) {   // each turn finishes a buffer or takes all that is left
        uint32_t k = ps->index % ARU_WB;
        if (!ps->valid[k]) break;
        aru_wb *w = &v->wb[k];
        uint32_t len = w->end > w->start ? w->end - w->start : 0;
        if (w->loop && len) { ps->offset = (uint32_t)(((uint64_t)ps->offset + need) % len); ps->played += need; need = 0; break; }
        uint32_t avail = len > ps->offset ? len - ps->offset : 0, take = need < avail ? need : avail;
        ps->offset += take; ps->played += take; need -= take;
        if (ps->offset >= len) {
            ps->offset = 0; ps->valid[k] = 0; ps->index = (k + 1) % ARU_WB; ps->consumed++; fr->wb_consumed++;
            if (w->eos) ps->played = 0;
        }
    }
    if (need) fr->voices_starved++; else fr->voices_played++;
}

void aru_frame(aru_renderer *r, aru_frame_report *fr) {
    aru_frame_report z = {0}, *f = fr ? fr : &z;
    *f = z;
    if (!r->started) return;
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
            voice_consume(r, v, f);
        } else if (v->state == ARU_STOPPING) {   // what is queued is dropped (and counted finished), and playback starts over
            for (int k = 0; k < ARU_WB; k++) {
                v->wb[k].sent = 1;
                if (ps->valid[k]) { ps->index = (ps->index + 1) % ARU_WB; ps->consumed++; f->wb_consumed++; }
                ps->valid[k] = 0;
            }
            ps->offset = 0; ps->played = 0; ps->frac = 0;
            v->state = ARU_STOPPED; f->voices_stopped++;
        }
    }
    for (uint32_t i = 0; i < r->effects; i++) if (r->effect[i].type) r->effect[i].usage = r->effect[i].enabled ? ARU_FX_ENABLED : ARU_FX_DISABLED;
    r->frames++;
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
