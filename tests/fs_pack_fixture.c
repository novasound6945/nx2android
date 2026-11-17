// nxnative/dm/host/fs_pack.h on the computer (tests/test_fs_pack.py builds and drives it). One mode per run, tab-separated lines on stdout:
// fs_pack_fixture load <index>                   "ok <files> <packs>", or "err <why>"
// fs_pack_fixture verify <index>                 "ok", or "err <why>" (pk_verify; "loaderr <why>" when pk_load refuses it)
// fs_pack_fixture walk <index>                   every folder from the root, depth first: "dir <path> <files> <folders> <both> <is_dir>", then each of its entries in
//                                                pk_dir_next's order, "ent <folder> <name> f|d <size>", then each of its files read whole, "file <path> <size> <sha256>";
//                                                "bad <what>" for anything pk_find and pk_is_dir say otherwise of an entry
// fs_pack_fixture query <index> <path>...        "q <path> <found> <size> <is_dir> <entries>" for each
// fs_pack_fixture pread <index> <path> <off> <n> "r <result> <sha256 of the bytes>" (one pk_pread)
// fs_pack_fixture open <index> <path>            "ok", or "err <errno>"
// fs_pack_fixture threads <index>                every file read by 8 threads at once, each from its own pk_open: "threads ok <files>", or "bad <path>"
// fs_pack_fixture bench <index> <folder> <finds> "load_ms <ms> find_ms <ms> walk_ms <ms> entries <n>": pk_load, <finds> pk_find of the index's names in turn, one
//                                                walk of <folder>
#define _GNU_SOURCE
#include "fs_pack.h"
#include <time.h>

typedef struct { uint32_t h[8]; uint64_t n; uint8_t b[64]; size_t k; } sha;
static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
#define ROR(x, n) ((x) >> (n) | (x) << (32 - (n)))
static void sha_block(sha *s, const uint8_t *p) {
    uint32_t w[64], a[8];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) w[i] = w[i - 16] + (ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ w[i - 15] >> 3) + w[i - 7] + (ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ w[i - 2] >> 10);
    memcpy(a, s->h, sizeof a);
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = a[7] + (ROR(a[4], 6) ^ ROR(a[4], 11) ^ ROR(a[4], 25)) + ((a[4] & a[5]) ^ (~a[4] & a[6])) + K[i] + w[i];
        uint32_t t2 = (ROR(a[0], 2) ^ ROR(a[0], 13) ^ ROR(a[0], 22)) + ((a[0] & a[1]) ^ (a[0] & a[2]) ^ (a[1] & a[2]));
        memmove(a + 1, a, 7 * sizeof *a); a[4] += t1; a[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++) s->h[i] += a[i];
}
static void sha_init(sha *s) { static const uint32_t h0[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19}; memcpy(s->h, h0, sizeof h0); s->n = 0; s->k = 0; }
static void sha_update(sha *s, const void *d, size_t n) {
    const uint8_t *p = d; s->n += n;
    while (n) { size_t m = 64 - s->k < n ? 64 - s->k : n; memcpy(s->b + s->k, p, m); s->k += m; p += m; n -= m; if (s->k == 64) { sha_block(s, s->b); s->k = 0; } }
}
static void sha_hex(sha *s, char out[65]) {
    uint64_t bits = s->n * 8; uint8_t pad = 0x80, z = 0;
    sha_update(s, &pad, 1); while (s->k != 56) sha_update(s, &z, 1);
    uint8_t l[8]; for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha_update(s, l, 8);
    for (int i = 0; i < 8; i++) snprintf(out + 8 * i, 9, "%08x", s->h[i]);
}

static pk_t *load(const char *p) {
    char err[512]; pk_t *pk = pk_load(p, err, sizeof err);
    if (!pk) printf("loaderr %s\n", err);
    return pk;
}

// a file of the index read whole through its own descriptor, in odd-sized reads (short ones and EINTR gone on from, as the host's loop does): 0 and its hash
static int read_file(pk_t *pk, const char *path, uint64_t *size, char hex[65]) {
    pk_ent e; int fd;
    if (!pk_find(pk, path, &e) || pk_open(pk, &e, &fd)) return -1;
    sha s; sha_init(&s); uint8_t b[7919]; uint64_t off = 0;
    for (;;) {
        ssize_t n = pk_pread(fd, &e, b, sizeof b, off);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { close(fd); return -1; }
        if (!n) break;
        sha_update(&s, b, (size_t)n); off += (uint64_t)n;
    }
    close(fd);
    if (off != e.size) return -1;
    *size = e.size; sha_hex(&s, hex);
    return 0;
}

static void walk(pk_t *pk, const char *dir) {
    printf("dir\t%s\t%d\t%d\t%d\t%d\n", dir, pk_count(pk, dir, 1, 0), pk_count(pk, dir, 0, 1), pk_count(pk, dir, 1, 1), pk_is_dir(pk, dir));
    if (*dir && pk_find(pk, dir, NULL)) printf("bad\tfolder %s found as a file\n", dir);
    pk_cur c; const char *n; size_t nl; int d; uint64_t sz;
    if (!pk_dir_begin(pk, dir, &c)) return;
    size_t cap = 64, cnt = 0; char **kids = malloc(cap * sizeof *kids); int *isd = malloc(cap * sizeof *isd);
    while (pk_dir_next(pk, &c, &n, &nl, &d, &sz)) {
        printf("ent\t%s\t%.*s\t%c\t%llu\n", dir, (int)nl, n, d ? 'd' : 'f', (unsigned long long)sz);
        if (cnt == cap) { cap *= 2; kids = realloc(kids, cap * sizeof *kids); isd = realloc(isd, cap * sizeof *isd); }
        size_t dl = strlen(dir); char *p = malloc(dl + nl + 2);
        if (dl) { memcpy(p, dir, dl); p[dl] = '/'; memcpy(p + dl + 1, n, nl); p[dl + 1 + nl] = 0; } else { memcpy(p, n, nl); p[nl] = 0; }
        kids[cnt] = p; isd[cnt++] = d;
        pk_ent e;
        if (!d && (!pk_find(pk, p, &e) || e.size != sz)) printf("bad\tfile %s not found at its size\n", p);
        if (!d && pk_is_dir(pk, p)) printf("bad\tfile %s taken for a folder\n", p);
    }
    for (size_t i = 0; i < cnt; i++) {
        if (isd[i]) walk(pk, kids[i]);
        else { uint64_t size; char hex[65]; if (read_file(pk, kids[i], &size, hex)) printf("bad\tfile %s cannot be read\n", kids[i]); else printf("file\t%s\t%llu\t%s\n", kids[i], (unsigned long long)size, hex); }
        free(kids[i]);
    }
    free(kids); free(isd);
}

typedef struct { pk_t *pk; char **paths; char (*want)[65]; size_t n; int bad; } job;
static void *thread_main(void *u) {
    job *j = u;
    for (size_t i = 0; i < j->n; i++) { uint64_t size; char hex[65]; if (read_file(j->pk, j->paths[i], &size, hex) || strcmp(hex, j->want[i])) { j->bad = (int)i + 1; break; } }
    return NULL;
}

static double ms(struct timespec a, struct timespec b) { return (double)(b.tv_sec - a.tv_sec) * 1e3 + (double)(b.tv_nsec - a.tv_nsec) / 1e6; }

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: fs_pack_fixture load|verify|walk|query|pread|open|threads|bench <index> ...\n"); return 2; }
    const char *mode = argv[1];
    if (!strcmp(mode, "bench") && argc == 5) {
        struct timespec t0, t1, t2, t3; char err[512];
        clock_gettime(CLOCK_MONOTONIC, &t0);
        pk_t *pk = pk_load(argv[2], err, sizeof err);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        if (!pk) { printf("loaderr %s\n", err); return 0; }
        char **names = malloc(sizeof *names * (pk->nfiles ? pk->nfiles : 1));
        for (uint32_t i = 0; i < pk->nfiles; i++) { size_t nl; const char *n = pk_name(pk, i, &nl); names[i] = malloc(nl + 1); memcpy(names[i], n, nl); names[i][nl] = 0; }
        long finds = atol(argv[4]), hit = 0; pk_ent e;
        struct timespec tl; clock_gettime(CLOCK_MONOTONIC, &tl);
        for (long i = 0; i < finds && pk->nfiles; i++) hit += pk_find(pk, names[(uint32_t)((uint64_t)i * 2654435761u % pk->nfiles)], &e);
        clock_gettime(CLOCK_MONOTONIC, &t2);
        pk_cur c; int nent = 0, d; const char *n; size_t nl; uint64_t sz;
        if (pk_dir_begin(pk, argv[3], &c)) while (pk_dir_next(pk, &c, &n, &nl, &d, &sz)) nent++;
        clock_gettime(CLOCK_MONOTONIC, &t3);
        printf("load_ms %.3f find_ms %.3f walk_ms %.3f entries %d hits %ld\n", ms(t0, t1), ms(tl, t2), ms(t2, t3), nent, hit);
        for (uint32_t i = 0; i < pk->nfiles; i++) free(names[i]);
        free(names); pk_free(pk);
        return 0;
    }
    if (!strcmp(mode, "load")) {
        char err[512]; pk_t *pk = pk_load(argv[2], err, sizeof err);
        if (!pk) printf("err %s\n", err); else printf("ok %u %u\n", pk->nfiles, pk->npacks);
        pk_free(pk); return 0;
    }
    pk_t *pk = load(argv[2]);
    if (!pk) return 0;
    if (!strcmp(mode, "verify")) {
        char err[512];
        if (pk_verify(pk, err, sizeof err)) printf("err %s\n", err); else printf("ok\n");
    } else if (!strcmp(mode, "walk")) {
        walk(pk, "");
    } else if (!strcmp(mode, "query")) {
        for (int i = 3; i < argc; i++) {
            pk_ent e = {0, 0, 0}; int f = pk_find(pk, argv[i], &e);
            printf("q\t%s\t%d\t%llu\t%d\t%d\n", argv[i], f, (unsigned long long)e.size, pk_is_dir(pk, argv[i]), pk_count(pk, argv[i], 1, 1));
        }
    } else if (!strcmp(mode, "pread") && argc == 6) {
        pk_ent e; int fd;
        if (!pk_find(pk, argv[3], &e) || pk_open(pk, &e, &fd)) printf("err\n");
        else {
            size_t n = (size_t)atoll(argv[5]); uint8_t *b = malloc(n + 1);
            ssize_t r = pk_pread(fd, &e, b, n, (uint64_t)atoll(argv[4]));
            sha s; sha_init(&s); if (r > 0) sha_update(&s, b, (size_t)r);
            char hex[65]; sha_hex(&s, hex); printf("r\t%lld\t%s\n", (long long)r, hex);
            free(b); close(fd);
        }
    } else if (!strcmp(mode, "open") && argc == 4) {
        pk_ent e; int fd;
        if (!pk_find(pk, argv[3], &e)) printf("err notfound\n");
        else if (pk_open(pk, &e, &fd)) printf("err %s\n", errno == EIO ? "EIO" : strerror(errno));
        else { printf("ok\n"); close(fd); }
    } else if (!strcmp(mode, "threads")) {
        size_t n = pk->nfiles; char **paths = malloc(sizeof *paths * (n ? n : 1)); char (*want)[65] = malloc(sizeof *want * (n ? n : 1)); int bad = 0;
        for (uint32_t i = 0; i < n; i++) {
            size_t nl; const char *nm = pk_name(pk, i, &nl); paths[i] = malloc(nl + 1); memcpy(paths[i], nm, nl); paths[i][nl] = 0;
            uint64_t size; if (read_file(pk, paths[i], &size, want[i])) bad = 1;
        }
        pk_free(pk); pk = load(argv[2]);   // fresh: no pack open yet, so the threads race for the first opens
        job j[8]; pthread_t t[8];
        for (int k = 0; k < 8; k++) { j[k] = (job){pk, paths, want, n, 0}; pthread_create(&t[k], NULL, thread_main, &j[k]); }
        for (int k = 0; k < 8; k++) { pthread_join(t[k], NULL); if (j[k].bad) { bad = 1; printf("bad\t%s\n", paths[j[k].bad - 1]); } }
        if (!bad) printf("threads ok %zu\n", n);
        for (size_t i = 0; i < n; i++) free(paths[i]);
        free(paths); free(want);
    } else { fprintf(stderr, "unknown mode %s\n", mode); pk_free(pk); return 2; }
    pk_free(pk);
    return 0;
}
