// Graphics pipeline libraries (gfx_vk_exec.c gpl_*): the parts of their bookkeeping that need no Vulkan, so a test on the computer runs them (the test is kept outside this repository).
// The reuse mark of the shader preparation (vk_gpl_prepare), the pre-rasterization key's states left static, which pipeline a draw takes, and the timing samples logged.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- the reuse mark: DM_ROOT/vk_gpl.id. The shader parts are made again (the preparation screen) when the bundle, the driver or the parts' key changes ----
#define GPL_ID_MAGIC 0x31495047u   // "GPI1"
#define GPL_KEY_VER 2u              // the libraries' keys and contents (gfx_vk_exec.c gpl_pr_make / gpl_fs_make): a change here makes every device prepare again
                                    // (2: the alpha reference a push constant and every layout with its range; the GREATER alpha test variants prepared too)
typedef struct {
    uint32_t magic, key_ver, vendor, device, driver, eds3, sg_pin, nshaders;   // eds3: the dynamic states the parts were made with (V.gpl_eds3); sg_pin: DM_VK_SUBGROUP_PIN's size (0 none)
    uint8_t uuid[16];      // pipelineCacheUUID
    uint64_t bundle;       // gpl_hash64 of the shader bundle file
    uint32_t complete, int_ver;   // complete: every part of the bundle was made (an interrupted preparation leaves 0: the next start prepares again, the cache keeps what was made)
                                  // int_ver: GPL_INT_VER when the integer output parts were made too (0: a mark from before them, which then has those made alone)
} gpl_id;
// The integer output parts (gfx_vk_gpl.inc, vk_spv_fragout.h spv_int_writer): outside the key, so a mark from before them stays good for a bundle without such shaders
// (every game but a few: no preparation again); the start finds them and makes them alone, then marks it
#define GPL_INT_VER 1u
_Static_assert(sizeof(gpl_id) == 64, "gpl_id layout");
static inline uint64_t gpl_hash64(const void *p, size_t n) {   // FNV-1a over 8-byte words, then the tail (fast enough for a 25 MB bundle at each start: ~10 ms)
    const uint8_t *b = (const uint8_t *)p; uint64_t h = 1469598103934665603ull ^ (uint64_t)n; size_t i = 0;
    for (; i + 8 <= n; i += 8) { uint64_t w; memcpy(&w, b + i, 8); h = (h ^ w) * 1099511628211ull; h ^= h >> 29; }
    for (; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
static inline int gpl_id_same(const gpl_id *a, const gpl_id *b) { return !memcmp(a, b, offsetof(gpl_id, complete)); }   // the same bundle, driver and key (complete aside)
static inline int gpl_id_up_to_date(const gpl_id *want, const gpl_id *have) { return gpl_id_same(want, have) && have->magic == GPL_ID_MAGIC && have->complete == 1; }
static inline int gpl_id_read(const char *path, gpl_id *out) {   // 1 when the file holds one mark
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    gpl_id x; int ok = fread(&x, 1, sizeof x, f) == sizeof x && fgetc(f) == EOF; fclose(f);
    if (ok) *out = x; return ok;
}
static inline int gpl_id_write(const char *path, const gpl_id *id) {   // written whole to <path>.tmp, then renamed over (an app killed mid-write keeps the old mark)
    char tmp[1100]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb"); if (!f) return 0;
    int ok = fwrite(id, 1, sizeof *id, f) == sizeof *id; if (fclose(f)) ok = 0;
#ifdef _WIN32
    if (ok) remove(path);
#endif
    if (!ok || rename(tmp, path)) { remove(tmp); return 0; }
    return 1;
}

// ---- the pre-rasterization part's key: a state the device can leave dynamic (extended dynamic state 3) is left out of it (0), the others are taken as drawn ----
enum { GPL_E3_CLAMP = 1, GPL_E3_POLY = 2, GPL_E3_CLIP = 4 };
typedef struct { uint32_t clamp, mode, clip; } gpl_rast;
static inline gpl_rast gpl_rast_key(uint32_t eds3, uint32_t clamp, uint32_t mode, uint32_t clip_neg1, int has_clip_ctl) {   // mode: NVN's (0 point, 1 line, else fill)
    gpl_rast r; r.clamp = eds3 & GPL_E3_CLAMP ? 0 : clamp != 0; r.mode = eds3 & GPL_E3_POLY ? 0 : mode == 0 ? 1 : mode == 1 ? 2 : 0;   // (0: fill, the preparation's)
    r.clip = eds3 & GPL_E3_CLIP || !has_clip_ctl ? 0 : clip_neg1 != 0; return r;
}
static inline int gpl_rast_variants(uint32_t eds3, int has_clip_ctl) { return !(eds3 & GPL_E3_CLIP) && has_clip_ctl ? 2 : 1; }   // pre-rasterization parts the preparation makes per vertex shader

// ---- which pipeline a draw takes. opt: the whole pipeline's entry (0 none, 1 built, 2 failed, 3 being built); eligible: the draw can take a linked one (gpl_eligible) ----
enum { GPL_OPT_NONE, GPL_OPT_OK, GPL_OPT_FAILED, GPL_OPT_PENDING };
enum { GPL_USE_OPT, GPL_USE_FAST, GPL_USE_OLD };   // the whole pipeline; the parts linked (made now if missing; the whole one queued when opt is NONE); the path without libraries
static inline int gpl_route(int gpl_on, int eligible, int opt) {
    if (opt == GPL_OPT_OK) return GPL_USE_OPT;
    if (gpl_on && eligible) return GPL_USE_FAST;   // (a failed whole pipeline keeps its linked one for good)
    return GPL_USE_OLD;
}

// ---- timing samples (microseconds): the last GPL_NS of them, for the log's p50 / p95 / max ----
enum { GPL_NS = 2048 };
typedef struct { uint32_t v[GPL_NS]; uint64_t n, sum_us; uint32_t max; } gpl_stat;
static inline void gpl_stat_add(gpl_stat *s, uint64_t ns) { uint32_t us = ns / 1000 > 0xffffffffu ? 0xffffffffu : (uint32_t)(ns / 1000); s->v[s->n % GPL_NS] = us; s->n++; s->sum_us += us; if (us > s->max) s->max = us; }
static inline int gpl_u32cmp(const void *a, const void *b) { uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b; return x < y ? -1 : x > y; }
static inline void gpl_stat_pct(const gpl_stat *s, double *p50, double *p95, double *max) {   // in ms, over the samples kept
    uint32_t n = s->n < GPL_NS ? (uint32_t)s->n : GPL_NS; *p50 = *p95 = 0; *max = s->max / 1000.0; if (!n) return;
    uint32_t *c = (uint32_t *)malloc(n * sizeof *c); if (!c) return; memcpy(c, s->v, n * sizeof *c); qsort(c, n, sizeof *c, gpl_u32cmp);
    *p50 = c[(n - 1) / 2] / 1000.0; *p95 = c[(uint32_t)((n - 1) * 0.95)] / 1000.0; free(c);
}
