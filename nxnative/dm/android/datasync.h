// The app's data from a web server (an APK ported with porter --data-url): the published pack's files.json (porter/publish.py) compared with the copy the app keeps in its
// files folder, and the files that are missing or changed downloaded, each into <name>.part (resumed with a Range request after a break), checked against its SHA-256,
// then renamed into place. files.json is written last: a files folder with it holds the whole pack, so an app that cannot reach the server starts the game from it.
// The player's own files stay: settings.txt is downloaded only when there is none; env.txt is the pack's with the rows the player added kept (ds_env_merge); a file the
// server no longer lists is left alone. No Android here: the transport is a callback (front_android.c: java.net.HttpURLConnection through JNI) and a test on the computer
// drives it with a folder (C:\d2rm\tools\easy-install\t_sync.c).
// A launch with nothing new costs little also for a pack of many files (Tales of Eternia Remastered: 33,696 files, an 8.6 MB files.json downloaded and every file's
// size read on the /sdcard at each launch kept the "connecting" screen up 10-15 s on a Retroid Pocket 6, 2026-11-15): the list is asked for only if it changed
// (the ETag / Last-Modified of the copy the folder has, kept in files.json.etag: HTTP 304 instead of the list), and a folder whose list is the server's is checked
// by a sample of its files (ds_sample), not by all of them.
// A pack with bundles (files.json format 2: porter/publish.py, android.tsv data_bundles) lists every game file as before, and its small ones with the bundle they are in:
// the server has those only inside their bundle (bundles/bNNNN.tar, an uncompressed tar). What the folder lacks is decided file by file as for any pack; the bundles that
// hold a needed file are downloaded (resumed as any file), or taken from the folder when porter install pushed them there, checked against their SHA-256, the needed files
// taken out (written at their names by several threads, at the list's size: ds_untar), and the bundle deleted: one request per 64 MB instead of one per file, and no file
// made through Android's FUSE layer (the app's own folder is a bind mount of the file system below, where adb's shell goes through FUSE). An APK without a server
// unpacks bundles porter install pushed the same way (ds_ctx.fetch NULL).
#ifndef DATASYNC_H
#define DATASYNC_H
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#ifdef _WIN32
#include <direct.h>
#define ds_mkdir(p) _mkdir(p)
typedef struct _stat64 ds_stat_t;
#define ds_stat(p, s) _stat64(p, s)
static inline int ds_rename(const char *a, const char *b) { remove(b); return rename(a, b); }
#define ds_fseek _fseeki64
#else
#define ds_mkdir(p) mkdir(p, 0775)   // group-writable (drwxrws--- under the app's umask 007), as the folders Android makes there: porter install (adb's shell, the same group)
                                     // pushes into a folder the app made (a bundles folder it began downloading into; a RomFS folder of a downloaded pack), which 0755 refused
typedef struct stat ds_stat_t;
#define ds_stat(p, s) stat(p, s)
#define ds_rename rename
#define ds_fseek fseeko
#endif

#define DS_LIST "files.json"
#define DS_PREV "files.json.prev"     // the list of the pack the folder held before a download started (its hashes save rehashing files that did not change)
#define DS_ENV_PACK "env.pack.txt"    // the pack's env.txt as downloaded: the rows the player added are those of env.txt that are not here (ds_env_merge)
#define DS_VAL "files.json.etag"      // the id of the folder's list, and the ETag and Last-Modified of the server's files.json it was taken from (ds_val_load)
#define DS_MANIFEST "nx2a-data.json"
#define DS_BUNDLES "bundles"          // the bundles' folder in the files folder (porter install pushes them there; downloaded ones go there too), removed once empty
#define DS_FORMAT 2                   // the newest files.json this app reads (1: every file on its own; 2: with bundles)
// DS_ERR_LOCAL: no server (an APK ported without --data-url) and the files folder does not hold a whole pack once its bundles are unpacked (a bundle or a file missing)
enum { DS_DONE = 0, DS_UPTODATE, DS_OFFLINE, DS_ERR_NET, DS_ERR_SERVER, DS_ERR_PACKAGE, DS_ERR_SPACE, DS_ERR_DATA, DS_ERR_DISK, DS_ERR_LOCAL };
enum { DS_PH_CONNECT, DS_PH_CHECK, DS_PH_DOWNLOAD, DS_PH_FINISH, DS_PH_UNPACK };

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
typedef struct { char *path; uint64_t size; char sha[65]; int bundle; } ds_file;   // bundle: its index in the list's bundles, -1 for a file the server has on its own
typedef struct { char *path; uint64_t size; char sha[65]; } ds_bundle;
// ix: the files by path (an open-addressing table of index + 1, ixn slots): a pack of 207,000 files looked up one by one through the list took hours
typedef struct { char id[65], package[256], version[64], display[64], variants[8][16]; int nvariants, n, cap, format, nb, bcap; ds_file *f; ds_bundle *b; uint64_t bytes; int *ix; uint32_t ixn; } ds_list;
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
static void ds_free(ds_list *l) { for (int i = 0; i < l->n; i++) free(l->f[i].path); for (int i = 0; i < l->nb; i++) free(l->b[i].path); free(l->f); free(l->b); free(l->ix); memset(l, 0, sizeof *l); }
static int ds_rel_ok(const char *path) {   // a path the app can write under its folder: relative, no empty, . or .. parts, no backslash
    if (!path[0] || path[0] == '/' || strchr(path, '\\')) return 0;
    for (const char *s = path; *s;) { const char *e = strchr(s, '/'); size_t k = e ? (size_t)(e - s) : strlen(s);
        if (!k || (k == 1 && s[0] == '.') || (k == 2 && s[0] == '.' && s[1] == '.')) return 0; s += k + (e != NULL); if (e && !*s) return 0; }
    return 1;
}
// one entry of "files" (bundle: its "bundle", -1 without) or of "bundles" (bundle NULL); each takes path, size, sha256
static int ds_file_obj(ds_js *j, char **out, uint64_t *size, char sha[65], int *bundle) {
    char key[32], path[1024] = ""; *size = (uint64_t)-1; sha[0] = 0; if (bundle) *bundle = -1;
    ds_ws(j); if (j->p >= j->e || *j->p++ != '{') return -1;
    ds_ws(j); if (j->p < j->e && *j->p == '}') { j->p++; return -1; }
    for (;;) {
        if (ds_str(j, key, sizeof key)) return -1;
        ds_ws(j); if (j->p >= j->e || *j->p++ != ':') return -1;
        if (!strcmp(key, "path")) { if (ds_str(j, path, sizeof path)) return -1; }
        else if (!strcmp(key, "size")) { if (ds_u64(j, size)) return -1; }
        else if (!strcmp(key, "sha256")) { if (ds_str(j, sha, 65)) return -1; }
        else if (bundle && !strcmp(key, "bundle")) { uint64_t b; if (ds_u64(j, &b) || b > 1u << 30) return -1; *bundle = (int)b; }
        else if (ds_skip(j, 1)) return -1;
        ds_ws(j); if (j->p >= j->e) return -1;
        if (*j->p == ',') { j->p++; continue; }
        if (*j->p++ != '}') return -1;
        break;
    }
    if (!ds_rel_ok(path) || strlen(sha) != 64 || *size == (uint64_t)-1) return -1;
    *out = strdup(path); return *out ? 0 : -1;
}
static uint32_t ds_hash(const char *s) { uint32_t h = 2166136261u; while (*s) h = (h ^ (uint8_t)*s++) * 16777619u; return h; }   // FNV-1a
static int ds_index(ds_list *l) {   // the table of l's files by path (a path listed twice: its first entry, as the search through the list found); -1 out of memory
    l->ixn = 64; while (l->ixn < 2u * (uint32_t)l->n) l->ixn *= 2;
    if (!(l->ix = calloc(l->ixn, sizeof *l->ix))) return -1;
    for (int i = 0; i < l->n; i++) {
        uint32_t k = ds_hash(l->f[i].path) & (l->ixn - 1); int dup = 0;
        while (l->ix[k] && !(dup = !strcmp(l->f[l->ix[k] - 1].path, l->f[i].path))) k = (k + 1) & (l->ixn - 1);
        if (!dup) l->ix[k] = i + 1;
    }
    return 0;
}
static int ds_find_i(const ds_list *l, const char *path) {   // the index of a path in l's files, -1 when it lists none
    if (!l || !l->ix) return -1;
    for (uint32_t k = ds_hash(path) & (l->ixn - 1); l->ix[k]; k = (k + 1) & (l->ixn - 1)) if (!strcmp(l->f[l->ix[k] - 1].path, path)) return l->ix[k] - 1;
    return -1;
}
static int ds_parse(const char *txt, size_t n, ds_list *l) {   // 0, or -1 for a list that is not the pack's (the reason is not kept: the server's file is what it is)
    memset(l, 0, sizeof *l); ds_js j = {txt, txt + n}; char key[32]; uint64_t fmt = 1;
    ds_ws(&j); if (j.p >= j.e || *j.p++ != '{') return -1;
    for (;;) {
        if (ds_str(&j, key, sizeof key)) goto bad;
        ds_ws(&j); if (j.p >= j.e || *j.p++ != ':') goto bad;
        if (!strcmp(key, "id")) { if (ds_str(&j, l->id, sizeof l->id)) goto bad; }
        else if (!strcmp(key, "package")) { if (ds_str(&j, l->package, sizeof l->package)) goto bad; }
        else if (!strcmp(key, "version")) { if (ds_str(&j, l->version, sizeof l->version)) goto bad; }
        else if (!strcmp(key, "display")) { if (ds_str(&j, l->display, sizeof l->display)) goto bad; }
        else if (!strcmp(key, "bytes")) { if (ds_u64(&j, &l->bytes)) goto bad; }
        else if (!strcmp(key, "format")) { if (ds_u64(&j, &fmt) || fmt > 1000) goto bad; }
        else if (!strcmp(key, "variants") || !strcmp(key, "files") || !strcmp(key, "bundles")) {
            int what = key[0] == 'f' ? 1 : key[0] == 'b' ? 2 : 0; ds_ws(&j); if (j.p >= j.e || *j.p++ != '[') goto bad;
            ds_ws(&j); if (j.p < j.e && *j.p == ']') j.p++;
            else for (;;) {
                if (what == 1) {
                    if (l->n == l->cap) { int c = l->cap ? l->cap * 2 : 256; ds_file *f = realloc(l->f, (size_t)c * sizeof *f); if (!f) goto bad; l->f = f; l->cap = c; }
                    ds_file *f = &l->f[l->n]; if (ds_file_obj(&j, &f->path, &f->size, f->sha, &f->bundle)) goto bad;
                    l->n++;
                } else if (what == 2) {
                    if (l->nb == l->bcap) { int c = l->bcap ? l->bcap * 2 : 64; ds_bundle *b = realloc(l->b, (size_t)c * sizeof *b); if (!b) goto bad; l->b = b; l->bcap = c; }
                    ds_bundle *b = &l->b[l->nb]; if (ds_file_obj(&j, &b->path, &b->size, b->sha, NULL)) goto bad;
                    l->nb++;
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
    l->format = (int)fmt;
    for (int i = 0; i < l->n; i++) if (l->f[i].bundle >= l->nb) goto bad;   // (a list of format 1 has neither)
    if (ds_index(l)) goto bad;
    return 0;
bad: ds_free(l); return -1;
}
static const ds_file *ds_find(const ds_list *l, const char *path) { int i = ds_find_i(l, path); return i < 0 ? NULL : &l->f[i]; }

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
#ifdef DS_COUNT_STATS   // the test (t_sync.c) counts the files whose size is read: a sync of a pack that did not change reads a sample's, not every file's
static long ds_stats;
#define DS_STAT_COUNTED() (ds_stats++)
#else
#define DS_STAT_COUNTED() ((void)0)
#endif
static int64_t ds_size(const char *path) { DS_STAT_COUNTED(); ds_stat_t st; return ds_stat(path, &st) ? -1 : (st.st_mode & S_IFMT) == S_IFREG ? (int64_t)st.st_size : -2; }
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
// *total: the file's size when known (else -1). Returns 0, DS_ERR_NET (no answer, a break), DS_ERR_SERVER (an HTTP error: *http its status).
// cond and got, for files.json only (NULL for the pack's files): cond, when not NULL, holds the validators of the copy the folder has, sent as If-None-Match and
// If-Modified-Since (each when not ""); a server whose file is still that one answers HTTP 304: returned as 0 with *http 304, *start 0 and the sink not called.
// got: the answer's ETag and Last-Modified, "" for one it has not (or one longer than the field)
typedef struct { char etag[128], modified[64]; } ds_val;
typedef int (*ds_fetch_fn)(void *u, const char *rel, uint64_t from, uint64_t *start, int64_t *total, int *http, const ds_val *cond, ds_val *got, ds_sink sink, void *su);
typedef struct { int phase, files, file_no; uint64_t total, done, net; char file[256]; } ds_progress;   // net: bytes that came over the network (the speed); done also counts a resumed part's bytes
typedef struct {
    const char *root, *package;                       // the files folder; the app's package (a list of another app's pack is refused)
    ds_fetch_fn fetch; void *fu;
    uint64_t (*free_bytes)(void *u, const char *root); void *bu;   // NULL: not checked
    void (*note)(void *u, const ds_progress *p); void *nu;         // progress, from the sync's own thread
    void (*log)(void *u, const char *msg); void *lu;
    ds_progress pr; char err[256];
    uint64_t space_need, space_free;   // DS_ERR_SPACE: the bytes more that are needed, and the bytes free (the screen says them in the app's language; err is the log's)
} ds_ctx;
#define DS_LOG(c, ...) do { if ((c)->log) { char _m[512]; snprintf(_m, sizeof _m, __VA_ARGS__); (c)->log((c)->lu, _m); } } while (0)
static void ds_note(ds_ctx *c) { if (c->note) c->note(c->nu, &c->pr); }
static void ds_path(char *out, size_t cap, const ds_ctx *c, const char *rel) { snprintf(out, cap, "%s/%s", c->root, rel); }
static const char *ds_local_name(const char *rel) { return !strcmp(rel, "env.txt") ? DS_ENV_PACK : rel; }   // where the pack's copy of a file is kept

typedef struct { char *b; size_t n, cap, max; } ds_mem;
static int ds_mem_sink(void *u, const void *b, size_t n) { ds_mem *m = u; if (m->n + n > m->max) return 1;
    if (m->n + n + 1 > m->cap) { size_t c = (m->n + n + 1) * 2; char *nb = realloc(m->b, c); if (!nb) return 1; m->b = nb; m->cap = c; }
    memcpy(m->b + m->n, b, n); m->n += n; m->b[m->n] = 0; return 0; }

static int ds_list_load(const ds_ctx *c, const char *name, ds_list *l, char **raw, size_t *rn) {   // raw (when not NULL): the file's bytes, kept (malloc) when it parsed
    char p[1100]; ds_path(p, sizeof p, c, name); size_t n = 0; char *t = ds_read(p, &n, 256u << 20);
    int r = t ? ds_parse(t, n, l) : -1; if (r) memset(l, 0, sizeof *l);
    if (!r && l->format > DS_FORMAT) { ds_free(l); r = -1; }   // a newer app's list (the app put back to an older APK): as if there were none
    if (raw && !r) { *raw = t; *rn = n; } else free(t);
    return r;
}
// Whether the files folder has a bundles folder: bundles to unpack (pushed by porter install, or left by an unpack that stopped part way). One stat: the folder goes once empty
static int ds_bundles_dir(const char *root) { char p[1100]; snprintf(p, sizeof p, "%s/" DS_BUNDLES, root); return ds_size(p) == -2; }
// The same for a folder whose list has no bundles (or no list): one more stat at every start, of a directory and not of a file of the pack, so not counted with the sample's sizes
// (DS_STAT_COUNTED: the tests hold the sample to its size)
static int ds_bundles_dir_any(const char *root) { char p[1100]; ds_stat_t st; snprintf(p, sizeof p, "%s/" DS_BUNDLES, root); return !ds_stat(p, &st) && (st.st_mode & S_IFMT) != S_IFREG; }
// Whether a folder with a list of bundles is not known to be whole: its bundles folder is there. That folder goes only once the folder holds every file (ds_clean_bundles),
// so while it is there every file is checked (not a sample): an unpack that refused a bundle deleted it, and the files it held are missing until it comes again
static int ds_pending(const ds_ctx *c, const ds_list *l) { return l->nb && ds_bundles_dir(c->root); }
static int ds_file_ok(const ds_ctx *c, const ds_file *f) {   // the file in the folder at the list's size
    char p[1100];
    if (!strcmp(f->path, "settings.txt")) return 1;   // the player's (it may have been rewritten since)
    ds_path(p, sizeof p, c, ds_local_name(f->path));
    if (ds_size(p) == (int64_t)f->size) return 1;
    // a pack installed over adb (porter install) has the pack's env.txt as env.txt itself, and no env.pack.txt: Tales of Eternia Remastered so installed
    // waited on its server's HTTP 404 (no pack published yet) instead of starting from its whole pack, 2026-11-15
    if (!strcmp(f->path, "env.txt")) { ds_path(p, sizeof p, c, "env.txt"); if (ds_size(p) >= 0) return 1; }
    return 0;
}
static int ds_complete(const ds_ctx *c, const ds_list *l) { for (int i = 0; i < l->n; i++) if (!ds_file_ok(c, &l->f[i])) return 0; return 1; }   // every file of the list
// The folder's pack when the server's list is the folder's own (the same id, or HTTP 304): files.json is written after the last of its files, so the folder held every
// one of them then. What may have gone since (a file removed by hand, a card's fault) is looked for in a sample, as ds_complete looks at each file: the DS_SAMPLE
// largest files (the ones an interrupted copy cuts), DS_SAMPLE at even steps through the list (it is in path order: every part of the folder tree), and the two the
// download writes last (env.txt, the manifest). At most 2 * DS_SAMPLE + 2 sizes read, not one per file (33,696 took 10-15 s on a Retroid Pocket 6's /sdcard); a pack
// of up to 4 * DS_SAMPLE files has every file looked at. 1: the sample is there; 0: a file is missing (the caller then checks every file and downloads what is not
// there). *looked: the files looked at, for the log
#define DS_SAMPLE 32
static int ds_sample(const ds_ctx *c, const ds_list *l, int *looked) {
    *looked = 0;
    if (l->n <= 4 * DS_SAMPLE) { *looked = l->n; return ds_complete(c, l); }
    int big[DS_SAMPLE], nb = 0;   // the largest, largest first (an insertion into the short array: the list itself stays in its order)
    for (int i = 0; i < l->n; i++) {
        uint64_t s = l->f[i].size; if (nb == DS_SAMPLE && s <= l->f[big[nb - 1]].size) continue;
        int k = nb < DS_SAMPLE ? nb++ : nb - 1;
        while (k && l->f[big[k - 1]].size < s) { big[k] = big[k - 1]; k--; }
        big[k] = i;
    }
    for (int k = 0; k < nb; k++) { ++*looked; if (!ds_file_ok(c, &l->f[big[k]])) return 0; }
    for (int k = 0; k < DS_SAMPLE; k++) { ++*looked; if (!ds_file_ok(c, &l->f[(2 * (int64_t)k + 1) * l->n / (2 * DS_SAMPLE)])) return 0; }   // the middle of each step
    for (int i = 0; i < l->n; i++)
        if (!strcmp(l->f[i].path, "env.txt") || !strcmp(l->f[i].path, DS_MANIFEST)) { ++*looked; if (!ds_file_ok(c, &l->f[i])) return 0; }
    return 1;
}

// files.json.etag: "<id of the folder's list>\n<ETag>\n<Last-Modified>\n", the validators of the server's files.json that list was taken from, written once a sync
// ends with that list (and removed when a download starts: the list is then files.json.prev). The id ties them to the list: validators of another list are not sent
static int ds_val_text(const char *s) { for (; *s; s++) if ((unsigned char)*s < 0x20 || (unsigned char)*s > 0x7e) return 0; return 1; }   // a header value as it can be sent
static int ds_val_load(const ds_ctx *c, const char *id, ds_val *v) {   // 1: the folder has validators for the list of that id
    memset(v, 0, sizeof *v); char p[1100]; ds_path(p, sizeof p, c, DS_VAL); char *t = ds_read(p, NULL, 4096); if (!t) return 0;
    char *a = strchr(t, '\n'), *b = a ? strchr(a + 1, '\n') : NULL, *e = b ? strchr(b + 1, '\n') : NULL;
    int ok = e && a - t == 64 && !memcmp(t, id, 64) && (size_t)(b - a - 1) < sizeof v->etag && (size_t)(e - b - 1) < sizeof v->modified;
    if (ok) { memcpy(v->etag, a + 1, (size_t)(b - a - 1)); memcpy(v->modified, b + 1, (size_t)(e - b - 1)); }
    free(t);
    ok = ok && (v->etag[0] || v->modified[0]) && ds_val_text(v->etag) && ds_val_text(v->modified);   // (one that cannot be sent would fail every request)
    if (!ok) memset(v, 0, sizeof *v);
    return ok;
}
static void ds_val_save(const ds_ctx *c, const char *id, const ds_val *v, const ds_val *had) {   // had: the folder's (NULL: none), not written again when the same
    char p[1100], t[300]; ds_path(p, sizeof p, c, DS_VAL);
    if (had && !strcmp(had->etag, v->etag) && !strcmp(had->modified, v->modified)) return;
    if (!(v->etag[0] || v->modified[0]) || !ds_val_text(v->etag) || !ds_val_text(v->modified)) { remove(p); return; }   // a server without them: the list comes whole
    int n = snprintf(t, sizeof t, "%s\n%s\n%s\n", id, v->etag, v->modified);
    if (n <= 0 || (size_t)n >= sizeof t || ds_write(p, t, (size_t)n)) remove(p);   // without them the next sync downloads the list again: no harm
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
    if ((uint64_t)have < f->size) rc = c->fetch(c->fu, f->path, (uint64_t)have, &start, &total, &http, NULL, NULL, ds_dl_sink, &d);   // (the fetch sets start before its first sink call)
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

// ---- bundles: tar archives as porter writes them (porter/datapack.py write_bundle: GNU format, regular files, GNU long names; POSIX ustar names read too) ----
static uint64_t ds_tar_num(const uint8_t *s, size_t n, int *bad) {   // a header's number: octal digits with spaces or NULs around them, or GNU's base-256 (first byte 0x80)
    uint64_t v = 0; size_t i = 0;
    if (n && s[0] & 0x80) {
        if (s[0] != 0x80) { *bad = 1; return 0; }   // (negative)
        for (i = 1; i < n; i++) { if (v >> 56) { *bad = 1; return 0; } v = v << 8 | s[i]; }
        return v;
    }
    while (i < n && s[i] == ' ') i++;
    size_t d = i;
    while (i < n && s[i] >= '0' && s[i] <= '7') { if (v >> 61) { *bad = 1; return 0; } v = v << 3 | (uint64_t)(s[i] - '0'); i++; }
    if (i == d) *bad = 1;
    while (i < n && (s[i] == ' ' || !s[i])) i++;
    if (i != n) *bad = 1;
    return v;
}
// Bundles are taken out by DS_UNPACKERS threads, one bundle each at a time. The files folder's file system on a phone is f2fs mounted dirsync (a Retroid Pocket Nova,
// Android 13: /data/media, which the app's own Android/data is a bind mount of): every file made waits for a checkpoint, about 13 ms, while its folder is locked. One file
// after another Fire Emblem Engage's small files came out at about 75 a second (103,656 in 24 minutes); the checkpoints of files made in different folders at the same time
// are merged (checkpoint_merge), and bundles, which hold the files in path order, mostly hold different folders. A file is written once at its own name (a temporary file
// and a rename were two of those waits for no safety: nothing is synced, and a file cut short is caught by its size at the next check, while its bundle stays until every
// one of its files is written). On the computer the test builds with -DDS_PTHREADS (Windows needs -pthread); without it, one bundle after another on the calling thread
#if !defined(_WIN32) || defined(DS_PTHREADS)
#include <pthread.h>
#define DS_UNPACKERS 8
#else
#define DS_UNPACKERS 0
#endif
#define DS_FILE_SLACK 4096   // the space a file written takes beyond its bytes (f2fs: its node, and its last block rounded up), counted for each file to write (ds_apply)
static int ds_hash_buf(const char *path, char out[65], uint8_t *b, size_t cap) {   // ds_hash_file with the caller's buffer and no progress (a thread of its own); 0 or -1
    FILE *f = fopen(path, "rb"); if (!f) return -1;
    ds_sha s; ds_sha_init(&s); size_t k;
    while ((k = fread(b, 1, cap, f)) > 0) ds_sha_update(&s, b, k);
    int bad = ferror(f); fclose(f); if (bad) return -1;
    ds_sha_hex(&s, out); return 0;
}
typedef struct { void (*add)(void *u, uint64_t bytes, int files); void *u; char *err; size_t cap; } ds_uout;   // where ds_untar reports its progress and its error

// Bundle k's files of l that are needed (need[i], read and cleared only for bundle k's files: other threads take other bundles) out of the bundle file tar, whose SHA-256
// was checked, each written at its own name at the list's size; buf: 1 MB of the caller's. A member the list does not put in this bundle, or one not needed, is passed over.
// 0; DS_ERR_DATA (not such a tar: a damaged header, an entry that is not a file, an unsafe name (absolute, .., a backslash), a size other than the list's, the archive
// ending early, or a needed file not in it); DS_ERR_DISK; the reason in o->err. *wrote: the files written
static int ds_untar(const ds_ctx *c, const ds_list *l, int k, char *need, const char *tar, int *wrote, uint8_t *buf, const ds_uout *o) {
    *wrote = 0; const char *bn = l->b[k].path;
    int64_t tsz = ds_size(tar);   // the bundle file's size: no entry may reach past it (a size field of 2**64 - 512 made the skip seek back onto the same header: a loop)
    FILE *t = tsz >= 0 ? fopen(tar, "rb") : NULL; if (!t) { snprintf(o->err, o->cap, "cannot read %s (errno %d)", bn, errno); return DS_ERR_DISK; }
    uint64_t pos = 0;   // the bytes of the file passed so far (every entry's header, then its data padded to 512)
    int want = 0, lname = 0, rc = 0, step = 0; uint64_t bytes = 0; for (int i = 0; i < l->n; i++) want += l->f[i].bundle == k && need[i];
    uint8_t h[512]; char name[1024], dir[1100] = "", dst[1100];
#define DS_TAR_BAD(...) do { snprintf(o->err, o->cap, __VA_ARGS__); rc = DS_ERR_DATA; goto out; } while (0)
#define DS_TAR_DISK(...) do { snprintf(o->err, o->cap, __VA_ARGS__); rc = DS_ERR_DISK; goto out; } while (0)
    for (;;) {
        if (fread(h, 1, 512, t) != 512) DS_TAR_BAD("%s ends before its end blocks (cut short?)", bn);
        int zero = 1; for (int i = 0; i < 512 && zero; i++) zero = !h[i];
        if (zero) break;   // the end
        unsigned sum = 0; for (int i = 0; i < 512; i++) sum += i >= 148 && i < 156 ? ' ' : h[i];
        int bad = 0; uint64_t ck = ds_tar_num(h + 148, 8, &bad), size = ds_tar_num(h + 124, 12, &bad);
        if (bad || ck != sum) DS_TAR_BAD("%s has a damaged header after %d files", bn, *wrote);
        pos += 512;
        if (size > (uint64_t)tsz - pos || ((size + 511) & ~(uint64_t)511) > (uint64_t)tsz - pos) DS_TAR_BAD("%s has an entry that reaches past its end (a size of %llu bytes)", bn, (unsigned long long)size);
        uint64_t padded = (size + 511) & ~(uint64_t)511; pos += padded;   // (no overflow: size is at most what is left of the file)
        char type = (char)h[156];
        if (type == 'L') {   // a GNU long name, the next entry's
            if (!size || size >= sizeof name || fread(name, 1, (size_t)padded, t) != padded) DS_TAR_BAD("%s has a damaged long name", bn);
            name[size] = 0; lname = 1; continue;
        }
        if (!lname) {   // the header's own name: GNU's 100 bytes; POSIX ustar's after its prefix
            char nm[101]; memcpy(nm, h, 100); nm[100] = 0;
            if (!memcmp(h + 257, "ustar\0", 6) && h[345]) { char pf[156]; memcpy(pf, h + 345, 155); pf[155] = 0; snprintf(name, sizeof name, "%s/%s", pf, nm); }
            else snprintf(name, sizeof name, "%s", nm);
        }
        lname = 0;
        if (type == '5') { if (ds_fseek(t, (int64_t)padded, SEEK_CUR)) DS_TAR_BAD("%s cannot be read", bn); continue; }   // a folder: made with its files
        if (type != '0' && type != 0) DS_TAR_BAD("%s holds %s, an entry of type '%c' (a bundle holds files only)", bn, name, type >= 32 && type < 127 ? type : '?');
        if (!ds_rel_ok(name)) DS_TAR_BAD("%s holds a file the app does not write: %.200s", bn, name);
        int i = ds_find_i(l, name);
        if (i < 0 || l->f[i].bundle != k || !need[i]) { if (ds_fseek(t, (int64_t)padded, SEEK_CUR)) DS_TAR_BAD("%s cannot be read", bn); continue; }
        if (l->f[i].size != size) DS_TAR_BAD("%s holds %.200s at %llu bytes, the list says %llu", bn, name, (unsigned long long)size, (unsigned long long)l->f[i].size);
        ds_path(dst, sizeof dst, c, name);
        const char *sl = strrchr(dst, '/'); size_t dl = sl ? (size_t)(sl - dst) : 0;
        if (dl != strlen(dir) || memcmp(dir, dst, dl)) {   // a folder of its own: made once for its files (they come in path order)
            if (ds_mkdirs(dst)) DS_TAR_DISK("cannot make the folder of %.200s (errno %d)", name, errno);
            memcpy(dir, dst, dl); dir[dl] = 0;
        }
        FILE *w = fopen(dst, "wb"); if (!w) DS_TAR_DISK("cannot write %.200s (errno %d)", name, errno);
        uint64_t left = size; int werr = 0;
        while (left) {
            size_t n = left < (1u << 20) ? (size_t)left : (1u << 20);
            if (fread(buf, 1, n, t) != n) { fclose(w); remove(dst); DS_TAR_BAD("%s ends inside %.200s", bn, name); }
            if (!werr && fwrite(buf, 1, n, w) != n) werr = errno ? errno : -1;
            left -= n;
        }
        if (fflush(w) && !werr) werr = errno ? errno : -1;
        if (fclose(w) && !werr) werr = errno ? errno : -1;
        if (werr) { remove(dst); DS_TAR_DISK("cannot write %.200s (errno %d)", name, werr); }
        if (padded > size && ds_fseek(t, (int64_t)(padded - size), SEEK_CUR)) DS_TAR_BAD("%s cannot be read", bn);
        need[i] = 0; ++*wrote; bytes += size;
        if (++step == 64) { o->add(o->u, bytes, step); bytes = 0; step = 0; }
    }
    if (*wrote < want) {
        const char *first = ""; for (int i = 0; i < l->n; i++) if (l->f[i].bundle == k && need[i]) { first = l->f[i].path; break; }
        DS_TAR_BAD("%s lacks %d of the %d files the list puts in it (%.200s)", bn, want - *wrote, want, first);
    }
#undef DS_TAR_BAD
#undef DS_TAR_DISK
out:
    if (step) o->add(o->u, bytes, step);
    fclose(t);
    return rc;
}

// The bundles to take out (bs[k] 1: in the folder, its SHA-256 still to read; 3: downloaded, read already), shared by the unpackers: each takes the next one, reads it,
// takes its files out and deletes it. One the folder had with other bytes is deleted and left as bs[k] 4 (the caller downloads it, on its own thread: the server's
// connection belongs to it). The first error stops the others after their bundle
typedef struct {
    ds_ctx *c; const ds_list *l; char *need; uint8_t *bs; int next, rc, got, nt, mt; char err[256];   // mt: the lock is in use (set before the first thread starts)
#if DS_UNPACKERS
    pthread_mutex_t mu; pthread_t t[DS_UNPACKERS];
#endif
} ds_upk;
static void ds_upk_lock(ds_upk *u, int on) {
#if DS_UNPACKERS
    if (u->mt) { if (on) pthread_mutex_lock(&u->mu); else pthread_mutex_unlock(&u->mu); }
#endif
    (void)u; (void)on;
}
static void ds_upk_add(void *a, uint64_t bytes, int files) { ds_upk *u = a; ds_upk_lock(u, 1); u->c->pr.done += bytes; u->c->pr.file_no += files; ds_note(u->c); ds_upk_lock(u, 0); }
static void *ds_unpacker(void *a) {
    ds_upk *u = a; ds_ctx *c = u->c; const ds_list *l = u->l; char p[1100], err[256], hex[65];
    uint8_t *buf = malloc(1 << 20);
    for (;;) {
        ds_upk_lock(u, 1);
        while (u->next < l->nb && u->bs[u->next] != 1 && u->bs[u->next] != 3) u->next++;
        int k = !buf ? -1 : u->rc || u->next >= l->nb ? -1 : u->next++;
        if (!buf && !u->rc) { u->rc = DS_ERR_DISK; snprintf(u->err, sizeof u->err, "out of memory"); }
        if (k >= 0) { snprintf(c->pr.file, sizeof c->pr.file, "%s", l->b[k].path); ds_note(c); }
        ds_upk_lock(u, 0);
        if (k < 0) break;
        const ds_bundle *b = &l->b[k]; ds_path(p, sizeof p, c, b->path); hex[0] = 0;
        if (u->bs[k] == 1 && (ds_hash_buf(p, hex, buf, 1 << 20) || strcmp(hex, b->sha))) {   // the folder's bundle: not the list's (another pack's, or damaged)
            DS_LOG(c, "data: %s in the folder is not the list's (SHA-256 %.16s, not %.16s): deleted", b->path, hex, b->sha);
            if (remove(p)) DS_LOG(c, "[RED] data: cannot delete %s (errno %d)", b->path, errno);
            ds_upk_lock(u, 1); u->bs[k] = 4; ds_upk_lock(u, 0); continue;
        }
        ds_uout o = {ds_upk_add, u, err, sizeof err}; int n = 0;
        int r = ds_untar(c, l, k, u->need, p, &n, buf, &o);
        if ((!r || r == DS_ERR_DATA) && remove(p)) DS_LOG(c, "[RED] data: cannot delete %s (errno %d)", b->path, errno);   // (taken out; or not what the list says, downloaded again by the next try)
        ds_upk_lock(u, 1); u->got += n; if (r && !u->rc) { u->rc = r; snprintf(u->err, sizeof u->err, "%s", err); } ds_upk_lock(u, 0);
        if (r) DS_LOG(c, "data: %s", err);
    }
    free(buf);
    return NULL;
}
static void ds_unpack_all(ds_upk *u) {   // every bundle of bs 1 or 3, by DS_UNPACKERS threads (none started: this thread)
#if DS_UNPACKERS
    pthread_mutex_init(&u->mu, NULL); u->mt = 1;
    for (u->nt = 0; u->nt < DS_UNPACKERS; u->nt++) if (pthread_create(&u->t[u->nt], NULL, ds_unpacker, u)) break;
    if (u->nt) { for (int i = 0; i < u->nt; i++) pthread_join(u->t[i], NULL); u->mt = 0; pthread_mutex_destroy(&u->mu); return; }
    u->mt = 0; pthread_mutex_destroy(&u->mu);
#endif
    ds_unpacker(u);
}
#ifdef _WIN32
#define ds_rmdir(p) _rmdir(p)
#else
#define ds_rmdir(p) rmdir(p)
#endif
static void ds_clean_bundles(const ds_ctx *c) {   // the folder is whole: what is left in the bundles folder (a bundle none of whose files was needed, an older pack's, a part) goes, then the folder
    char d[1100], p[1400]; ds_path(d, sizeof d, c, DS_BUNDLES);
    DIR *dir = opendir(d); if (!dir) return;
    for (struct dirent *e; (e = readdir(dir));) if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
        snprintf(p, sizeof p, "%s/%s", d, e->d_name);
        if (remove(p)) DS_LOG(c, "[RED] data: cannot delete %s (errno %d)", p, errno);
    }
    closedir(dir); if (ds_rmdir(d)) DS_LOG(c, "[RED] data: cannot delete %s (errno %d)", d, errno);
}

// The files of list l the folder lacks or holds with other bytes (not there at l's size, or there at that size with another hash: known's, the folder's list or the one
// before an update began, else the file's own) made right: a file the server has on its own downloaded; a bundled one taken out of its bundle, the folder's own (porter install
// pushed it; its SHA-256 is read first) or downloaded, the bundles first and then each bundle's files (DS_PH_UNPACK), before env.txt and the manifest come last.
// local_id: the id of the folder's list (NULL: none), which goes aside (files.json.prev) before anything of another pack is written. fetch: 0 without a server, when only
// the folder's bundles can be used (DS_ERR_LOCAL once they are, if more is missing). *got: the files written. 0, or an error with c->err
static int ds_apply(ds_ctx *c, const ds_list *l, const ds_list *known, const char *local_id, int fetch, int *got) {
    *got = 0; char p[1100], pp[1110];
    char *need = calloc((size_t)l->n + 1, 1); uint8_t *bs = calloc((size_t)l->nb + 1, 1);   // bs: a bundle not needed (0), needed in the folder (1), to download (2), downloaded (3)
    if (!need || !bs) { free(need); free(bs); snprintf(c->err, sizeof c->err, "out of memory"); return DS_ERR_DISK; }
    int rc = 0, nneed = 0, nown = 0, nmem = 0, nbloc = 0, nbdl = 0, lack = 0;
    c->pr.phase = DS_PH_CHECK; c->pr.files = l->n;
    for (int i = 0; i < l->n; i++) c->pr.total += l->f[i].size;
    ds_note(c);
    uint64_t want = 0, parts = 0, unpack = 0, bwant = 0, maxb = 0;
    for (int i = 0; i < l->n; i++) {
        const ds_file *f = &l->f[i]; c->pr.file_no = i + 1; snprintf(c->pr.file, sizeof c->pr.file, "%s", f->path);
        if (!(i & 4095)) ds_note(c);   // (a pack of 200,000 files takes a while to look through)
        if (!strcmp(f->path, DS_LIST)) { c->pr.done += f->size; continue; }
        ds_path(p, sizeof p, c, ds_local_name(f->path));
        int64_t sz = ds_size(p);
        if (!strcmp(f->path, "settings.txt") && sz >= 0) { c->pr.done += f->size; continue; }   // the player's settings stay
        if (!fetch && sz < 0 && !strcmp(f->path, "env.txt") && ds_file_ok(c, f)) { c->pr.done += f->size; continue; }   // porter install's env.txt is itself (ds_file_ok); with a
        // server it is downloaded once, for its env.pack.txt, as before
        const ds_file *k = known ? ds_find(known, f->path) : NULL; char hex[65];
        if (sz == (int64_t)f->size && k && k->size == f->size && !strcmp(k->sha, f->sha)) { c->pr.done += f->size; continue; }
        if (sz == (int64_t)f->size) { uint64_t d0 = c->pr.done; if (!ds_hash_file(p, hex, c, &c->pr.done) && !strcmp(hex, f->sha)) continue; c->pr.done = d0 + f->size; }
        else c->pr.done += f->size;
        need[i] = 1; nneed++;
        if (f->bundle >= 0) { bs[f->bundle] = 1; nmem++; unpack += f->size; continue; }
        nown++; want += f->size;
        snprintf(pp, sizeof pp, "%s.part", p); int64_t ps = ds_size(pp); if (ps > 0 && (uint64_t)ps <= f->size) parts += (uint64_t)ps;
    }
    for (int k = 0; k < l->nb; k++) if (bs[k]) {   // the bundles of the files needed: in the folder at their size, else to download (a push cut short goes on as a part)
        const ds_bundle *b = &l->b[k]; ds_path(p, sizeof p, c, b->path); snprintf(pp, sizeof pp, "%s.part", p);
        int64_t sz = ds_size(p);
        if (b->size > maxb) maxb = b->size;
        if (sz == (int64_t)b->size) { nbloc++; continue; }
        if (sz >= 0 && (uint64_t)sz < b->size && ds_size(pp) < 0) ds_rename(p, pp); else if (sz >= 0) remove(p);
        bs[k] = 2; nbdl++; bwant += b->size;
        int64_t ps = ds_size(pp); if (ps > 0 && (uint64_t)ps <= b->size) parts += (uint64_t)ps;
    }
    if (!l->nb) DS_LOG(c, "data: %d of the server's %d files to download, %.1f MB (%.1f MB of them already in part files)", nneed, l->n, want / 1048576.0, parts / 1048576.0);
    else DS_LOG(c, "data: %d of the list's %d files to get: %d on their own (%.1f MB), %d in %d bundles (%.1f MB; %d bundles in the folder, %d to download, %.1f MB; %.1f MB in part files)",
                nneed, l->n, nown, want / 1048576.0, nmem, nbloc + nbdl, unpack / 1048576.0, nbloc, nbdl, bwant / 1048576.0, parts / 1048576.0);
    if (!nneed) goto out;
    if (c->free_bytes) {   // what comes down, and one bundle's files beside the bundles (each bundle is deleted once its files are out)
        // what comes down; the bundles being taken out at once (DS_UNPACKERS of them, or fewer when there are fewer), each beside its files before it is deleted (the largest is
        // counted for each); f2fs's node and block rounding for every file written (DS_FILE_SLACK); a margin. porter/install.py install() counts the same (its UNPACKERS, SLACK, MARGIN)
        uint64_t par = (uint64_t)(DS_UNPACKERS ? DS_UNPACKERS : 1); if (par > (uint64_t)(nbloc + nbdl)) par = (uint64_t)(nbloc + nbdl);
        uint64_t fr = c->free_bytes(c->bu, c->root), need_b = (fetch ? want + bwant - parts : 0) + par * maxb + (uint64_t)nneed * DS_FILE_SLACK + (64u << 20);
        if (fr < need_b) {
            c->space_need = need_b - fr; c->space_free = fr;
            snprintf(c->err, sizeof c->err, "%.1f GB more space is needed; the device has %.1f GB free", (need_b - fr) / 1073741824.0, fr / 1073741824.0); rc = DS_ERR_SPACE; goto out;
        }
    }
    if (local_id && ((fetch && (nown || nbdl)) || strcmp(local_id, l->id))) {   // the folder is no longer one whole pack until the new list is written: an app without the
        // server must not start a mix of two packs (only the folder's own bundles of the folder's own list taken out: still that pack)
        char a[1100], b[1100]; ds_path(a, sizeof a, c, DS_LIST); ds_path(b, sizeof b, c, DS_PREV); ds_rename(a, b);
        ds_path(a, sizeof a, c, DS_VAL); remove(a);   // (the validators of the list that went)
    }
    ds_progress dl = c->pr;
    if (fetch && (nown || nbdl)) {
        c->pr.phase = DS_PH_DOWNLOAD; c->pr.total = want + bwant; c->pr.done = 0; c->pr.files = nown + nbdl; c->pr.file_no = 0; ds_note(c);
        for (int k = 0; k < l->nb; k++) if (bs[k] == 2) {   // the bundles first: their files are taken out before env.txt and the manifest come
            const ds_bundle *b = &l->b[k]; ds_file bf = {b->path, b->size, "", -1}; memcpy(bf.sha, b->sha, sizeof bf.sha);
            c->pr.file_no++; snprintf(c->pr.file, sizeof c->pr.file, "%s", b->path); ds_note(c);
            ds_path(p, sizeof p, c, b->path);
            if ((rc = ds_get(c, &bf, p))) { DS_LOG(c, "data: %s", c->err); goto out; }
            bs[k] = 3;
        }
    }
    // the pack's own files first; env.txt and the manifest last of all, after the bundles' files
    for (int pass = 0; pass < 2; pass++) {
        if (pass && (nbloc || nbdl)) {
            dl = c->pr; c->pr.phase = DS_PH_UNPACK; c->pr.total = unpack; c->pr.done = 0; c->pr.files = nmem; c->pr.file_no = 0; ds_note(c);
            for (int k = 0; k < l->nb; k++) if (bs[k] == 2) lack = 1;   // (no server: it could not be downloaded)
            ds_upk u; memset(&u, 0, sizeof u); u.c = c; u.l = l; u.need = need; u.bs = bs;
            ds_unpack_all(&u); *got += u.got;   // the bundles in the folder, and those just downloaded
            if (u.rc) { snprintf(c->err, sizeof c->err, "%s", u.err); rc = u.rc; goto out; }
            uint8_t *buf = NULL;
            for (int k = 0; k < l->nb; k++) if (bs[k] == 4) {   // one the folder had with other bytes, deleted: downloaded here (the server's connection is this thread's)
                if (!fetch) { lack = 1; bs[k] = 2; continue; }   // (counted as one to download)
                const ds_bundle *b = &l->b[k]; ds_path(p, sizeof p, c, b->path); snprintf(c->pr.file, sizeof c->pr.file, "%s", b->path); ds_note(c);
                ds_file bf = {b->path, b->size, "", -1}; memcpy(bf.sha, b->sha, sizeof bf.sha);
                uint64_t d0 = c->pr.done; rc = ds_get(c, &bf, p); c->pr.done = d0;
                if (!rc && !buf && !(buf = malloc(1 << 20))) { snprintf(c->err, sizeof c->err, "out of memory"); rc = DS_ERR_DISK; }
                if (rc) { free(buf); DS_LOG(c, "data: %s", c->err); goto out; }
                char err[256]; ds_uout o = {ds_upk_add, &u, err, sizeof err}; int n = 0;   // (u: no threads now, its lock unused)
                rc = ds_untar(c, l, k, need, p, &n, buf, &o); *got += n;
                if ((!rc || rc == DS_ERR_DATA) && remove(p)) DS_LOG(c, "[RED] data: cannot delete %s (errno %d)", b->path, errno);
                if (rc) { free(buf); snprintf(c->err, sizeof c->err, "%s", err); DS_LOG(c, "data: %s", c->err); goto out; }
            }
            free(buf);
            DS_LOG(c, "data: %d files taken out of %d bundles", *got, nbloc + nbdl);
            if (fetch && nown) { c->pr = dl; c->pr.phase = DS_PH_DOWNLOAD; ds_note(c); }
        }
        if (!fetch) { lack |= nown > 0; continue; }
        for (int i = 0; i < l->n; i++) {
            const ds_file *f = &l->f[i]; int late = !strcmp(f->path, "env.txt") || !strcmp(f->path, DS_MANIFEST);
            if (!need[i] || f->bundle >= 0 || late != pass) continue;
            c->pr.file_no++; snprintf(c->pr.file, sizeof c->pr.file, "%s", f->path); ds_note(c);
            ds_path(p, sizeof p, c, !strcmp(f->path, "env.txt") ? DS_ENV_PACK ".new" : f->path);
            if ((rc = ds_get(c, f, p))) { DS_LOG(c, "data: %s", c->err); goto out; }
            if (!strcmp(f->path, "env.txt")) {   // env.txt: the pack's, with the player's rows
                char e[1100], q[1100]; ds_path(e, sizeof e, c, "env.txt"); ds_path(q, sizeof q, c, DS_ENV_PACK);
                char *cur = ds_read(e, NULL, 1 << 20), *old = ds_read(q, NULL, 1 << 20), *pk = ds_read(p, NULL, 1 << 20);
                char *mg = pk ? ds_env_merge(cur, old, pk) : NULL;
                int bad = !mg || ds_write(e, mg, strlen(mg)) || ds_rename(p, q);
                free(cur); free(old); free(pk); free(mg);
                if (bad) { snprintf(c->err, sizeof c->err, "cannot write env.txt (errno %d)", errno); rc = DS_ERR_DISK; goto out; }
            }
        }
    }
    if (lack) {
        int nb = 0, nf = 0; const char *first = "";
        for (int k = 0; k < l->nb; k++) if (bs[k] == 2) { nb++; if (!first[0]) first = l->b[k].path; }
        for (int i = 0; i < l->n; i++) if (need[i] && l->f[i].bundle < 0) { nf++; if (!first[0]) first = l->f[i].path; }
        snprintf(c->err, sizeof c->err, "%d bundles and %d other files of the pack are not in the files folder (%.120s), and there is no server: install the pack again", nb, nf, first);
        rc = DS_ERR_LOCAL; goto out;
    }
    *got = nneed;
out:
    free(need); free(bs);
    return rc;
}

// The whole sync: DS_DONE (files downloaded), DS_UPTODATE (nothing to do), DS_OFFLINE (no server, the folder holds a whole pack: start the game), else an error with
// c->err (DS_ERR_NET: try again later; a part already downloaded is kept). c->fetch NULL: no server at all (an APK without --data-url whose files folder has bundles to
// unpack: front_android.c runs the sync then only): DS_OFFLINE once they are, DS_ERR_LOCAL when the folder still lacks files
static int ds_sync(ds_ctx *c) {
    memset(&c->pr, 0, sizeof c->pr); c->err[0] = 0; c->space_need = c->space_free = 0; c->pr.phase = DS_PH_CONNECT; ds_note(c);
    ds_list local, prev, srv; char *lraw = NULL; size_t lrn = 0; int looked = 0;
    int have_local = !ds_list_load(c, DS_LIST, &local, &lraw, &lrn), have_prev = !ds_list_load(c, DS_PREV, &prev, NULL, NULL);
    int pending = have_local && ds_pending(c, &local);   // bundles in the folder (porter install's, or an unpack that stopped): their files are taken out before the game starts
    // A bundles folder with no list of bundles beside it (none, or one of format 1) is an install that has not finished (porter install pushes the bundles first, files.json
    // and the manifest after): its pack is not known to be whole, and the folder is neither taken for whole nor deleted (that would delete the bundles the install pushed). Only
    // a server's list (below), which the files are then made right by, decides what the folder holds
    int stray = !pending && ds_bundles_dir_any(c->root);
    ds_val cond, got; memset(&cond, 0, sizeof cond); memset(&got, 0, sizeof got);
    int ask = have_local && ds_val_load(c, local.id, &cond);   // the list only if it is no longer the folder's
    ds_mem m = {NULL, 0, 0, 256u << 20}; uint64_t st = 0; int64_t tot = -1; int http = 0;
    int rc = c->fetch ? c->fetch(c->fu, DS_LIST, 0, &st, &tot, &http, ask ? &cond : NULL, &got, ds_mem_sink, &m) : DS_ERR_NET;
    if (rc || st) {
        free(m.b); m.b = NULL; free(lraw);
        // the folder's list with no update begun after it (files.json.prev) is a whole pack by construction: a sample (ds_sample), and every file only when the sample
        // misses one (then not offline) or with files.json.prev there. An offline start read every file's size: 10-15 s for 33,696 files on a Retroid Pocket 6's /sdcard
        int whole = 0, why = 0;
        if (stray) {
            snprintf(c->err, sizeof c->err, "the files folder has bundles, but no list of them beside it: the install did not finish, install the pack again");
            DS_LOG(c, "data: %s", c->err); why = DS_ERR_LOCAL;
        }
        else if (have_local && !strcmp(local.package, c->package)) {
            if (pending) {   // its bundles taken out, from the folder alone; whole once every file of the list is there
                int n = 0; why = ds_apply(c, &local, &local, NULL, 0, &n); whole = !why;
                if (whole) DS_LOG(c, "data: no server; %d files taken out of the bundles in the folder", n);
            }
            else if (!have_prev && ds_sample(c, &local, &looked)) { whole = 1; DS_LOG(c, "data: no server; the folder's pack, %d of its %d files looked at", looked, local.n); }
            else whole = ds_complete(c, &local);
        }
        if (!whole && !stray && !have_local && !have_prev) {   // a pack installed over adb (porter install): its manifest goes last, so with it the pack is whole (a pack with bundles: only
            // by its files.json, which porter install pushes before the manifest, once the bundles are unpacked)
            char p[1100]; ds_path(p, sizeof p, c, DS_MANIFEST); size_t n; char *t = ds_read(p, &n, 1 << 20);
            char want[300]; snprintf(want, sizeof want, "\"package\": \"%s\"", c->package); whole = t && strstr(t, want) && !strstr(t, "\"bundles\":"); free(t);
        }
        if (whole) ds_clean_bundles(c);   // (what an older pack left there: else an APK without a server would run this at every launch)
        if (!stray && (!why || why == DS_ERR_LOCAL)) {
            if (c->fetch) snprintf(c->err, sizeof c->err, rc == DS_ERR_SERVER ? "the server answered HTTP %d for " DS_LIST : "the server could not be reached", http);
            else if (!why) snprintf(c->err, sizeof c->err, "the files folder does not hold the whole pack, and there is no server: install the pack again");
        }
        if (whole) c->err[0] = 0;   // (the game starts: nothing to say, not even that the server could not be reached)
        if (have_local) ds_free(&local);
        if (have_prev) ds_free(&prev);
        return whole ? DS_OFFLINE : stray ? DS_ERR_LOCAL : why && why != DS_ERR_LOCAL ? why : !c->fetch ? DS_ERR_LOCAL : rc ? rc : DS_ERR_NET;
    }
    int same = ask && http == 304;   // the server's list is still the folder's: not downloaded
    if (same) {
        int mine = !strcmp(local.package, c->package);
        if (mine && !pending && !stray && ds_sample(c, &local, &looked)) {
            DS_LOG(c, "data: the server's " DS_LIST " has not changed (HTTP 304); %d of the pack's %d files looked at", looked, local.n); rc = DS_UPTODATE; goto out0; }
        if (mine) DS_LOG(c, pending ? "data: the server's " DS_LIST " has not changed (HTTP 304); the folder has bundles to unpack" :
                         "data: the server's " DS_LIST " has not changed (HTTP 304), but a file of the pack is missing: every file is checked");
        free(m.b); m.b = lraw; m.n = lrn; lraw = NULL;   // the folder's list stands for the server's (the same bytes it sent then), the rest as for a list just downloaded
    }
    free(lraw); lraw = NULL;   // (only a 304 needs the folder's bytes: not kept through a download)
    if (ds_parse(m.b ? m.b : "", m.n, &srv)) { snprintf(c->err, sizeof c->err, "the server's " DS_LIST " is not a data pack's list"); rc = DS_ERR_SERVER; goto out0; }   // (m.b is freed at out0)
    if (srv.format > DS_FORMAT) { snprintf(c->err, sizeof c->err, "the server's " DS_LIST " is format %d; this app reads up to format %d: install the APK ported with that pack", srv.format, DS_FORMAT); rc = DS_ERR_SERVER; goto out1; }
    if (strcmp(srv.package, c->package)) { snprintf(c->err, sizeof c->err, "the server's data is for %.100s, not this app (%.100s)", srv.package, c->package); rc = DS_ERR_PACKAGE; goto out1; }
    if (!same && have_local && !pending && !stray && !strcmp(local.id, srv.id) && ds_sample(c, &srv, &looked)) {   // the server's pack is the folder's
        DS_LOG(c, "data: the server's " DS_LIST " lists the folder's pack; %d of its %d files looked at", looked, srv.n);
        ds_val_save(c, srv.id, &got, ask ? &cond : NULL); rc = DS_UPTODATE; goto out1;   // its validators: the next sync asks for the list only if it changed
    }
    // which files to download: not there at the list's size, or there at that size with another hash (the earlier list's, else the file's own)
    const ds_list *known = have_local ? &local : have_prev ? &prev : NULL;
    int nneed = 0; char p[1100];
    if ((rc = ds_apply(c, &srv, known, have_local ? local.id : NULL, 1, &nneed))) goto out1;
    c->pr.phase = DS_PH_FINISH; ds_note(c);
    ds_path(p, sizeof p, c, DS_LIST);
    if (ds_write(p, m.b, m.n)) { snprintf(c->err, sizeof c->err, "cannot write " DS_LIST " (errno %d)", errno); rc = DS_ERR_DISK; goto out1; }
    ds_path(p, sizeof p, c, DS_PREV); remove(p);
    ds_val_save(c, srv.id, same ? &cond : &got, NULL);   // after the list: validators beside a list they are not of are never sent (their id is checked)
    ds_clean_bundles(c);
    rc = nneed ? DS_DONE : DS_UPTODATE;
out1: ds_free(&srv);
out0: free(m.b); free(lraw);
    if (have_local) ds_free(&local);
    if (have_prev) ds_free(&prev);
    return rc;
}
#endif
