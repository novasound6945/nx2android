// The audio renderer's update protocol, server side, for the SDKs whose renderer runs only in the audio service (audren:u): SDK 1.x links the IPC client alone, with no
// in-process renderer for hle_audren.c's CPU + manual takeover. hle_audren_srv.c answers the IPC; this part is the state the service keeps per renderer, the parsing of
// RequestUpdate's input, its output, and one audio frame of the "DSP" (wave buffers consumed at the rate playback takes them). No host dependencies: tests/audren_upd_fixture.c
// builds it on the development machine.
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

enum { ARU_WB = 4, ARU_CH = 6, ARU_MIXBUF = 24,
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

typedef struct {
    uint8_t in_use, is_new, state, fmt, chans;
    uint32_t rate, wb_count, flush;
    int32_t mix_id, splitter_id;
    float pitch, volume;
    uint64_t adpcm_coef, adpcm_coef_size;   // the voice's data source state (ADPCM coefficients) for phase 2's decoder
    int32_t chres[ARU_CH];                  // channel resource of each channel: the per-channel mix volumes (aru_renderer.chres)
    uint8_t biquad[2][12];                  // the two biquad filters as sent (enable, reserved, s16 b0 b1 b2 a1 a2 in Q14)
    aru_wb wb[ARU_WB];
    aru_play ps;
} aru_voice;

typedef struct { uint8_t type, enabled, usage; int32_t mix_id; } aru_effect;
typedef struct { uint8_t type, used, inputs, input[ARU_CH]; uint64_t buf_addr; uint32_t buf_size, last_written; } aru_sink;

typedef struct {
    // the parameter the renderer was opened with
    uint32_t sample_rate, sample_count, mix_buffers, submixes, voices, sinks, effects, perf_frames, splitters, splitter_dests, revision;
    uint8_t voice_drop;
    int started;
    aru_voice *voice; aru_effect *effect; aru_sink *sink;
    uint8_t *mix;     // each mix's last in parameter, raw (submixes + 1 of ARU_IN_MIX): +0 volume, +8 buffer count, +0xc in use, +0x24 the 24 × 24 mix volumes, +0x924 destination mix
    uint8_t *chres;   // each channel resource's last in parameter, raw (voices of ARU_IN_CHRES): +0 id, +4 the 24 mix volumes, +0x64 in use
    uint64_t frames, updates;
    uint32_t behaviour_rev; uint64_t behaviour_flags;
} aru_renderer;

typedef struct {   // what an update saw, for the caller's log
    uint32_t revision, npools, nvoices, nchres, neffects, nmixes, nsinks, splitter_bytes, out_total;
    uint32_t voices_in_use, voices_started, wb_new, pools_attached, pools_detached;
    uint32_t effect_types, sink_types;   // bit n: an effect / sink of type n in use
    const char *error;                   // why the update was refused (NULL when it was taken)
} aru_report;

typedef struct { uint32_t wb_consumed, voices_played, voices_starved, voices_stopped; } aru_frame_report;

// Opens a renderer from the service's parameter (ARU_PARAM bytes); 0 or an nn::audio result (the renderer is left closed)
uint32_t aru_open(aru_renderer *r, const uint8_t *param);
void aru_close(aru_renderer *r);
// RequestUpdate: input in[0, in_n), output into out[0, out_n); 0 or an nn::audio result. Under the caller's lock, as aru_frame
uint32_t aru_update(aru_renderer *r, const uint8_t *in, size_t in_n, uint8_t *out, size_t out_n, aru_report *rep);
// One audio frame (sample_count samples at sample_rate) of a started renderer: queued wave buffers start, playing voices take their samples, stop requests are carried out
void aru_frame(aru_renderer *r, aru_frame_report *fr);
// GetWorkBufferSize: what the service would need for this parameter (the game allocates it and hands it over as transfer memory the host does not use)
uint64_t aru_work_buffer_size(const uint8_t *param);
// Source samples a voice takes in one frame at its pitch and sample rate, carrying the fraction in *frac
uint32_t aru_frame_samples(const aru_renderer *r, const aru_voice *v, double *frac);
