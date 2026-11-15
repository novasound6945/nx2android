#include "../nxnative/dm/gfx/nvn/nvn_tex.c"
// target, format, samples, w, h, d, levels -> out[8]
void sparse_layout(int target, int format, int samples, uint32_t w, uint32_t h, uint32_t d, uint32_t levels, int32_t out[8]) {
    nvn_texdesc t = {target, format, 0, samples, w, h, d, levels, 0, 0};
    nvn_sparse_tile_layout(&t, out);
}
