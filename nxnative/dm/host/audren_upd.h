// The audio renderer's update protocol, server side, for the SDKs whose renderer runs only in the audio service (audren:u): SDK 1.x links the IPC client alone, with no
// in-process renderer for hle_audren.c's CPU + manual takeover. hle_audren_srv.c answers the IPC; this part is the state the service keeps per renderer, the parsing of
// RequestUpdate's input, its output, and one audio frame of the "DSP": wave buffers decoded (PCM16 / PCM8 / float / DSP-ADPCM) at the rate playback takes them, resampled,
// filtered, mixed through the submixes into the final mix and out of the device sink as stereo int16. No host dependencies: tests/audren_upd_fixture.c builds it on the
// development machine.
// Formats (REV1-REV4: one layout; REV2 is what SDK 1.7.1 sends), read off the SDK's own client (nn::audio::RequestUpdateAudioRenderer and the managers' UpdateInParameter /
// UpdateOutStatus) and checked against Ryujinx's server:
//   input:  header 0x40 {u32 revision "REVn", sizes: +4 behaviour, +8 memory pools, +0xc voices, +0x10 voice channel resources, +0x14 effects, +0x18 mixes, +0x1c sinks,
//           +0x20 performance, +0x24 splitter, +0x3c total}, then behaviour (0x10), pools (0x20 each), channel resources (0x70), voices (0x170), effects (0xc0), splitter,
//           mixes (0x930), sinks (0x140), performance (0x10)
//   output: header 0x40 with the same size fields, then pools (0x10 each: u32 state), voices (0x10: u64 played samples, u32 wave buffers consumed, u8 voice dropped),
//           effects (0x10: u8 state), sinks (0x20: u32 last written offset), performance (0x10: u32 bytes written), behaviour (0xb0: error infos); +0x3c total, which the
//           client checks against its output buffer's size (a mismatch aborts there)
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { ARU_WB = 4, ARU_CH = 6, ARU_MIXBUF = 24, ARU_HIST = 4, ARU_UP_TAPS = 16,
       ARU_OUT_MAX = 1536,   // output frames of one audio frame at most: 1024 samples at 48 kHz, or 3/2 of them for a 32 kHz renderer
       ARU_IN_HDR = 0x40, ARU_IN_BEHAVIOUR = 0x10, ARU_IN_POOL = 0x20, ARU_IN_CHRES = 0x70, ARU_IN_VOICE = 0x170, ARU_IN_EFFECT = 0xc0, ARU_IN_MIX = 0x930,
       ARU_IN_SINK = 0x140, ARU_IN_PERF = 0x10,
       ARU_OUT_POOL = 0x10, ARU_OUT_VOICE = 0x10, ARU_OUT_EFFECT = 0x10, ARU_OUT_SINK = 0x20, ARU_OUT_PERF = 0x10, ARU_OUT_BEHAVIOUR = 0xb0,
       ARU_PARAM = 0x34 };   // the service's AudioRendererParameter (IPC raw data of OpenAudioRenderer / GetWorkBufferSize)
// nn::audio results (module 153): an update the service cannot take, a revision it does not know
enum { ARU_R_INVALID_UPDATE = 0x5299u, ARU_R_REVISION = 0x499u, ARU_R_PARAM = 0x1099u };
// voice play state on the server: the user's Started / Stopped / Paused, and Stopping (a stop request the next frame carries out: it drops the queued wave buffers)
enum { ARU_STARTED = 0, ARU_STOPPED = 1, ARU_PAUSED = 2, ARU_STOPPING = 3 };
// effect usage state (what the out status reports follows it): set by the frames from the effect's enabled flag
enum { ARU_FX_INVALID = 0, ARU_FX_NEW = 1, ARU_FX_ENABLED = 2, ARU_FX_DISABLED = 3 };
// sample formats (the voice's +0xb) and effect types (the effect's +0)
enum { ARU_PCM8 = 1, ARU_PCM16 = 2, ARU_PCMF = 5, ARU_ADPCM = 6 };
enum { ARU_FX_BUFMIX = 1, ARU_FX_AUX = 2, ARU_FX_DELAY = 3, ARU_FX_REVERB = 4, ARU_FX_REVERB3D = 5, ARU_FX_BIQUAD = 6 };
// what a frame met that this renderer leaves out (aru_frame_report.warn), for the caller's one-time log lines
enum { ARU_W_FORMAT = 1, ARU_W_SPLITTER = 2, ARU_W_MIXBUF = 4, ARU_W_CIRCULAR = 8 };

typedef struct {   // a wave buffer as the voice last received it (WaveBufferInternal: +0 address, +8 size, +0x10 / +0x14 start / end sample offsets, +0x18 loop, +0x19 end of stream,
                   // +0x1a sent to the server, +0x20 / +0x28 ADPCM context address / size)
    uint64_t addr, size, ctx, ctx_size;
    uint32_t start, end;
    uint8_t loop, eos;
    uint8_t sent;   // handed to the frames (IsSendToAudioProcessor): a buffer that arrived in an update becomes playable at the next frame of a started voice
} aru_wb;

typedef struct {   // where playback stands (the DSP's VoiceUpdateState; hardware keeps one per channel resource, all channels of a voice in step: one per voice here)
    uint8_t valid[ARU_WB];   // queued for playback
    uint32_t index;          // the wave buffer playing
    uint32_t consumed;       // wave buffers finished since the voice was new (the client's GetReleasedWaveBuffer compares this with what it took back)
    uint64_t played;         // source samples played since new, the last stop or the last end-of-stream buffer
    uint32_t offset;         // samples into the playing buffer
    double frac;             // the part of a source sample the next frame starts into
} aru_play;

typedef struct {   // one channel's own state (per channel resource on hardware): what the frames carry from one to the next
    float hist[ARU_HIST];   // the last source samples of the previous frame: the resampler's first taps
    float bq[2][2];         // the two biquad filters' state
    int16_t ps, h0, h1;     // DSP-ADPCM: predictor / scale of the frame being decoded and the last two samples (the wave buffer's context has the same three at its start)
} aru_chan;

typedef struct {
    uint8_t in_use, is_new, state, fmt, chans;
    uint8_t mixed;                          // mixed since it was new: until then there is no last frame to ramp from (the first frame starts at the voice's own volumes)
    uint32_t rate, wb_count, flush;
    int32_t mix_id, splitter_id;
    float pitch, volume, prev_volume;       // prev_volume: what the last mixed frame ended on (the volume ramps from it to volume over a frame)
    uint64_t adpcm_coef, adpcm_coef_size;   // the voice's data source state: the ADPCM coefficients (8 pairs of s16)
    int32_t chres[ARU_CH];                  // channel resource of each channel: the per-channel mix volumes (aru_renderer.chres)
    uint8_t biquad[2][12];                  // the two biquad filters as sent (enable, reserved, s16 b0 b1 b2 a1 a2 in Q14)
    aru_wb wb[ARU_WB];
    aru_play ps;
    aru_chan ch[ARU_CH];
} aru_voice;

typedef struct { uint8_t type, enabled, usage; int32_t mix_id; uint32_t order; uint8_t param[0xa0]; } aru_effect;   // param: the type's parameter as sent (+0x20)
typedef struct {
    uint8_t type, used, inputs, input[ARU_CH];
    uint8_t downmix_on; float downmix[4];   // a device's own 6 → 2 coefficients (front, center, LFE, back), when it sends them
    uint64_t buf_addr; uint32_t buf_size, last_written, old_written, write_off;   // a circular buffer: where the frames write and what the out status reports (two frames behind)
} aru_sink;
typedef struct { uint64_t addr, size; uint8_t attached; } aru_pool;

typedef struct {
    // the parameter the renderer was opened with
    uint32_t sample_rate, sample_count, mix_buffers, submixes, voices, sinks, effects, perf_frames, splitters, splitter_dests, revision;
    uint8_t voice_drop;
    int started;
    aru_voice *voice; aru_effect *effect; aru_sink *sink;
    uint8_t *mix;     // each mix's last in parameter, raw (submixes + 1 of ARU_IN_MIX): +0 volume, +8 buffer count, +0xc in use, +0x24 the 24 × 24 mix volumes, +0x924 destination mix
    uint8_t *chres;   // each channel resource's last in parameter by its place in the section, raw (voices of ARU_IN_CHRES): +0 id, +4 the 24 mix volumes, +0x64 in use
    float (*chres_prev)[ARU_MIXBUF];   // each channel resource's mix volumes as the last mixed frame ended (the mix ramps from them)
    aru_pool *pool; uint32_t npool, pool_hit;   // memory pools by their index in the updates: the "DSP" reads guest memory only inside attached ones (or with force mapping)
    float *mixbuf;    // the mix buffers of one frame (mix_buffers × sample_count, in int16 scale)
    float *src;       // one channel's source samples of a frame after the history (ARU_HIST + the most a frame takes)
    float *vbuf;      // one channel's resampled frame (sample_count)
    float *stereo;    // the device sink's stereo frame, room for the 3:2 upsampled one
    int32_t *mix_off; // each mix's first mix buffer this frame (-1: not used or no room)
    float up_buf[2 * (ARU_UP_TAPS - 1 + 1024)]; uint32_t up_pos;   // a 32 kHz renderer's 3:2 upsampler: the last input samples and the next output's position
    uint64_t frames, updates;
    uint32_t behaviour_rev; uint64_t behaviour_flags;
} aru_renderer;

typedef struct {   // what an update saw, for the caller's log
    uint32_t revision, npools, nvoices, nchres, neffects, nmixes, nsinks, splitter_bytes, out_total;
    uint32_t voices_in_use, voices_started, wb_new, pools_attached, pools_detached;
    uint32_t effect_types, sink_types;   // bit n: an effect / sink of type n in use
    const char *error;                   // why the update was refused (NULL when it was taken)
} aru_report;

typedef struct {
    uint32_t wb_consumed, voices_played, voices_starved, voices_stopped;
    uint32_t voices_mixed;          // voices that reached a mix this frame
    uint32_t unmapped;              // wave buffer, coefficient or context reads refused: outside every attached memory pool (silence in their place)
    uint64_t unmapped_addr;         // the first such address
    uint32_t peak, clipped;         // the output's largest magnitude before clamping, and the samples clamped to int16
    uint32_t warn, fx_skipped;      // ARU_W_* met this frame; bit n: an effect of type n in use that is not processed (its buffers pass through)
} aru_frame_report;

// Opens a renderer from the service's parameter (ARU_PARAM bytes); 0 or an nn::audio result (the renderer is left closed)
uint32_t aru_open(aru_renderer *r, const uint8_t *param);
void aru_close(aru_renderer *r);
// RequestUpdate: input in[0, in_n), output into out[0, out_n); 0 or an nn::audio result. Under the caller's lock, as aru_frame
uint32_t aru_update(aru_renderer *r, const uint8_t *in, size_t in_n, uint8_t *out, size_t out_n, aru_report *rep);
// One audio frame (sample_count samples at sample_rate) of a started renderer: queued wave buffers start, playing voices take their samples (decoded, resampled, filtered and
// mixed), stop requests are carried out, the submixes go into the final mix and the device sink's inputs come out as stereo int16 at 48 kHz into out (ARU_OUT_MAX frames of
// room; NULL: mixed but not written). Returns the frames written: sample_count, 3/2 of it for a 32 kHz renderer, 0 while the renderer is stopped (nothing happens then)
uint32_t aru_frame(aru_renderer *r, int16_t *out, aru_frame_report *fr);
// Whether the frames may read guest memory [a, a + n): inside an attached memory pool, or force mapping on (for the caller's diagnostics)
int aru_readable(aru_renderer *r, uint64_t a, uint64_t n);
// GetWorkBufferSize: what the service would need for this parameter (the game allocates it and hands it over as transfer memory the host does not use)
uint64_t aru_work_buffer_size(const uint8_t *param);
// Source samples a voice takes in one frame at its pitch and sample rate, carrying the fraction in *frac
uint32_t aru_frame_samples(const aru_renderer *r, const aru_voice *v, double *frac);
