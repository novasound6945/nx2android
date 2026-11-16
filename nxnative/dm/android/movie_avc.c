// Android port: H.264 for the SDK's movie library (dm/host/hle_movie_avc.h) on MediaCodec, VideoToolbox's counterpart (dm/host/hle_movie_avc.c).
// Unpacking's only movie is its 5 s Humble Games logo (StreamingAssets/HumbleGames_LogoIntro-2021_5sec_1080p30.mp4, H.264 Main 1080p, no audio), which Unity 2019.4 plays through
// the SDK's decoder in CPU mode (NV12 out); with no pictures the intro ended at once and the game crashed right after it (main+0x8a74d0).
// The decoder is configured from SPS / PPS (csd-0 / csd-1 with start codes, as Android's own movie players pass them) and fed one access unit per call, in Annex B.
// MediaCodec hands out pictures in presentation order, a few frames after their input (the hardware decoder's pipeline): mavc_take waits briefly for one while inputs are in flight,
// and at the end of the stream sends an end-of-stream input and collects every picture still in the codec. Pictures leave the codec as compact NV12 copies (the output buffer's stride,
// slice height and crop come from its format: Qualcomm's decoder pads rows to 128 bytes and planes to 32 rows), so a buffer goes back to the codec at once
// VP9 goes through the same decoder object when the device has a hardware VP9 decoder (mavc_new_vp9): The Legend of Zelda: Link's Awakening's opening and title movies are
// VP9 1080p at 60 fps, which libvpx (4 threads) decoded at 11 ms a packet on a Retroid Pocket 6, beside the game's own threads (the game ran the movies at 35-40 fps).
// A VP9 packet is one WebM block, a superframe or a single frame, given to the codec as it is; a hidden frame (an alt-ref outside a superframe) has no picture, so an input older
// than a picture that came out is counted as consumed (VP9 has no reordering: pictures leave in the order the packets went in)
#include "dm.h"
#include "hle_movie_avc.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#define QMAX 16
#define COLOR_YUV420_PLANAR 19   // I420; all other formats retain the existing bounded NV12 fallback
struct mavc {
    uint8_t sps[512], pps[512]; int nsps, npps, dirty;
    AMediaCodec *mc; int eos_sent;
    int fw, fh, fstride, fslice, fcolor, cl, ct, cr, cb;   // output format: size, stride, slice height, colour format, crop rectangle (inclusive)
    long nin, nout;                                        // inputs queued and pictures out since the codec started (or was flushed)
    struct { uint8_t *nv12; int w, h; int64_t pts; } q[QMAX]; int nq;
    uint8_t *au; size_t aucap;   // Annex B copy of the access unit being decoded
    int unknown_format_logged, dropped_picture_logged; // once per decoder, independently of other errors
    int red, rgba;               // rgba: pictures as 4-byte RGBA (native texture mode) instead of NV12
    // The codec's timestamps: the game's own, each mapped back to the game's on output (pmap). 13 Sentinels: Aegis Rim sends every packet of its 1024x576 movies with time 0,
    // and c2.qti.avc.decoder gave no picture for 81 packets and then took no more input. From the first packet whose time repeats one still in the codec, the times are made up:
    // the game's while they increase, else the last one + 1 µs, and the codec's own output order stands (synth: the queue is not sorted).
    // Times that only go back are real ones: a stream with B-frames is sent in decode order, each packet with its presentation time (Disgaea 5's opening movie, H.264 Main, I P B B B:
    // 16666, 83333, 33333, 50000, 66666, 150000, 100000 …). Made-up increasing times put each P-frame's time below its B-frames', and the codec framework's output reordering, which
    // sorts the pictures it holds by time, then showed the P-frame before them (a picture from ahead, then the three before it) whenever several were held at once: 20 to 100 pictures
    // in 600 went back in time, "previous and next frames mixed". Real times keep them in presentation order, and the queue stays sorted by them
    int64_t last_in; int synth; struct { int64_t c, g; } pmap[64]; int npmap;
    int64_t last_out;                   // the latest time of a picture handed out (mavc_take)
    long back_out, none; int nq_max;    // mavc_stats: pictures handed out with a time before one handed out already, takes that found none while inputs were in the codec, the deepest queue
    long ntr_in, ntr_out;        // DM_MOVIE_TRACE: the first inputs and pictures, one line each
    int vp9;                     // a VP9 decoder (mavc_new_vp9): packets go in as they are, through the hardware decoder found by vp9_probe
    long hidden;                 // VP9 inputs that gave no picture (hidden frames), counted when a later picture came out
    int crange, cstd;            // the output format's colour range (1 full, 2 limited) and standard (1 BT.709, 2 / 4 BT.601), 0 when it gives none; used for VP9 only
};
static const char *codec_name(const mavc *a) { return a->vp9 ? "VP9" : "AVC"; }
static int avc_trace(void) { static int on = -1; if (on < 0) on = getenv("DM_MOVIE_TRACE") != NULL; return on; }

mavc *mavc_new(void) { mavc *a = calloc(1, sizeof *a); if (!a) die("movie: AVC decoder allocation failed"); a->last_in = a->last_out = INT64_MIN; return a; }
// The device's VP9 decoder, looked up once: the codec MediaCodec picks for the type, kept only when it is not Android's software one (c2.android / OMX.google: libvpx again, out of
// process, so nothing to gain over the host's own libvpx). DM_MOVIE_VP9_HW=0 keeps every VP9 movie on libvpx
static char g_vp9_name[128]; static int g_vp9_ok;
static void vp9_probe(void) {
    const char *e = getenv("DM_MOVIE_VP9_HW");
    if (e && !atoi(e)) { LOG("[movie] VP9: DM_MOVIE_VP9_HW=0, libvpx decodes"); return; }
    AMediaCodec *mc = AMediaCodec_createDecoderByType("video/x-vnd.on2.vp9");
    if (!mc) { LOG("[movie] VP9: MediaCodec has no decoder, libvpx decodes"); return; }
    char *name = NULL; AMediaCodec_getName(mc, &name);
    int soft = !name || !strncmp(name, "c2.android.", 11) || !strncmp(name, "OMX.google.", 11);
    if (name) { snprintf(g_vp9_name, sizeof g_vp9_name, "%s", name); AMediaCodec_releaseName(mc, name); }
    AMediaCodec_delete(mc);
    g_vp9_ok = !soft;
    LOG("[movie] VP9: MediaCodec's decoder is %s%s", g_vp9_name[0] ? g_vp9_name : "?", soft ? " (software): libvpx decodes" : " (hardware): VP9 movies decode on it");
}
mavc *mavc_new_vp9(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, vp9_probe);
    if (!g_vp9_ok) return NULL;
    mavc *a = mavc_new(); a->vp9 = 1; return a;
}
static void close_codec(mavc *a) { if (a->mc) { AMediaCodec_stop(a->mc); AMediaCodec_delete(a->mc); a->mc = NULL; } a->nin = a->nout = 0; a->eos_sent = 0; a->last_in = a->last_out = INT64_MIN; a->npmap = 0; }
void mavc_drop(mavc *a) {
    for (int i = 0; i < a->nq; i++) free(a->q[i].nv12);
    a->nq = 0;
    if (a->mc) { AMediaCodec_flush(a->mc); a->nin = a->nout = 0; a->eos_sent = 0; a->last_in = a->last_out = INT64_MIN; a->npmap = 0; }   // a seek: pictures in the codec belong to the old position
}
void mavc_free(mavc *a) { if (!a) return; mavc_drop(a); close_codec(a); free(a->au); free(a); }
int mavc_queued(const mavc *a) { return a->nq; }
void mavc_stats(mavc *a, long *back, long *none, int *nq_max) { *back = a->back_out; *none = a->none; *nq_max = a->nq_max; a->nq_max = 0; }
int mavc_held(const mavc *a) { return a->nq + (a->mc && !a->eos_sent && a->nin > a->nout ? (int)(a->nin - a->nout) : 0); }
void mavc_set_rgba(mavc *a) { a->rgba = 1; }

static void keep_ps(mavc *a, const uint8_t *nal, size_t n) {   // a parameter set NAL unit (without start code)
    int t = nal[0] & 31; uint8_t *dst = t == 7 ? a->sps : a->pps; int *dn = t == 7 ? &a->nsps : &a->npps;
    if (n > sizeof a->sps) { if (!a->red++) LOG("[RED] movie: AVC parameter set of %zu bytes, too long", n); return; }
    if ((size_t)*dn == n && !memcmp(dst, nal, n)) return;
    memcpy(dst, nal, n); *dn = (int)n; a->dirty = 1;
}
// walk the NAL units of p (Annex B start codes, or else 4-byte lengths); fn gets each unit without its prefix (as hle_movie_avc.c)
static size_t next_sc(const uint8_t *p, size_t n, size_t i) { for (; i + 3 <= n; i++) if (!p[i] && !p[i + 1] && p[i + 2] == 1) return i; return n; }
static void each_nal(const uint8_t *p, size_t n, void (*fn)(void *, const uint8_t *, size_t), void *ctx) {
    size_t s = next_sc(p, n, 0);
    if (s < n && s <= 1) {
        while (s < n) {
            size_t b = s + 3, e = next_sc(p, n, b), end = e;
            while (end > b && !p[end - 1]) end--;
            if (end > b) fn(ctx, p + b, end - b);
            s = e;
        }
    } else for (size_t i = 0; i + 4 <= n;) {
        size_t len = (size_t)p[i] << 24 | (size_t)p[i + 1] << 16 | (size_t)p[i + 2] << 8 | p[i + 3]; i += 4;
        if (!len || len > n - i) break;
        fn(ctx, p + i, len); i += len;
    }
}
static void ps_only(void *ctx, const uint8_t *nal, size_t n) { int t = nal[0] & 31; if (t == 7 || t == 8) keep_ps(ctx, nal, n); }
void mavc_param_sets(mavc *a, const uint8_t *p, size_t n) { each_nal(p, n, ps_only, a); }
void mavc_avcc(mavc *a, const uint8_t *p, size_t n) {   // AVCDecoderConfigurationRecord (the MP4 avcC box)
    if (n < 7 || p[0] != 1) { if (!a->red++) LOG("[RED] movie: unrecognized avcC record (%zu bytes, version %d)", n, n ? p[0] : -1); return; }
    size_t i = 5;
    for (int set = 0; set < 2 && i < n; set++) {
        int cnt = set ? p[i] : p[i] & 31; i++;
        for (int k = 0; k < cnt && i + 2 <= n; k++) { size_t len = (size_t)p[i] << 8 | p[i + 1]; i += 2; if (len > n - i) return; if (len) keep_ps(a, p + i, len); i += len; }
    }
}

static void set_csd(AMediaFormat *f, const char *key, const uint8_t *nal, int n) {
    uint8_t b[4 + 512] = {0, 0, 0, 1}; memcpy(b + 4, nal, (size_t)n); AMediaFormat_setBuffer(f, key, b, (size_t)n + 4);
}
static int open_vp9(mavc *a) {   // the hardware VP9 decoder (vp9_probe), configured by size alone: VP9 has no codec-specific data, the key frame carries the size
    a->mc = AMediaCodec_createCodecByName(g_vp9_name);
    if (!a->mc) { if (!a->red++) LOG("[RED] movie: MediaCodec could not make the VP9 decoder %s, outputting black frames", g_vp9_name); return 0; }
    AMediaFormat *f = AMediaFormat_new();
    AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "video/x-vnd.on2.vp9");
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, 1920); AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, 1080);   // the stream's own size comes as an output format change
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE, 4 << 20);
    media_status_t st = AMediaCodec_configure(a->mc, f, NULL, NULL, 0);
    if (st == AMEDIA_OK) st = AMediaCodec_start(a->mc);
    AMediaFormat_delete(f);
    if (st != AMEDIA_OK) { if (!a->red++) LOG("[RED] movie: MediaCodec VP9 decoder %s refused its configuration (%d), outputting black frames", g_vp9_name, (int)st); AMediaCodec_delete(a->mc); a->mc = NULL; return 0; }
    LOG("[movie] VP9: MediaCodec decoder %s", g_vp9_name);
    a->fw = a->fh = 0; a->dirty = 0; return 1;
}
static int open_codec(mavc *a) {
    close_codec(a);
    if (a->vp9) return open_vp9(a);
    if (!a->nsps || !a->npps) { if (!a->red++) LOG("[RED] movie: AVC has no SPS / PPS, outputting black frames"); return 0; }
    a->mc = AMediaCodec_createDecoderByType("video/avc");
    if (!a->mc) { if (!a->red++) LOG("[RED] movie: MediaCodec has no AVC decoder, outputting black frames"); return 0; }
    AMediaFormat *f = AMediaFormat_new();
    AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, 1920); AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, 1080);   // the decoder takes the real size from the SPS (an output format change)
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE, 4 << 20);
    set_csd(f, AMEDIAFORMAT_KEY_CSD_0, a->sps, a->nsps); set_csd(f, AMEDIAFORMAT_KEY_CSD_1, a->pps, a->npps);
    media_status_t st = AMediaCodec_configure(a->mc, f, NULL, NULL, 0);
    if (st == AMEDIA_OK) st = AMediaCodec_start(a->mc);
    AMediaFormat_delete(f);
    if (st != AMEDIA_OK) { if (!a->red++) LOG("[RED] movie: MediaCodec AVC decoder refused its configuration (%d), outputting black frames", (int)st); AMediaCodec_delete(a->mc); a->mc = NULL; return 0; }
    char *name = NULL; AMediaCodec_getName(a->mc, &name);
    LOG("[movie] AVC: MediaCodec decoder %s, profile %d", name ? name : "?", a->nsps > 1 ? a->sps[1] : 0);
    if (name) AMediaCodec_releaseName(a->mc, name);
    a->fw = a->fh = 0; a->dirty = 0; return 1;
}
static void read_format(mavc *a) {
    AMediaFormat *f = AMediaCodec_getOutputFormat(a->mc); int32_t v;
    a->fw = AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_WIDTH, &v) ? v : 0; a->fh = AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_HEIGHT, &v) ? v : 0;
    a->fstride = AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_STRIDE, &v) && v >= a->fw ? v : a->fw;
    a->fslice = AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_SLICE_HEIGHT, &v) && v >= a->fh ? v : a->fh;
    a->fcolor = AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, &v) ? v : 0;
    int32_t l, t, r, b;
    if (AMediaFormat_getRect(f, AMEDIAFORMAT_KEY_DISPLAY_CROP, &l, &t, &r, &b) && l >= 0 && t >= 0 && r >= l && b >= t && r < a->fw && b < a->fh) { a->cl = l; a->ct = t; a->cr = r; a->cb = b; }
    else { a->cl = a->ct = 0; a->cr = a->fw > 0 ? a->fw - 1 : -1; a->cb = a->fh > 0 ? a->fh - 1 : -1; }
    a->crange = AMediaFormat_getInt32(f, "color-range", &v) ? v : 0; a->cstd = AMediaFormat_getInt32(f, "color-standard", &v) ? v : 0;   // (string keys: the constants are API 28)
    LOG("[movie] %s: output %dx%d stride %d slice height %d colour format %#x crop %d,%d-%d,%d, colour range %d standard %d", codec_name(a), a->fw, a->fh, a->fstride, a->fslice, a->fcolor,
        a->cl, a->ct, a->cr, a->cb, a->crange, a->cstd);
    AMediaFormat_delete(f);
}
// one output buffer → a compact NV12 picture of the crop rectangle, queued in presentation order
static void keep_picture(mavc *a, const uint8_t *src, size_t n, int64_t pts) {
    // Existing Android-parity copy_picture (nxnative 1f01e28c): validate
    // the layout and last byte actually read before copying any planes.
    int cw=a->fw, ch=a->fh, stride=a->fstride, slice=a->fslice;
    int cf=a->fcolor;
    if (cf != COLOR_YUV420_PLANAR) {
        if (cf != 21 && cf != 0x7fa30c04 && !a->unknown_format_logged) {
            a->unknown_format_logged = 1;
            LOG("[RED] movie: MediaCodec AVC output colour format %#x, using NV12 fallback", cf);
        }
        cf = 21; // keep the existing NV12 interpretation, including Qualcomm Venus
    }
    int l=a->cl, t=a->ct, r=a->cr, b=a->cb;
    if (cw <= 0 || ch <= 0 || stride < cw || slice < ch || stride > 32768 || slice > 32768 ||
        l < 0 || t < 0 || r < l || b < t || r >= cw || b >= ch || ((l | t) & 1)) {
        if (!a->dropped_picture_logged) {
            a->dropped_picture_logged = 1;
            LOG("[RED] movie: AVC picture dropped: invalid layout, %zu bytes, %dx%d, stride %d, slice height %d, crop %d,%d-%d,%d", n, cw, ch, stride, slice, l, t, r, b);
        }
        return;
    }
    int w = r-l+1, h = b-t+1, uvw = (w+1)/2, uvh = (h+1)/2;
    size_t ysize = (size_t)stride*slice, cs = cf == 19 ? (stride+1)/2 : stride;
    size_t cbase = ysize + (size_t)(t/2)*cs + (cf == 19 ? l/2 : l);
    size_t vbase = cf == 19 ? cbase + cs*((slice+1)/2) : cbase+1;
    size_t step = cf == 19 ? 1 : 2;
    if ((size_t)b * stride + r >= n || vbase + (uvh-1)*cs + (uvw-1)*step >= n) {
        if (!a->dropped_picture_logged) {
            a->dropped_picture_logged = 1;
            LOG("[RED] movie: AVC picture dropped: short buffer, %zu bytes, %dx%d, stride %d, slice height %d, crop %d,%d-%d,%d", n, cw, ch, stride, slice, l, t, r, b);
        }
        return;
    }
    uint8_t *o = malloc((size_t)w*h + (size_t)uvw*2*uvh);
    if (!o) die("movie: AVC picture allocation failed");
    for (int y = 0; y < h; y++) memcpy(o + (size_t)y*w, src + (size_t)(t+y)*stride + l, w);
    uint8_t *uv = o + (size_t)w*h;
    for (int y = 0; y < uvh; y++) for (int x = 0; x < uvw; x++) {
        size_t at = (size_t)y*cs + x*step;
        *uv++ = src[cbase+at]; *uv++ = src[vbase+at];
    }
    if (a->nq == QMAX) { if (!a->red++) LOG("[RED] movie: AVC picture queue full, dropping the oldest picture"); free(a->q[0].nv12); memmove(a->q, a->q + 1, --a->nq * sizeof a->q[0]); }
    int64_t cpts = pts;
    for (int m = 0; m < a->npmap; m++) if (a->pmap[m].c == pts) { pts = a->pmap[m].g; a->pmap[m] = a->pmap[--a->npmap]; break; }   // the game's own timestamp
    if (a->vp9) {   // inputs older than this picture that are still listed gave none (hidden frames): no longer in the codec's pipeline (mavc_held, mavc_take's wait)
        int k = 0; for (int m = 0; m < a->npmap;) if (a->pmap[m].c < cpts) { a->pmap[m] = a->pmap[--a->npmap]; k++; } else m++;
        if (k) { a->nin -= k; if (!a->hidden) LOG("[movie] VP9: an input gave no picture (a hidden frame), first case before codec time %lld", (long long)cpts); a->hidden += k; }
    }
    if (avc_trace() && a->ntr_out++ < 120) LOG("[movie] %s picture %ld out: codec time %lld → time %lld (%d queued before it)", codec_name(a), a->ntr_out, (long long)cpts, (long long)pts, a->nq);
    int k = a->nq; while (!a->synth && k > 0 && a->q[k - 1].pts > pts) { a->q[k] = a->q[k - 1]; k--; }
    a->q[k].nv12 = o; a->q[k].w = w; a->q[k].h = h; a->q[k].pts = pts; a->nq++;
}
// collect the pictures the codec has ready, waiting up to wait_us for the first; 1 = the end-of-stream output came
static int drain(mavc *a, int64_t wait_us) {
    for (;;) {
        AMediaCodecBufferInfo bi; ssize_t i = AMediaCodec_dequeueOutputBuffer(a->mc, &bi, wait_us);
        if (i == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) { read_format(a); continue; }
        if (i == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) continue;
        if (i < 0) return 0;   // AMEDIACODEC_INFO_TRY_AGAIN_LATER
        if (!a->fw) read_format(a);
        size_t n = 0; uint8_t *b = AMediaCodec_getOutputBuffer(a->mc, (size_t)i, &n);
        if (b && bi.size > 0 && !(bi.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG)) {
            // Existing movie_mediacodec.h bounds: form the source pointer only
            // after MediaCodec's offset and length fit its actual output buffer.
            if (bi.offset >= 0 && (size_t)bi.offset <= n && (size_t)bi.size <= n - bi.offset)
                keep_picture(a, b + bi.offset, (size_t)bi.size, bi.presentationTimeUs);
            else if (!a->red++) LOG("[RED] movie: AVC output offset/size exceeds its codec buffer");
            a->nout++;
        }
        AMediaCodec_releaseOutputBuffer(a->mc, (size_t)i, false);
        if (bi.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) return 1;
        wait_us = 0;
    }
}
typedef struct { mavc *a; size_t len; int slice; } au_ctx;   // slice: the first slice's slice_type (0 P, 1 B, 2 I, +5 for "all slices"), -1 none; 10 + the type for an IDR
// slice_type of a slice NAL unit: first_mb_in_slice ue(v), then slice_type ue(v), right after the one-byte header (emulation prevention cannot occur in these first bits)
static int slice_type(const uint8_t *p, size_t n) {
    size_t bit = 8; int v[2];
    for (int k = 0; k < 2; k++) {
        int z = 0; while (bit < n * 8 && !(p[bit >> 3] >> (7 - (bit & 7)) & 1)) { z++; bit++; }
        if (bit >= n * 8 || z > 30) return -1;
        bit++; unsigned x = 1;
        for (int i = 0; i < z; i++, bit++) { if (bit >= n * 8) return -1; x = x << 1 | (p[bit >> 3] >> (7 - (bit & 7)) & 1); }
        v[k] = (int)x - 1;
    }
    return v[1];
}
static void au_add(void *ctx, const uint8_t *nal, size_t n) {
    au_ctx *c = ctx; mavc *a = c->a; int t = nal[0] & 31;
    if ((t == 1 || t == 5) && c->slice < 0) { int s = slice_type(nal, n); c->slice = s < 0 ? -1 : (t == 5 ? 10 : 0) + s % 5; }
    if (t == 7 || t == 8) { keep_ps(a, nal, n); return; }
    if (t == 9) return;   // access unit delimiter
    if (c->len + 4 + n > a->aucap) { a->aucap = (c->len + 4 + n) * 2; a->au = realloc(a->au, a->aucap); if (!a->au) die("movie: AVC buffer allocation failed"); }
    uint8_t *o = a->au + c->len; o[0] = o[1] = o[2] = 0; o[3] = 1; memcpy(o + 4, nal, n); c->len += 4 + n;
}
static ssize_t input_slot(mavc *a) {   // a free input buffer; while none is free, the pictures holding the codec up are collected
    for (int tries = 0; tries < 50; tries++) {
        ssize_t i = AMediaCodec_dequeueInputBuffer(a->mc, 2000);
        if (i >= 0) return i;
        drain(a, 0);
    }
    return -1;
}
int mavc_decode(mavc *a, const uint8_t *p, int n, int64_t pts_us) {
    au_ctx c = {a, 0, -1};
    const uint8_t *src = a->au;
    if (a->vp9) { if (n <= 0) return 0; src = p; c.len = (size_t)n; }   // a VP9 packet (superframe or frame) goes in as it is
    else each_nal(p, (size_t)n, au_add, &c);
    if (!c.len) return 0;
    if ((a->dirty || !a->mc || a->eos_sent) && !open_codec(a)) return -1;
    ssize_t i = input_slot(a);
    if (i < 0) { static long nd; long k = ++nd; if (!(k & (k - 1))) LOG("[RED] movie: MediaCodec %s decoder has no free input buffer, a packet is dropped (%ld so far)", codec_name(a), k); return -1; }
    size_t cap = 0; uint8_t *b = AMediaCodec_getInputBuffer(a->mc, (size_t)i, &cap);
    if (!b || cap < c.len) { if (!a->red++) LOG("[RED] movie: %s packet of %zu bytes does not fit MediaCodec's %zu-byte input buffer", codec_name(a), c.len, cap); AMediaCodec_queueInputBuffer(a->mc, (size_t)i, 0, 0, (uint64_t)pts_us, 0); return -1; }
    memcpy(b, src, c.len);
    int dup = a->last_in != INT64_MIN && pts_us == a->last_in;   // a time the codec holds already: made-up times from here on
    for (int m = 0; !dup && m < a->npmap; m++) dup = a->pmap[m].g == pts_us;
    if (dup && !a->synth) { a->synth = 1; LOG("[movie] %s: packet time %lld µs repeats one still in the decoder: increasing times are made up from here on", codec_name(a), (long long)pts_us); }
    int64_t cpts = !a->synth || pts_us > a->last_in ? pts_us : a->last_in + 1;
    if (cpts > a->last_in) a->last_in = cpts;
    if (a->npmap == 64) a->pmap[0] = a->pmap[--a->npmap];   // (outputs the codec never gave)
    a->pmap[a->npmap].c = cpts; a->pmap[a->npmap].g = pts_us; a->npmap++;
    if (AMediaCodec_queueInputBuffer(a->mc, (size_t)i, 0, c.len, (uint64_t)cpts, 0) != AMEDIA_OK) { if (!a->red++) LOG("[RED] movie: MediaCodec refused an AVC access unit (first case)"); return -1; }
    if (avc_trace() && a->ntr_in++ < 120) LOG("[movie] AVC packet %ld in: time %lld → codec time %lld, %zu bytes, %s%c slice", a->ntr_in, (long long)pts_us, (long long)cpts, c.len,
                                              c.slice >= 10 ? "IDR " : "", c.slice < 0 ? '?' : "PBISS"[c.slice % 10]);
    a->nin++;
    drain(a, 0);
    return 0;
}
// the end of the stream: an end-of-stream input, then every picture still in the codec; the codec is opened afresh for the next stream (a loop restarts with Flush and new packets)
static void finish(mavc *a) {
    if (!a->mc || a->eos_sent) return;
    ssize_t i = input_slot(a);
    if (i >= 0) AMediaCodec_queueInputBuffer(a->mc, (size_t)i, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
    a->eos_sent = 1;
    for (int k = 0; i >= 0 && k < 100 && !drain(a, 10000); k++) {}
    close_codec(a); a->eos_sent = 1;
}
// NV12 → RGBA rows of w pixels: video range, BT.709 from 720 lines up, else BT.601 (VideoToolbox converts with the stream's colour matrix); a VP9 picture by the range and
// standard its output format declares where it does (libvpx's path, hle_movie.c md_rgba, reads the stream's colour config the same way)
static void nv12_rgba(const mavc *a, const uint8_t *y, const uint8_t *uv, int pw, int ph, uint8_t *dst, int w, int h) {
    int hd = ph >= 720, full = 0, cw = pw < w ? pw : w, chh = ph < h ? ph : h;
    if (a->vp9) { full = a->crange == 1; if (a->cstd == 1) hd = 1; else if (a->cstd == 2 || a->cstd == 4) hd = 0; }
    int rv = hd ? 459 : 409, gu = hd ? 55 : 100, gv = hd ? 136 : 208, bu = hd ? 541 : 516;   // ×256, 8-bit video range (luma × 298)
    int ky = 298, y0 = 16;
    if (full) { ky = 256; y0 = 0; rv = hd ? 403 : 359; gu = hd ? 48 : 88; gv = hd ? 120 : 183; bu = hd ? 475 : 454; }   // full range: the same matrices without the 255/219 and 255/224 scaling
    if (cw < w || chh < h) for (size_t i = 0; i < (size_t)w * h; i++) ((uint32_t *)dst)[i] = 0xff000000u;
    for (int r = 0; r < chh; r++) {
        const uint8_t *yr = y + (size_t)r * pw, *cr = uv + (size_t)(r / 2) * ((pw + 1) & ~1); uint8_t *o = dst + (size_t)r * w * 4;
        for (int x = 0; x < cw; x++) {
            int c = ky * (yr[x] - y0), u = cr[x & ~1] - 128, v = cr[(x & ~1) + 1] - 128;
            int R = (c + rv * v + 128) >> 8, G = (c - gu * u - gv * v + 128) >> 8, B = (c + bu * u + 128) >> 8;
            o[4 * x] = (uint8_t)(R < 0 ? 0 : R > 255 ? 255 : R); o[4 * x + 1] = (uint8_t)(G < 0 ? 0 : G > 255 ? 255 : G); o[4 * x + 2] = (uint8_t)(B < 0 ? 0 : B > 255 ? 255 : B); o[4 * x + 3] = 255;
        }
    }
}
// The earliest time of an input still in the codec after the last picture handed out (an entry at or before it is one the codec dropped or gave late), for mavc_take's log
static int64_t earliest_pending(const mavc *a) {
    int64_t m = INT64_MAX;
    for (int i = 0; i < a->npmap; i++) if (a->pmap[i].g > a->last_out && a->pmap[i].g < m) m = a->pmap[i].g;
    return m;
}
int mavc_take(mavc *a, int flush, uint8_t *dst, int w, int h, int64_t *pts_us) {
    if (flush) finish(a);
    else if (!a->nq && a->mc && a->nin > a->nout) drain(a, 4000);   // a picture is in the codec's pipeline: give it a few milliseconds
    if (a->nq > a->nq_max) a->nq_max = a->nq;
    if (!a->nq) { if (a->mc && a->nin > a->nout) a->none++; return 0; }
    uint8_t *pic = a->q[0].nv12; int pw = a->q[0].w, ph = a->q[0].h; if (pts_us) *pts_us = a->q[0].pts;
    // A picture handed out with a time before one handed out already (real times only: made-up ones keep the codec's order). c2.qti.avc.decoder gave Disgaea 5's opening
    // movie (I P B B B, real times) in presentation order: none in 2 runs of 5,400 pictures on the Retroid Pocket Nova; the pictures it showed out of order came from the
    // game's own output queue (hle_movie.c, "Disgaea 5 1.0.3's movie player"). Counted for md_pace, the first ones logged with the queue and what is still in the codec
    if (!a->synth && a->last_out != INT64_MIN && a->q[0].pts < a->last_out && ++a->back_out <= 8) {
        char qs[200]; int o = 0; for (int i = 0; i < a->nq && o < 180; i++) o += snprintf(qs + o, sizeof qs - (size_t)o, " %lld", (long long)(a->q[i].pts / 1000));
        int64_t ep = earliest_pending(a);
        LOG("[movie] AVC: picture time %lld µs handed out after %lld (%ld so far): queue (ms)%s, %ld in the codec, earliest still in it %lld, %d time entries%s",
            (long long)a->q[0].pts, (long long)a->last_out, a->back_out, qs, a->nin - a->nout, (long long)(ep == INT64_MAX ? -1 : ep), a->npmap, flush ? ", flushing" : "");
    }
    if (a->q[0].pts > a->last_out) a->last_out = a->q[0].pts;
    memmove(a->q, a->q + 1, --a->nq * sizeof a->q[0]);
    const uint8_t *uv = pic + (size_t)pw * ph;
    if (a->rgba) nv12_rgba(a, pic, uv, pw, ph, dst, w, h);
    else {
        int cw = pw < w ? pw : w, chh = ph < h ? ph : h;
        if (cw < w || chh < h) { memset(dst, 16, (size_t)w * h); memset(dst + (size_t)w * h, 128, (size_t)w * h / 2); }
        for (int r = 0; r < chh; r++) memcpy(dst + (size_t)r * w, pic + (size_t)r * pw, (size_t)cw);
        int cs = (pw + 1) & ~1, copied = (cw + 1) & ~1;
        if (copied > w) copied = w;
        for (int r = 0; r < (chh + 1) / 2 && r < h / 2; r++) memcpy(dst + (size_t)w * h + (size_t)r * w, uv + (size_t)r * cs, (size_t)copied);
    }
    free(pic);
    return 1;
}
