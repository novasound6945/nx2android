//     (x%64/32)·256 + (y%8/2)·64 + (x%32/16)·32 + (y%2)·16 + x%16
// Tegra X1 block-linear layout (public descriptions: deko3d / Ryujinx BlockLinearLayout):
//   GOB = 64 bytes × 8 rows = 512 bytes; offset of (x byte, y row) within a GOB =
//     (x%64/32)·256 + (y%8/2)·64 + (x%32/16)·32 + (y%2)·16 + x%16
//   block = 1 GOB wide × 2^tileH GOBs high × 2^tileD GOBs deep, ordered y first then z within a block; blocks fill the level in x → y → z row-major order.
// Level / layer start offsets and the actual block height and block depth of this level come from nvn_tex.c; here we only do intra-level addressing and copying.
#include "nvn_deswizzle.h"
#include <stdlib.h>
#include <string.h>

static uint32_t gob_off(uint32_t x, uint32_t y) {
    return (x % 64 / 32) * 256 + (y % 8 / 2) * 64 + (x % 32 / 16) * 32 + (y % 2) * 16 + x % 16;
}

uint64_t nvn_bl_offset(uint32_t x, uint32_t y, uint32_t z, uint32_t row_bytes, uint32_t rows, unsigned th, unsigned td) {
    uint32_t bh = 1u << th, bd = 1u << td;
    uint64_t gobs_x = (row_bytes + 63) / 64, blocks_y = (rows + 8 * bh - 1) / (8 * bh);
    uint64_t block = 512ull * bh * bd;                                  // bytes in one block
    return (z / bd) * gobs_x * blocks_y * block                        // which block layer (along depth)
         + (y / (8 * bh)) * gobs_x * block                             // which block row
         + (x / 64) * block                                            // which block column
         + ((z % bd) * bh + (y % (8 * bh)) / 8) * 512ull               // which GOB within the block
         + gob_off(x, y);
}

static uint32_t mip(uint32_t s, unsigned l) { s >>= l; return s ? s : 1; }

// Geometry of this level: width-in-blocks byte count, block row count, slice count, and where this level and layer starts in storage; returns -1 for unsupported textures
int nvn_deswizzle_geom(const nvn_texdesc *d, unsigned level, unsigned layer,
                       uint32_t *row_bytes, uint32_t *rows, uint32_t *slices, uint64_t *base, unsigned *th, unsigned *td, uint32_t *bpb) {
    if (d->target == 5 || d->target == 6 || d->target == 10 || (d->flags & 0x10) || d->samples > 1) return -1;   // MS / BUFFER / LINEAR
    int bw, bh; *bpb = (uint32_t)nvn_fmt_bpb(d->format, &bw, &bh);
    if (!*bpb || level >= (d->levels ? d->levels : 1)) return -1;
    nvn_texlayout L; nvn_tex_layout(d, &L);
    if (layer >= L.layers) return -1;
    int one_d = d->target == 0 || d->target == 3;
    uint32_t w = mip(d->width, level), h = one_d ? 1 : mip(d->height, level);
    *slices = d->target == 2 ? mip(d->depth ? d->depth : 1, level) : 1;
    *row_bytes = (w + bw - 1) / bw * *bpb; *rows = (h + bh - 1) / bh;
    *base = (uint64_t)layer * L.layer_size + nvn_tex_level_offset(d, level);
    nvn_tex_level_tiles(d, level, th, td);
    return 0;
}

static int move(const nvn_texdesc *d, unsigned level, unsigned layer, uint8_t *bl, uint8_t *lin, size_t pitch, int to_linear) {
    uint32_t row_bytes, rows, slices, bpb; uint64_t base; unsigned th, td;
    if (nvn_deswizzle_geom(d, level, layer, &row_bytes, &rows, &slices, &base, &th, &td, &bpb)) return -1;
    if (pitch < row_bytes) return -1;
    bl += base; uint32_t lb = nvn_tex_level_gobs(d, level) * 64;   // the row as laid out (padded in a sparse texture's tiled levels)
    if (lb < row_bytes) lb = row_bytes;
    // GOB by GOB in storage order (nvn_bl_offset taken apart): block rows, their blocks (GOB columns) left to right, the GOBs of a block top to bottom, so the
    // block-linear side is read (or written) straight through; a GOB holds each of its 8 rows' 64 bytes as four 16-byte segments at 0, 32, 256 and 288 (gob_off).
    // The old loop worked out the whole formula, with its 64-bit divisions, for every 16 bytes in linear order. Retroid Pocket Nova, big core, cached memory both
    // sides: 1.4-2.2 GB/s before, 8.9-13.5 GB/s this way. In the app (into Vulkan scratch memory, from textures the game just loaded) Super Mario Bros. Wonder's
    // course start uploads 86 textures, 74 MB, in one frame: the packaged uploads took 70 ms of the replay thread before, 53 ms now (30-32 ms of it this loop).
    // tests/t_tex.c (F) checks every byte against nvn_bl_offset
    uint32_t bh = 1u << th, bd = 1u << td;
    uint64_t gobs_x = (lb + 63) / 64, blocks_y = (rows + 8 * bh - 1) / (8 * bh), block = 512ull * bh * bd;
    uint32_t cols = (row_bytes + 63) / 64;   // GOB columns the level's bytes reach into (the last may be partial)
    for (uint32_t z = 0; z < slices; z++) {
        uint8_t *slab = bl + (z / bd) * gobs_x * blocks_y * block + (z % bd) * bh * 512ull;   // this slice's GOBs: block layer, then its place in a block's depth
        uint8_t *zl = lin + (size_t)z * rows * pitch;
        for (uint32_t by = 0; by < blocks_y; by++) {
            uint32_t left = rows - by * bh * 8, gn = (left + 7) / 8 < bh ? (left + 7) / 8 : bh;   // GOBs of this block row inside the level
            for (uint32_t gx = 0; gx < cols; gx++) {
                uint32_t xb = gx * 64, xn = row_bytes - xb < 64 ? row_bytes - xb : 64;
                for (uint32_t g = 0; g < gn; g++) {
                    uint8_t *gob = slab + by * gobs_x * block + gx * block + g * 512ull;
                    uint32_t y0 = (by * bh + g) * 8, ny = rows - y0 < 8 ? rows - y0 : 8;
                    for (uint32_t r = 0; r < ny; r++) {
                        uint8_t *o = zl + (size_t)(y0 + r) * pitch + xb, *s = gob + (r / 2) * 64 + (r % 2) * 16;
                        if (xn == 64) {
                            if (to_linear) { memcpy(o, s, 16); memcpy(o + 16, s + 32, 16); memcpy(o + 32, s + 256, 16); memcpy(o + 48, s + 288, 16); }
                            else { memcpy(s, o, 16); memcpy(s + 32, o + 16, 16); memcpy(s + 256, o + 32, 16); memcpy(s + 288, o + 48, 16); }
                        } else for (uint32_t x = 0; x < xn; x += 16) {   // the partial last column: 16-byte segments (16 bytes are contiguous within a GOB)
                            uint32_t n = xn - x < 16 ? xn - x : 16; uint8_t *q = s + (x / 32) * 256 + (x % 32 / 16) * 32;
                            if (to_linear) memcpy(o + x, q, n); else memcpy(q, o + x, n);
                        }
                    }
                }
            }
        }
    }
    return 0;
}

int nvn_deswizzle(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src, void *dst, size_t dst_pitch) {
    return move(d, level, layer, (uint8_t *)src, (uint8_t *)dst, dst_pitch, 1);
}
int nvn_swizzle(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src, size_t src_pitch, void *dst) {
    return move(d, level, layer, (uint8_t *)dst, (uint8_t *)src, src_pitch, 0);
}
