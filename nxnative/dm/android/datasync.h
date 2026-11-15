// The app's data from a web server (an APK ported with porter --data-url): the published pack's files.json (porter/publish.py) compared with the copy the app keeps in its
// files folder, and the files that are missing or changed downloaded, each into <name>.part (resumed with a Range request after a break), checked against its SHA-256,
// then renamed into place. files.json is written last: a files folder with it holds the whole pack, so an app that cannot reach the server starts the game from it.
// The player's own files stay: settings.txt is downloaded only when there is none; env.txt is the pack's with the rows the player added kept (ds_env_merge); a file the
// server no longer lists is left alone. No Android here: the transport is a callback (front_android.c: java.net.HttpURLConnection through JNI) and a test on the computer
// drives it with a folder (C:\d2rm\tools\easy-install\t_sync.c).
#ifndef DATASYNC_H
#define DATASYNC_H
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define ds_mkdir(p) _mkdir(p)
typedef struct _stat64 ds_stat_t;
#define ds_stat(p, s) _stat64(p, s)
static inline int ds_rename(const char *a, const char *b) { remove(b); return rename(a, b); }
#define ds_fseek _fseeki64
#else
#define ds_mkdir(p) mkdir(p, 0755)
typedef struct stat ds_stat_t;
#define ds_stat(p, s) stat(p, s)
#define ds_rename rename
#define ds_fseek fseeko
#endif

#define DS_LIST "files.json"
#define DS_PREV "files.json.prev"     // the list of the pack the folder held before a download started (its hashes save rehashing files that did not change)
#define DS_ENV_PACK "env.pack.txt"    // the pack's env.txt as downloaded: the rows the player added are those of env.txt that are not here (ds_env_merge)
#define DS_MANIFEST "nx2a-data.json"
enum { DS_DONE = 0, DS_UPTODATE, DS_OFFLINE, DS_ERR_NET, DS_ERR_SERVER, DS_ERR_PACKAGE, DS_ERR_SPACE, DS_ERR_DATA, DS_ERR_DISK };
enum { DS_PH_CONNECT, DS_PH_CHECK, DS_PH_DOWNLOAD, DS_PH_FINISH };

// ---- SHA-256 ----
typedef struct { uint32_t h[8]; uint64_t n; uint8_t b[64]; size_t k; } ds_sha;
static const uint32_t DS_K[64] = {0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,
    0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,
    0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,
    0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,
    0xbef9a3f7,0xc67178f2};
#define DS_ROR(x, n) ((x) >> (n) | (x) << (32 - (n)))
static void ds_sha_block(ds_sha *s, const uint8_t *p) {
    uint32_t w[64], a[8];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) w[i] = w[i - 16] + (DS_ROR(w[i - 15], 7) ^ DS_ROR(w[i - 15], 18) ^ w[i - 15] >> 3) + w[i - 7] + (DS_ROR(w[i - 2], 17) ^ DS_ROR(w[i - 2], 19) ^ w[i - 2] >> 10);
    memcpy(a, s->h, sizeof a);
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = a[7] + (DS_ROR(a[4], 6) ^ DS_ROR(a[4], 11) ^ DS_ROR(a[4], 25)) + ((a[4] & a[5]) ^ (~a[4] & a[6])) + DS_K[i] + w[i];
        uint32_t t2 = (DS_ROR(a[0], 2) ^ DS_ROR(a[0], 13) ^ DS_ROR(a[0], 22)) + ((a[0] & a[1]) ^ (a[0] & a[2]) ^ (a[1] & a[2]));
        memmove(a + 1, a, 7 * sizeof *a); a[4] += t1; a[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++) s->h[i] += a[i];
}
static void ds_sha_init(ds_sha *s) { static const uint32_t h0[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19}; memcpy(s->h, h0, sizeof h0); s->n = 0; s->k = 0; }
static void ds_sha_update(ds_sha *s, const void *d, size_t n) {
    const uint8_t *p = d; s->n += n;
    if (s->k) { size_t t = 64 - s->k < n ? 64 - s->k : n; memcpy(s->b + s->k, p, t); s->k += t; p += t; n -= t; if (s->k == 64) { ds_sha_block(s, s->b); s->k = 0; } }
    for (; n >= 64; p += 64, n -= 64) ds_sha_block(s, p);
    if (n) { memcpy(s->b, p, n); s->k = n; }
}
static void ds_sha_hex(ds_sha *s, char out[65]) {
    uint64_t bits = s->n * 8; uint8_t pad = 0x80, z = 0, len[8];
    ds_sha_update(s, &pad, 1); while (s->k != 56) ds_sha_update(s, &z, 1);
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    ds_sha_update(s, len, 8);
    for (int i = 0; i < 8; i++) snprintf(out + 8 * i, 9, "%08x", s->h[i]);
}

// ---- files.json: a small JSON reader for the list porter/publish.py writes (objects, arrays, strings with escapes, numbers, true / false / null) ----
typedef struct { char *path; uint64_t size; char sha[65]; } ds_file;
typedef struct { char id[65], package[256], version[64], display[64], variants[8][16]; int nvariants, n, cap; ds_file *f; uint64_t bytes; } ds_list;
typedef struct { const char *p, *e; } ds_js;
static void ds_ws(ds_js *j) { while (j->p < j->e && (*j->p == ' ' || *j->p == '\n' || *j->p == '\r' || *j->p == '\t')) j->p++; }
static int ds_str(ds_js *j, char *out, size_t cap) {   // a string into out (NUL-terminated, cut at cap - 1 bytes); -1 on a syntax error
    ds_ws(j); if (j->p >= j->e || *j->p != '"') return -1; j->p++;
    size_t n = 0;
    while (j->p < j->e && *j->p != '"') {
        unsigned c = (unsigned char)*j->p++;
        if (c == '\\') {
            if (j->p >= j->e) return -1;
            char x = *j->p++;
            if (x == 'u') {
                if (j->e - j->p < 4) return -1;
                unsigned u = 0; for (int i = 0; i < 4; i++) { char h = *j->p++; u = u << 4 | (unsigned)(h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : 0); }
                if (u >= 0xD800 && u < 0xDC00 && j->e - j->p >= 6 && j->p[0] == '\\' && j->p[1] == 'u') {   // a surrogate pair
                    unsigned l = 0; for (int i = 2; i < 6; i++) { char h = j->p[i]; l = l << 4 | (unsigned)(h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : 0); }
                    if (l >= 0xDC00 && l < 0xE000) { u = 0x10000 + ((u - 0xD800) << 10) + (l - 0xDC00); j->p += 6; }
                }
                char b[4]; int k = u < 0x80 ? (b[0] = (char)u, 1) : u < 0x800 ? (b[0] = (char)(0xC0 | u >> 6), b[1] = (char)(0x80 | (u & 63)), 2)
                    : u < 0x10000 ? (b[0] = (char)(0xE0 | u >> 12), b[1] = (char)(0x80 | (u >> 6 & 63)), b[2] = (char)(0x80 | (u & 63)), 3)
                    : (b[0] = (char)(0xF0 | u >> 18), b[1] = (char)(0x80 | (u >> 12 & 63)), b[2] = (char)(0x80 | (u >> 6 & 63)), b[3] = (char)(0x80 | (u & 63)), 4);
                for (int i = 0; i < k; i++) if (n + 1 < cap) out[n++] = b[i];
                continue;
            }
            c = x == 'n' ? '\n' : x == 't' ? '\t' : x == 'r' ? '\r' : x == 'b' ? '\b' : x == 'f' ? '\f' : (unsigned char)x;
        }
        if (n + 1 < cap) out[n++] = (char)c;
    }
    if (j->p >= j->e) return -1;
    j->p++; if (cap) out[n] = 0; return 0;
}
static int ds_skip(ds_js *j, int depth) {   // any value
    ds_ws(j); if (j->p >= j->e || depth > 32) return -1;
    char c = *j->p;
    if (c == '"') { char t[1]; return ds_str(j, t, 0); }
    if (c == '{' || c == '[') {
        char close = c == '{' ? '}' : ']'; j->p++; ds_ws(j);
        if (j->p < j->e && *j->p == close) { j->p++; return 0; }
        for (;;) {
            if (c == '{') { char t[1]; if (ds_str(j, t, 0)) return -1; ds_ws(j); if (j->p >= j->e || *j->p++ != ':') return -1; }
            if (ds_skip(j, depth + 1)) return -1;
            ds_ws(j); if (j->p >= j->e) return -1;
            if (*j->p == ',') { j->p++; continue; }
            if (*j->p++ == close) return 0;
            return -1;
        }
    }
    const char *s = j->p; while (j->p < j->e && (strchr("+-.eE", *j->p) || (*j->p >= '0' && *j->p <= '9') || (*j->p >= 'a' && *j->p <= 'z'))) j->p++;
    return j->p > s ? 0 : -1;
}
static int ds_u64(ds_js *j, uint64_t *v) {
    ds_ws(j); uint64_t x = 0; const char *s = j->p;
    while (j->p < j->e && *j->p >= '0' && *j->p <= '9') x = x * 10 + (uint64_t)(*j->p++ - '0');
    if (j->p == s) return -1;
    if (j->p < j->e && (*j->p == '.' || *j->p == 'e' || *j->p == 'E')) return -1;   // sizes are whole numbers
    *v = x; return 0;
}
static void ds_free(ds_list *l) { for (int i = 0; i < l->n; i++) free(l->f[i].path); free(l->f); memset(l, 0, sizeof *l); }
static int ds_file_obj(ds_js *j, ds_file *f) {
    char key[32], path[1024] = ""; f->size = (uint64_t)-1; f->sha[0] = 0;
    ds_ws(j); if (j->p >= j->e || *j->p++ != '{') return -1;
    ds_ws(j); if (j->p < j->e && *j->p == '}') { j->p++; return -1; }
    for (;;) {
        if (ds_str(j, key, sizeof key)) return -1;
        ds_ws(j); if (j->p >= j->e || *j->p++ != ':') return -1;
        if (!strcmp(key, "path")) { if (ds_str(j, path, sizeof path)) return -1; }
        else if (!strcmp(key, "size")) { if (ds_u64(j, &f->size)) return -1; }
        else if (!strcmp(key, "sha256")) { if (ds_str(j, f->sha, sizeof f->sha)) return -1; }
        else if (ds_skip(j, 1)) return -1;
        ds_ws(j); if (j->p >= j->e) return -1;
        if (*j->p == ',') { j->p++; continue; }
        if (*j->p++ != '}') return -1;
        break;
    }
    // a path the app can write under its folder: relative, no empty, . or .. parts, no backslash
    if (!path[0] || path[0] == '/' || strchr(path, '\\') || strlen(f->sha) != 64 || f->size == (uint64_t)-1) return -1;
    for (const char *s = path; *s;) { const char *e = strchr(s, '/'); size_t k = e ? (size_t)(e - s) : strlen(s);
        if (!k || (k == 1 && s[0] == '.') || (k == 2 && s[0] == '.' && s[1] == '.')) return -1; s += k + (e != NULL); if (e && !*s) return -1; }
    f->path = strdup(path); return f->path ? 0 : -1;
}
static int ds_parse(const char *txt, size_t n, ds_list *l) {   // 0, or -1 for a list that is not the pack's (the reason is not kept: the server's file is what it is)
    memset(l, 0, sizeof *l); ds_js j = {txt, txt + n}; char key[32];
    ds_ws(&j); if (j.p >= j.e || *j.p++ != '{') return -1;
    for (;;) {
        if (ds_str(&j, key, sizeof key)) goto bad;
        ds_ws(&j); if (j.p >= j.e || *j.p++ != ':') goto bad;
        if (!strcmp(key, "id")) { if (ds_str(&j, l->id, sizeof l->id)) goto bad; }
        else if (!strcmp(key, "package")) { if (ds_str(&j, l->package, sizeof l->package)) goto bad; }
        else if (!strcmp(key, "version")) { if (ds_str(&j, l->version, sizeof l->version)) goto bad; }
        else if (!strcmp(key, "display")) { if (ds_str(&j, l->display, sizeof l->display)) goto bad; }
        else if (!strcmp(key, "bytes")) { if (ds_u64(&j, &l->bytes)) goto bad; }
        else if (!strcmp(key, "variants") || !strcmp(key, "files")) {
            int files = key[0] == 'f'; ds_ws(&j); if (j.p >= j.e || *j.p++ != '[') goto bad;
            ds_ws(&j); if (j.p < j.e && *j.p == ']') j.p++;
            else for (;;) {
                if (files) {
                    if (l->n == l->cap) { int c = l->cap ? l->cap * 2 : 256; ds_file *f = realloc(l->f, (size_t)c * sizeof *f); if (!f) goto bad; l->f = f; l->cap = c; }
                    if (ds_file_obj(&j, &l->f[l->n])) goto bad;
                    l->n++;
                } else { char v[16]; if (ds_str(&j, v, sizeof v)) goto bad; if (l->nvariants < 8) snprintf(l->variants[l->nvariants++], sizeof l->variants[0], "%s", v); }
                ds_ws(&j); if (j.p >= j.e) goto bad;
                if (*j.p == ',') { j.p++; continue; }
                if (*j.p++ != ']') goto bad;
                break;
            }
        } else if (ds_skip(&j, 1)) goto bad;
        ds_ws(&j); if (j.p >= j.e) goto bad;
        if (*j.p == ',') { j.p++; continue; }
        if (*j.p++ != '}') goto bad;
        break;
    }
    if (strlen(l->id) != 64 || !l->package[0]) goto bad;
    return 0;
bad: ds_free(l); return -1;
}
static const ds_file *ds_find(const ds_list *l, const char *path) { for (int i = 0; l && i < l->n; i++) if (!strcmp(l->f[i].path, path)) return &l->f[i]; return NULL; }

// ---- files ----
static char *ds_read(const char *path, size_t *n, size_t max) {   // the whole file, NUL-terminated (malloc); NULL when it is not there or bigger than max
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    char *b = NULL; size_t len = 0, cap = 0; char t[65536]; size_t k;
    while ((k = fread(t, 1, sizeof t, f)) > 0) {
        if (len + k > max) { free(b); fclose(f); return NULL; }
        if (len + k + 1 > cap) { cap = (len + k + 1) * 2; char *nb = realloc(b, cap); if (!nb) { free(b); fclose(f); return NULL; } b = nb; }
        memcpy(b + len, t, k); len += k;
    }
    fclose(f); if (!b) { b = malloc(1); if (!b) return NULL; }
    b[len] = 0; if (n) *n = len; return b;
}
static int ds_write(const char *path, const void *d, size_t n) {   // through <path>.new and a rename
    char t[1100]; snprintf(t, sizeof t, "%s.new", path); FILE *f = fopen(t, "wb"); if (!f) return -1;
    int ok = fwrite(d, 1, n, f) == n; ok &= fflush(f) == 0; ok &= fclose(f) == 0;
    return ok && !ds_rename(t, path) ? 0 : -1;
}
static int64_t ds_size(const char *path) { ds_stat_t st; return ds_stat(path, &st) ? -1 : (st.st_mode & S_IFMT) == S_IFREG ? (int64_t)st.st_size : -2; }
static int ds_mkdirs(char *path) {   // the folders of a file's path (path is restored)
    for (char *s = path + 1; *s; s++) if (*s == '/') { *s = 0; int r = ds_mkdir(path); int e = errno; *s = '/'; if (r && e != EEXIST) return -1; }
    return 0;
}

// ---- the server's address of a file: base (ending in /) + the path with each byte but the unreserved ones and / percent-encoded (RomFS names have spaces, # and UTF-8) ----
static int ds_url(char *out, size_t cap, const char *base, const char *rel) {   // 0, or -1 when it does not fit
    size_t n = (size_t)snprintf(out, cap, "%s", base); if (n >= cap) return -1;
    for (const unsigned char *s = (const unsigned char *)rel; *s; s++) {
        int keep = (*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || strchr("-._~/", *s);
        if (n + (keep ? 1 : 3) >= cap) return -1;
        if (keep) out[n++] = (char)*s; else n += (size_t)snprintf(out + n, cap - n, "%%%02X", *s);
    }
    out[n] = 0; return 0;
}
static size_t ds_base64(char *out, const char *in, size_t n) {   // out holds 4 * ((n + 2) / 3) + 1
    static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"; size_t k = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)(unsigned char)in[i] << 16 | (i + 1 < n ? (uint32_t)(unsigned char)in[i + 1] << 8 : 0) | (i + 2 < n ? (unsigned char)in[i + 2] : 0);
        out[k++] = A[v >> 18 & 63]; out[k++] = A[v >> 12 & 63]; out[k++] = i + 1 < n ? A[v >> 6 & 63] : '='; out[k++] = i + 2 < n ? A[v & 63] : '=';
    }
    out[k] = 0; return k;
}
// the APK's asset of the server (porter/publish.py source_asset): "NX2S", 1, then "url\ncred" masked. The mask is no protection, only against reading the password by accident
static int ds_source(const uint8_t *a, size_t n, char *url, size_t ucap, char *cred, size_t ccap) {
    static const char M[] = "nx2a-data-source:not-a-secret!"; const size_t ml = sizeof M - 1;
    if (n < 6 || memcmp(a, "NX2S\x01", 5) || n - 5 > 2048) return -1;
    char t[2049]; size_t k = n - 5;
    for (size_t i = 0; i < k; i++) t[i] = (char)(a[5 + i] ^ (uint8_t)M[i % ml] ^ (uint8_t)(i * 131 & 255));
    t[k] = 0; char *nl = strchr(t, '\n'); if (!nl) return -1;
    *nl = 0; if (strlen(t) >= ucap || strlen(nl + 1) >= ccap) return -1;
    strcpy(url, t); strcpy(cred, nl + 1); memset(t, 0, sizeof t); return 0;
}

// ---- the sync ----
typedef int (*ds_sink)(void *u, const void *b, size_t n);   // 0 to go on
// fetch: rel (a path in the pack, '/'-separated, not escaped) from byte from on; *start: where the data the sink gets begins (0 when the server sent the whole file);
// *total: the file's size when known (else -1). Returns 0, DS_ERR_NET (no answer, a break), DS_ERR_SERVER (an HTTP error: *http its status)
typedef int (*ds_fetch_fn)(void *u, const char *rel, uint64_t from, uint64_t *start, int64_t *total, int *http, ds_sink sink, void *su);
typedef struct { int phase, files, file_no; uint64_t total, done, net; char file[256]; } ds_progress;   // net: bytes that came over the network (the speed); done also counts a resumed part's bytes
typedef struct {
    const char *root, *package;                       // the files folder; the app's package (a list of another app's pack is refused)
    ds_fetch_fn fetch; void *fu;
    uint64_t (*free_bytes)(void *u, const char *root); void *bu;   // NULL: not checked
    void (*note)(void *u, const ds_progress *p); void *nu;         // progress, from the sync's own thread
    void (*log)(void *u, const char *msg); void *lu;
    ds_progress pr; char err[256];
} ds_ctx;
#define DS_LOG(c, ...) do { if ((c)->log) { char _m[512]; snprintf(_m, sizeof _m, __VA_ARGS__); (c)->log((c)->lu, _m); } } while (0)
static void ds_note(ds_ctx *c) { if (c->note) c->note(c->nu, &c->pr); }
static void ds_path(char *out, size_t cap, const ds_ctx *c, const char *rel) { snprintf(out, cap, "%s/%s", c->root, rel); }
static const char *ds_local_name(const char *rel) { return !strcmp(rel, "env.txt") ? DS_ENV_PACK : rel; }   // where the pack's copy of a file is kept

typedef struct { char *b; size_t n, cap, max; } ds_mem;
static int ds_mem_sink(void *u, const void *b, size_t n) { ds_mem *m = u; if (m->n + n > m->max) return 1;
    if (m->n + n + 1 > m->cap) { size_t c = (m->n + n + 1) * 2; char *nb = realloc(m->b, c); if (!nb) return 1; m->b = nb; m->cap = c; }
    memcpy(m->b + m->n, b, n); m->n += n; m->b[m->n] = 0; return 0; }

static int ds_list_load(const ds_ctx *c, const char *name, ds_list *l) {
    char p[1100]; ds_path(p, sizeof p, c, name); size_t n; char *t = ds_read(p, &n, 256u << 20);
    int r = t ? ds_parse(t, n, l) : -1; free(t); if (r) memset(l, 0, sizeof *l); return r;
}
static int ds_complete(const ds_ctx *c, const ds_list *l) {   // every file of the list in the folder at its size
    char p[1100];
    for (int i = 0; i < l->n; i++) {
        if (!strcmp(l->f[i].path, "settings.txt")) continue;   // the player's (it may have been rewritten since)
        ds_path(p, sizeof p, c, ds_local_name(l->f[i].path));
        if (ds_size(p) == (int64_t)l->f[i].size) continue;
        // a pack installed over adb (porter install) has the pack's env.txt as env.txt itself, and no env.pack.txt: Tales of Eternia Remastered so installed
        // waited on its server's HTTP 404 (no pack published yet) instead of starting from its whole pack, 2026-11-15
        if (!strcmp(l->f[i].path, "env.txt")) { ds_path(p, sizeof p, c, "env.txt"); if (ds_size(p) >= 0) continue; }
        return 0;
    }
    return 1;
}
static int ds_hash_file(const char *path, char out[65], ds_ctx *c, uint64_t *done) {   // 0, or -1 when it cannot be read
    FILE *f = fopen(path, "rb"); if (!f) return -1;
    ds_sha s; ds_sha_init(&s); static uint8_t b[1 << 20]; size_t k; int64_t t = 0;
    while ((k = fread(b, 1, sizeof b, f)) > 0) { ds_sha_update(&s, b, k); if (done) { *done += k; if ((t += (int64_t)k) >= (16 << 20)) { t = 0; ds_note(c); } } }
    int bad = ferror(f); fclose(f); if (bad) return -1;
    ds_sha_hex(&s, out); return 0;
}

// env.txt: the pack's rows, then the rows of the earlier env.txt that the player added: those that are neither the earlier pack's (env.pack.txt; without it, every row
// whose key the new pack does not set) nor set by the new pack, each with the comment right above it. A DM_ROMFS row (porter install --romfs-at's) goes: the RomFS
// downloaded here is in the files folder
static int ds_env_key(const char *line, char *key, size_t cap) {   // KEY of a KEY=VALUE line, 0 for a comment or anything else
    const char *s = line; while (*s == ' ') s++;
    const char *eq = strchr(s, '='); if (*s == '#' || !eq || eq == s || (size_t)(eq - s) >= cap) return 0;
    memcpy(key, s, (size_t)(eq - s)); key[eq - s] = 0; return 1;
}
static int ds_has_line(const char *text, const char *line, size_t n) {
    for (const char *s = text; s && *s;) { const char *e = strchr(s, '\n'); size_t k = e ? (size_t)(e - s) : strlen(s); if (k && s[k - 1] == '\r') k--;
        if (k == n && !memcmp(s, line, n)) return 1; s = e ? e + 1 : NULL; }
    return 0;
}
static __attribute__((unused)) int ds_has_key(const char *text, const char *key) {
    char k[128], line[1024];
    for (const char *s = text; s && *s;) { const char *e = strchr(s, '\n'); size_t n = e ? (size_t)(e - s) : strlen(s); if (n >= sizeof line) n = sizeof line - 1;
        memcpy(line, s, n); line[n] = 0; if (ds_env_key(line, k, sizeof k) && !strcmp(k, key)) return 1; s = e ? e + 1 : NULL; }
    return 0;
}
static char *ds_env_merge(const char *cur, const char *prev_pack, const char *pack) {   // malloc
    size_t cap = strlen(pack) + (cur ? strlen(cur) : 0) + 128; char *out = malloc(cap); if (!out) return NULL;
    size_t n = (size_t)snprintf(out, cap, "%s", pack); if (n && out[n - 1] != '\n') out[n++] = '\n', out[n] = 0;
    int head = 0; const char *cmt = NULL; size_t cmtn = 0;
    for (const char *s = cur; s && *s;) {
        const char *e = strchr(s, '\n'); size_t k = e ? (size_t)(e - s) : strlen(s); if (k && s[k - 1] == '\r') k--;
        char line[1024], key[128]; size_t m = k < sizeof line - 1 ? k : sizeof line - 1; memcpy(line, s, m); line[m] = 0;
        if (ds_env_key(line, key, sizeof key)) {
            // the player's: any row that is not one of the earlier pack's (nor the same row in the new pack), also one changing a key the pack sets (DM_VK_DRIVER of a
            // test: kept after the pack's rows, so it wins). A switch the pack itself dropped is a row of the earlier pack: it goes
            int mine = !(prev_pack && ds_has_line(prev_pack, line, m)) && !ds_has_line(pack, line, m) && strcmp(key, "DM_ROMFS");
            if (mine) {
                if (!head) { n += (size_t)snprintf(out + n, cap - n, "# kept from the earlier env.txt (rows the data pack does not set)\n"); head = 1; }
                if (cmt) { memcpy(out + n, cmt, cmtn); n += cmtn; out[n++] = '\n'; }
                memcpy(out + n, line, m); n += m; out[n++] = '\n'; out[n] = 0;
            }
            cmt = NULL;
        } else if (m && line[0] == '#') { cmt = s; cmtn = m; } else cmt = NULL;
        s = e ? e + 1 : NULL;
    }
    return out;
}

typedef struct { FILE *f; ds_sha *s; ds_ctx *c; uint64_t got, cap, have; int64_t t; const uint64_t *start; const char *part; int begun; } ds_dl;
static int ds_dl_sink(void *u, const void *b, size_t n) {
    ds_dl *d = u;
    if (!d->begun) {   // the first bytes: where they start is known now. A server that ignored the Range header sends the whole file: the part starts again
        d->begun = 1;
        if (*d->start != d->have) {
            if (*d->start) return 3;
            if (!(d->f = freopen(d->part, "wb", d->f))) return 2;
            ds_sha_init(d->s); d->c->pr.done -= d->have; d->got = 0;
        }
    }
    if (d->got + n > d->cap) return 1;   // more than the list says: not this file
    if (fwrite(b, 1, n, d->f) != n) return 2;
    ds_sha_update(d->s, b, n); d->got += n; d->c->pr.done += n; d->c->pr.net += n;
    if ((d->t += (int64_t)n) >= (1 << 20)) { d->t = 0; ds_note(d->c); }
    return 0;
}
// One file into <dst>.part, resumed where an earlier try left it; 0 once it is complete and its hash is the list's (then renamed to dst)
static int ds_get(ds_ctx *c, const ds_file *f, const char *dst) {
    char part[1100]; snprintf(part, sizeof part, "%s.part", dst);
    if (ds_mkdirs(part)) { snprintf(c->err, sizeof c->err, "cannot make the folder of %s (errno %d)", f->path, errno); return DS_ERR_DISK; }
    int64_t have = ds_size(part); if (have < 0 || (uint64_t)have > f->size) have = 0;
    ds_sha s; ds_sha_init(&s);
    FILE *fp = fopen(part, have ? "r+b" : "wb"); if (!fp) { snprintf(c->err, sizeof c->err, "cannot write %s (errno %d)", part, errno); return DS_ERR_DISK; }
    c->pr.done += (uint64_t)have; ds_note(c);   // counted before the part is read back for the hash (a 1 GB part takes seconds: the bar showed 0% meanwhile)
    if (have) {   // the part already there goes into the hash
        static uint8_t b[1 << 20]; uint64_t left = (uint64_t)have; size_t k;
        while (left && (k = fread(b, 1, left < sizeof b ? (size_t)left : sizeof b, fp)) > 0) { ds_sha_update(&s, b, k); left -= k; }
        if (left) { c->pr.done -= (uint64_t)have; have = 0; ds_sha_init(&s); }
        ds_fseek(fp, have, SEEK_SET);
    }
    uint64_t start = 0; int64_t total = -1;
    ds_dl d = {fp, &s, c, (uint64_t)have, f->size, (uint64_t)have, 0, &start, part, 0};
    int rc = 0, http = 0;
    if ((uint64_t)have < f->size) rc = c->fetch(c->fu, f->path, (uint64_t)have, &start, &total, &http, ds_dl_sink, &d);   // (the fetch sets start before its first sink call)
    fp = d.f; if (!fp) { snprintf(c->err, sizeof c->err, "cannot write %s (errno %d)", part, errno); return DS_ERR_DISK; }
    int wbad = fflush(fp) || ferror(fp); fclose(fp);
    if (wbad) { snprintf(c->err, sizeof c->err, "cannot write %s (errno %d)", part, errno); return DS_ERR_DISK; }
    if (rc == DS_ERR_SERVER) { snprintf(c->err, sizeof c->err, "the server answered HTTP %d for %s", http, f->path); return rc; }
    if (rc) { snprintf(c->err, sizeof c->err, "the download of %s broke off at %llu of %llu bytes", f->path, (unsigned long long)d.got, (unsigned long long)f->size); return DS_ERR_NET; }
    if (d.got != f->size) { snprintf(c->err, sizeof c->err, "the download of %s ended at %llu of %llu bytes", f->path, (unsigned long long)d.got, (unsigned long long)f->size); return DS_ERR_NET; }
    char hex[65]; ds_sha_hex(&s, hex);
    if (strcmp(hex, f->sha)) { remove(part); c->pr.done -= f->size; snprintf(c->err, sizeof c->err, "%s came with other bytes than the list says (SHA-256 %.16s, not %.16s)", f->path, hex, f->sha); return DS_ERR_DATA; }
    if (ds_rename(part, dst)) { snprintf(c->err, sizeof c->err, "cannot rename %s (errno %d)", part, errno); return DS_ERR_DISK; }
    return 0;
}

// The whole sync: DS_DONE (files downloaded), DS_UPTODATE (nothing to do), DS_OFFLINE (no server, the folder holds a whole pack: start the game), else an error with
// c->err (DS_ERR_NET: try again later; a part already downloaded is kept)
static int ds_sync(ds_ctx *c) {
    memset(&c->pr, 0, sizeof c->pr); c->err[0] = 0; c->pr.phase = DS_PH_CONNECT; ds_note(c);
    ds_list local, prev, srv; int have_local = !ds_list_load(c, DS_LIST, &local), have_prev = !ds_list_load(c, DS_PREV, &prev);
    ds_mem m = {NULL, 0, 0, 256u << 20}; uint64_t st = 0; int64_t tot = -1; int http = 0;
    int rc = c->fetch(c->fu, DS_LIST, 0, &st, &tot, &http, ds_mem_sink, &m);
    if (rc || st) {
        free(m.b); m.b = NULL;
        int whole = have_local && !strcmp(local.package, c->package) && ds_complete(c, &local);
        if (!whole && !have_local && !have_prev) {   // a pack installed over adb (porter install): its manifest goes last, so with it the pack is whole
            char p[1100]; ds_path(p, sizeof p, c, DS_MANIFEST); size_t n; char *t = ds_read(p, &n, 1 << 20);
            char want[300]; snprintf(want, sizeof want, "\"package\": \"%s\"", c->package); whole = t && strstr(t, want); free(t);
        }
        snprintf(c->err, sizeof c->err, rc == DS_ERR_SERVER ? "the server answered HTTP %d for " DS_LIST : "the server could not be reached", http);
        if (have_local) ds_free(&local);
        if (have_prev) ds_free(&prev);
        return whole ? DS_OFFLINE : rc ? rc : DS_ERR_NET;
    }
    if (ds_parse(m.b ? m.b : "", m.n, &srv)) { free(m.b); snprintf(c->err, sizeof c->err, "the server's " DS_LIST " is not a data pack's list"); rc = DS_ERR_SERVER; goto out0; }
    if (strcmp(srv.package, c->package)) { snprintf(c->err, sizeof c->err, "the server's data is for %.100s, not this app (%.100s)", srv.package, c->package); rc = DS_ERR_PACKAGE; goto out1; }
    if (have_local && !strcmp(local.id, srv.id) && ds_complete(c, &srv)) { rc = DS_UPTODATE; goto out1; }
    // which files to download: not there at the list's size, or there at that size with another hash (the earlier list's, else the file's own)
    const ds_list *known = have_local ? &local : have_prev ? &prev : NULL;
    char *need = calloc((size_t)srv.n + 1, 1); char p[1100];
    if (!need) { snprintf(c->err, sizeof c->err, "out of memory"); rc = DS_ERR_DISK; goto out1; }
    c->pr.phase = DS_PH_CHECK; c->pr.files = srv.n;
    for (int i = 0; i < srv.n; i++) c->pr.total += srv.f[i].size;
    ds_note(c);
    uint64_t want = 0, parts = 0; int nneed = 0;
    for (int i = 0; i < srv.n; i++) {
        const ds_file *f = &srv.f[i]; c->pr.file_no = i + 1; snprintf(c->pr.file, sizeof c->pr.file, "%s", f->path);
        if (!strcmp(f->path, DS_LIST)) { c->pr.done += f->size; continue; }
        ds_path(p, sizeof p, c, ds_local_name(f->path));
        int64_t sz = ds_size(p);
        if (!strcmp(f->path, "settings.txt") && sz >= 0) { c->pr.done += f->size; continue; }   // the player's settings stay
        const ds_file *k = known ? ds_find(known, f->path) : NULL; char hex[65];
        if (sz == (int64_t)f->size && k && k->size == f->size && !strcmp(k->sha, f->sha)) { c->pr.done += f->size; continue; }
        if (sz == (int64_t)f->size) { uint64_t d0 = c->pr.done; if (!ds_hash_file(p, hex, c, &c->pr.done) && !strcmp(hex, f->sha)) continue; c->pr.done = d0 + f->size; }
        else c->pr.done += f->size;
        need[i] = 1; nneed++; want += f->size;
        char pp[1110]; snprintf(pp, sizeof pp, "%s.part", p); int64_t ps = ds_size(pp); if (ps > 0 && (uint64_t)ps <= f->size) parts += (uint64_t)ps;
    }
    DS_LOG(c, "data: %d of the server's %d files to download, %.1f MB (%.1f MB of them already in part files)", nneed, srv.n, want / 1048576.0, parts / 1048576.0);
    if (!nneed) goto finish;
    if (c->free_bytes) {
        uint64_t fr = c->free_bytes(c->bu, c->root), need_b = want - parts + (64u << 20);
        if (fr < need_b) { snprintf(c->err, sizeof c->err, "%.1f GB more space is needed; the device has %.1f GB free", (need_b - fr) / 1073741824.0, fr / 1073741824.0); rc = DS_ERR_SPACE; free(need); goto out1; }
    }
    if (have_local) {   // the folder is no longer one whole pack until the new list is written: an app without the server must not start a mix of two packs
        char a[1100], b[1100]; ds_path(a, sizeof a, c, DS_LIST); ds_path(b, sizeof b, c, DS_PREV); ds_rename(a, b);
    }
    c->pr.phase = DS_PH_DOWNLOAD; c->pr.total = want; c->pr.done = 0; c->pr.files = nneed; c->pr.file_no = 0; ds_note(c);
    // the pack's own files first; env.txt and the manifest last of all
    for (int pass = 0; pass < 2; pass++) for (int i = 0; i < srv.n; i++) {
        const ds_file *f = &srv.f[i]; int late = !strcmp(f->path, "env.txt") || !strcmp(f->path, DS_MANIFEST);
        if (!need[i] || late != pass) continue;
        c->pr.file_no++; snprintf(c->pr.file, sizeof c->pr.file, "%s", f->path); ds_note(c);
        ds_path(p, sizeof p, c, !strcmp(f->path, "env.txt") ? DS_ENV_PACK ".new" : f->path);
        if ((rc = ds_get(c, f, p))) { DS_LOG(c, "data: %s", c->err); free(need); goto out1; }
        if (!strcmp(f->path, "env.txt")) {   // env.txt: the pack's, with the player's rows
            char e[1100], q[1100]; ds_path(e, sizeof e, c, "env.txt"); ds_path(q, sizeof q, c, DS_ENV_PACK);
            char *cur = ds_read(e, NULL, 1 << 20), *old = ds_read(q, NULL, 1 << 20), *pk = ds_read(p, NULL, 1 << 20);
            char *mg = pk ? ds_env_merge(cur, old, pk) : NULL;
            int bad = !mg || ds_write(e, mg, strlen(mg)) || ds_rename(p, q);
            free(cur); free(old); free(pk); free(mg);
            if (bad) { snprintf(c->err, sizeof c->err, "cannot write env.txt (errno %d)", errno); rc = DS_ERR_DISK; free(need); goto out1; }
        }
    }
    free(need); need = NULL;
finish:
    free(need);
    c->pr.phase = DS_PH_FINISH; ds_note(c);
    ds_path(p, sizeof p, c, DS_LIST);
    if (ds_write(p, m.b, m.n)) { snprintf(c->err, sizeof c->err, "cannot write " DS_LIST " (errno %d)", errno); rc = DS_ERR_DISK; goto out1; }
    ds_path(p, sizeof p, c, DS_PREV); remove(p);
    rc = nneed ? DS_DONE : DS_UPTODATE;
out1: ds_free(&srv);
out0: free(m.b);
    if (have_local) ds_free(&local);
    if (have_prev) ds_free(&prev);
    return rc;
}
#endif
