// YUV 4:2:0 → RGBA rows with NEON, 16 pixels a step, bit-exact with the scalar loop it stands in for (hle_movie.c md_rgba, libvpx's I420 pictures): each output is
// clamp((ky * (Y - y0) + chroma term + round) >> shift), in 32-bit lanes as the scalar code computes it. A 1080p frame is 2 million pixels: on the Retroid Pocket Nova the
// scalar loop took 7.5 to 10 ms of each Link's Awakening opening picture (libvpx's decode itself 2 to 3 ms), this one about 4 ms.
// Android arm64 only (the Mac build keeps the scalar loops); the caller does the pixels past the last multiple of 16
#pragma once
#if defined(__aarch64__) && defined(__ANDROID__)
#include <arm_neon.h>
#include <stdint.h>
#include <stdlib.h>
#define DM_YUV_NEON 1
// DM_MOVIE_NEON=0 (read once): the scalar loops only (A/B)
static inline int dm_yuv_neon_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("DM_MOVIE_NEON"); on = !e || atoi(e) != 0; } return on; }
typedef struct { int32_t ky, y0, rv, gu, gv, bu, round, shift; } dm_yuv_k;
static inline uint8x16_t dm_yuv_ch(int32x4_t l0, int32x4_t l1, int32x4_t l2, int32x4_t l3, int32x4_t c0, int32x4_t c1, int32x4_t sh) {
    // c0 / c1: the chroma term (rounding included) of pixel pairs 0-3 and 4-7, each lane for two pixels
    int32x4_t a = vshlq_s32(vaddq_s32(l0, vzip1q_s32(c0, c0)), sh), b = vshlq_s32(vaddq_s32(l1, vzip2q_s32(c0, c0)), sh);
    int32x4_t c = vshlq_s32(vaddq_s32(l2, vzip1q_s32(c1, c1)), sh), d = vshlq_s32(vaddq_s32(l3, vzip2q_s32(c1, c1)), sh);
    uint16x8_t lo = vcombine_u16(vqmovun_s32(a), vqmovun_s32(b)), hi = vcombine_u16(vqmovun_s32(c), vqmovun_s32(d));
    return vcombine_u8(vqmovn_u16(lo), vqmovn_u16(hi));
}
// 16 pixels from 16 luma bytes and 8 chroma samples each (u[i], v[i] for pixels 2i, 2i+1)
static inline void dm_yuv16(const dm_yuv_k *k, uint8x16_t y, uint8x8_t u8, uint8x8_t v8, uint8_t *o) {
    int16x8_t y0 = vdupq_n_s16((int16_t)k->y0);
    int16x8_t yl = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(y))), y0), yh = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(vget_high_u8(y))), y0);
    int32x4_t ky = vdupq_n_s32(k->ky);
    int32x4_t l0 = vmulq_s32(vmovl_s16(vget_low_s16(yl)), ky), l1 = vmulq_s32(vmovl_s16(vget_high_s16(yl)), ky);
    int32x4_t l2 = vmulq_s32(vmovl_s16(vget_low_s16(yh)), ky), l3 = vmulq_s32(vmovl_s16(vget_high_s16(yh)), ky);
    int16x8_t c128 = vdupq_n_s16(128);
    int16x8_t u = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(u8)), c128), v = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(v8)), c128);
    int32x4_t ul = vmovl_s16(vget_low_s16(u)), uh = vmovl_s16(vget_high_s16(u)), vl = vmovl_s16(vget_low_s16(v)), vh = vmovl_s16(vget_high_s16(v));
    int32x4_t rnd = vdupq_n_s32(k->round), sh = vdupq_n_s32(-k->shift);
    int32x4_t rv = vdupq_n_s32(k->rv), gu = vdupq_n_s32(k->gu), gv = vdupq_n_s32(k->gv), bu = vdupq_n_s32(k->bu);
    uint8x16x4_t px;
    px.val[0] = dm_yuv_ch(l0, l1, l2, l3, vmlaq_s32(rnd, rv, vl), vmlaq_s32(rnd, rv, vh), sh);
    px.val[1] = dm_yuv_ch(l0, l1, l2, l3, vmlsq_s32(vmlsq_s32(rnd, gu, ul), gv, vl), vmlsq_s32(vmlsq_s32(rnd, gu, uh), gv, vh), sh);
    px.val[2] = dm_yuv_ch(l0, l1, l2, l3, vmlaq_s32(rnd, bu, ul), vmlaq_s32(rnd, bu, uh), sh);
    px.val[3] = vdupq_n_u8(255);
    vst4q_u8(o, px);
}
#endif
