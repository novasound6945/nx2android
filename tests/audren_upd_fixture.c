// The audio renderer update protocol (nxnative/dm/host/hle_audren_upd.c) behind a ctypes surface for tests/test_audren_upd.py: an opaque renderer, updates, frames
#include "../nxnative/dm/host/hle_audren_upd.c"

void *fx_open(const uint8_t *param, uint32_t *res) {
    aru_renderer *r = calloc(1, sizeof *r);
    *res = aru_open(r, param);
    if (*res) { free(r); return NULL; }
    return r;
}
void fx_close(void *r) { aru_close(r); free(r); }
void fx_start(void *r, int on) { ((aru_renderer *)r)->started = on; }
uint32_t fx_update(void *r, const uint8_t *in, size_t in_n, uint8_t *out, size_t out_n, uint32_t *total, uint32_t *wb_new) {
    aru_report rep;
    uint32_t res = aru_update(r, in, in_n, out, out_n, &rep);
    *total = rep.out_total; *wb_new = rep.wb_new;
    return res;
}
uint32_t fx_frame(void *r) { aru_frame_report f; aru_frame(r, NULL, &f); return f.wb_consumed; }
// One frame into out (2 × ARU_OUT_MAX int16): the frames written; st[0..6] peak, clipped, reads refused, voices mixed, warnings, effect types skipped, wave buffers finished
uint32_t fx_render(void *r, int16_t *out, uint32_t *st) {
    aru_frame_report f; uint32_t n = aru_frame(r, out, &f);
    st[0] = f.peak; st[1] = f.clipped; st[2] = f.unmapped; st[3] = f.voices_mixed; st[4] = f.warn; st[5] = f.fx_skipped; st[6] = f.wb_consumed;
    return n;
}
uint32_t fx_out_max(void) { return ARU_OUT_MAX; }
uint64_t fx_work(const uint8_t *param) { return aru_work_buffer_size(param); }
