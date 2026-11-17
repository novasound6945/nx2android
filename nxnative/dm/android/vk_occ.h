// Occlusion counters (NVN_COUNTER_TYPE_SAMPLES_PASSED) on Vulkan, the parts without Vulkan: the counter the completion thread keeps (occ_ctr), what a report carries
// (occ_val) and writes to the guest, a report address's last completed result (occ_ent), and the CPU judgement of SetRenderEnableConditional on such results
// (occ_pick, occ_eq). gfx_vk_core.c (completion) and gfx_vk_exec.c (replay, DM_VK_OCCLUSION) use it; a test outside this repository runs it on the PC.
#pragma once
#include <stdint.h>

// NVN's counter: ResetCounter zeroes it, every draw after adds the samples it passed, ReportCounter writes it out (it does not reset). The replay counts with Vulkan
// occlusion queries, one per stretch of draws inside one render pass (a query cannot span passes, nor a report's point); the completion thread adds each stretch's
// result in command order (OCC_SEG) and a report takes the sum, across passes and submissions alike. A draw the replay does not make while counting (a render
// condition judged false, a pipeline not ready, no query left) is OCC_LOST: its samples are unknown, so the sum is known only up to the lost draws. Two reports of
// one epoch (between two resets) with the same lost count still compare exactly, the same unknown samples being in both (a game that reports before and after an
// object's bounding box: draws lost elsewhere in the frame do not matter); anything else involving a lost draw is unknown. OCC_BROKEN: nothing is known until the
// next reset (a query's result missing: a dropped submission, counting stopped)
enum { OCC_RESET = 1, OCC_SEG, OCC_LOST, OCC_BROKEN };
typedef struct { uint64_t acc; uint32_t lost, epoch; int valid; } occ_ctr;   // valid: reset since the start / the last break
typedef struct { uint64_t v; uint32_t lost, epoch; uint8_t known, approx; } occ_val;   // approx: from imprecise queries (only zero / not zero means anything)
static inline void occ_apply(occ_ctr *c, int kind, int ok, uint64_t v) {   // ok, v: an OCC_SEG's query result
    switch (kind) {
    case OCC_RESET: c->acc = 0; c->lost = 0; c->epoch++; c->valid = 1; break;
    case OCC_SEG: if (!ok) c->valid = 0; else c->acc += v; break;
    case OCC_LOST: c->lost++; break;
    case OCC_BROKEN: c->valid = 0; break;
    }
}
static inline occ_val occ_take(const occ_ctr *c, int precise) { occ_val r = {c->acc, c->lost, c->epoch, (uint8_t)(c->valid != 0), (uint8_t)!precise}; return r; }
// The counter field written to the guest: unknown 1 << 20 (what every report got before: "visible"); each lost draw adds 1 << 20, so a game comparing two reports on the
// CPU sees them differ (visible) unless no draw was lost between them
static inline uint64_t occ_guest(occ_val r) { return r.known ? r.v + ((uint64_t)r.lost << 20) : 1ull << 20; }

// A report address's last completed result. The replay thread inserts entries and counts the reports it records (rec_seq); the completion thread writes each completed
// one's result under a sequence lock (lock odd while it writes), with done_seq the report it was (0: none completed yet)
typedef struct { uint64_t addr; uint32_t rec_seq; int dead; uint32_t lock, done_seq; occ_val val; uint64_t frame; } occ_ent;   // frame: the present the report was recorded in; dead: a report of another type came last
static inline void occ_ent_put(occ_ent *e, uint32_t seq, occ_val v, uint64_t frame) {
    uint32_t l = __atomic_load_n(&e->lock, __ATOMIC_RELAXED);
    __atomic_store_n(&e->lock, l + 1, __ATOMIC_RELAXED); __atomic_thread_fence(__ATOMIC_RELEASE);
    e->val = v; e->frame = frame; __atomic_store_n(&e->done_seq, seq, __ATOMIC_RELAXED);
    __atomic_store_n(&e->lock, l + 2, __ATOMIC_RELEASE);
}
static inline int occ_ent_get(const occ_ent *e, uint32_t *seq, occ_val *v, uint64_t *frame) {   // 0: none completed yet
    for (int tries = 0; tries < 64; tries++) {
        uint32_t l = __atomic_load_n(&e->lock, __ATOMIC_ACQUIRE); if (l & 1) continue;
        occ_val vv = e->val; uint64_t f = e->frame; uint32_t s = __atomic_load_n(&e->done_seq, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&e->lock, __ATOMIC_RELAXED) != l) continue;
        *seq = s; *v = vv; *frame = f; return s != 0;
    }
    return 0;   // (a writer kept it busy: as if none, which draws)
}

// Where a condition's samples-passed side comes from. EXACT: the address's last recorded report has completed (what the GPU would compare). LAG: it has not (replayed
// earlier in this submission or one still running), and the one before it is used (DM_VK_OCCLUSION=2): a frame or more late. PENDING: not completed and nothing
// earlier to use, or strict (DM_VK_OCCLUSION=1). OLD: the earlier result was recorded more than max_age presents ago. PENDING and OLD draw
enum { OCC_FROM_EXACT, OCC_FROM_LAG, OCC_FROM_PENDING, OCC_FROM_OLD };
static inline int occ_pick(uint32_t rec_seq, int has_done, uint32_t done_seq, uint64_t done_frame, uint64_t now, int strict, uint32_t max_age) {
    if (has_done && done_seq == rec_seq) return OCC_FROM_EXACT;
    if (strict || !has_done) return OCC_FROM_PENDING;
    if (now > done_frame && now - done_frame > max_age) return OCC_FROM_OLD;
    return OCC_FROM_LAG;
}
// One side of a condition: OCC_S_NONE not known, OCC_S_MEM an exact 64-bit value (memory, a timestamp report's 0), OCC_S_REP a samples-passed result
enum { OCC_S_NONE, OCC_S_MEM, OCC_S_REP };
typedef struct { int kind; uint64_t mem; occ_val r; } occ_side;
static inline int occ_eq(const occ_side *a, const occ_side *b, int *eq) {   // 1: known, *eq whether the two counters are equal
    if (a->kind == OCC_S_NONE || b->kind == OCC_S_NONE) return 0;
    if (a->kind == OCC_S_MEM && b->kind == OCC_S_MEM) { *eq = a->mem == b->mem; return 1; }
    if (a->kind == OCC_S_MEM || b->kind == OCC_S_MEM) {   // a result against a value: no lost draw at all
        const occ_val *r = a->kind == OCC_S_REP ? &a->r : &b->r; uint64_t m = a->kind == OCC_S_MEM ? a->mem : b->mem;
        if (!r->known || r->lost) return 0;
        if (r->approx) { if (!r->v) { *eq = m == 0; return 1; } if (!m) { *eq = 0; return 1; } return 0; }
        *eq = r->v == m; return 1;
    }
    const occ_val *x = &a->r, *y = &b->r;
    if (!x->known || !y->known) return 0;
    if ((x->lost || y->lost) && (x->epoch != y->epoch || x->lost != y->lost)) return 0;
    if (x->approx || y->approx) { if (!x->v && !y->v) { *eq = 1; return 1; } if (!x->v != !y->v) { *eq = 0; return 1; } return 0; }   // (an imprecise result is 0 only when no sample passed)
    *eq = x->v == y->v; return 1;
}
// SetRenderEnableConditional's mode 0 draws while equal, 1 while different: 1 when the block is skipped (known and false)
static inline int occ_skip(int mode, const occ_side *a, const occ_side *b) { int eq = 0; if (!occ_eq(a, b, &eq)) return 0; return mode ? eq : !eq; }

// The replay's side. Counting starts at a reset; a query is begun at the first draw made in a pass while counting and ended at the pass's end, at a report and at a
// reset, each such stretch one OCC_SEG (a draw not made, or with no query left, OCC_LOST). The caller ends the pass only after occ_rec_close, and posts a report
// after occ_rec_close. io: begin a query (its index, -1 none left), end it, post an operation for the completion thread in command order
typedef struct { int (*begin)(void *u); void (*end)(void *u, int q); void (*post)(void *u, int kind, int q); void *u; } occ_io;
typedef struct { int counting, q; uint64_t frags, lost, nopool; } occ_rec;   // q: the query open in the pass (-1 none); frags, lost, nopool: counts for the log
static inline void occ_rec_close(occ_rec *r, const occ_io *io) { if (r->q >= 0) { io->end(io->u, r->q); io->post(io->u, OCC_SEG, r->q); r->q = -1; } }
static inline void occ_rec_reset(occ_rec *r, const occ_io *io) { occ_rec_close(r, io); r->counting = 1; io->post(io->u, OCC_RESET, -1); }
static inline void occ_rec_stop(occ_rec *r, const occ_io *io) { occ_rec_close(r, io); if (r->counting) io->post(io->u, OCC_BROKEN, -1); r->counting = 0; }
static inline void occ_rec_draw(occ_rec *r, const occ_io *io, int made) {   // a draw at its place in the stream; made: it reaches the GPU, inside an open pass
    if (!r->counting) return;
    if (made && r->q < 0) { r->q = io->begin(io->u); if (r->q >= 0) r->frags++; else r->nopool++; }
    if (!made || r->q < 0) { r->lost++; io->post(io->u, OCC_LOST, -1); }
}
