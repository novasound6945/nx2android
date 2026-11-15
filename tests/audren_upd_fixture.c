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
uint32_t fx_frame(void *r) { aru_frame_report f; aru_frame(r, &f); return f.wb_consumed; }
uint64_t fx_work(const uint8_t *param) { return aru_work_buffer_size(param); }
