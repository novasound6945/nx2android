// nxnative/dm/android/datasync.h on the computer (tests/test_datasync.py builds and drives it): the "server" is a folder, read by a fetch that can break off after some
// bytes; '-' is a server that cannot be reached, 'none' no server at all (ds_ctx.fetch NULL: an APK ported without --data-url). -DDS_H="<path>" builds it with another
// datasync.h (an app from before bundles).
// datasync_fixture <server dir|-|none> <root> <package> [--break BYTES] [--free BYTES]
// Prints "fetch <path> <from>" for each file the server sent, then: result <R> fetched_files <n> fetched_bytes <b> space_need <B> space_free <B> [err <c->err>]
// The pointer file and the APK's asset (a datasync.h with them, DS_PTR_MAX):
// datasync_fixture --hmac <key hex> <message hex>   prints the HMAC-SHA256 in hex
// datasync_fixture --pointer <key hex> <file>       prints "ok <root>" and "cred <cred>" on two lines (ds_pointer_open), or "bad <why>"
// datasync_fixture --source <file>                  prints "v<N>", then url, cred, pointer, key (hex), sub on a line each (ds_source), or "bad"
#include <locale.h>
#ifdef DS_H
#include DS_H
#else
#include "datasync.h"
#endif
#ifdef _WIN32
#define fx_seek _fseeki64
#define fx_tell _ftelli64
#else
#define fx_seek fseeko
#define fx_tell ftello
#endif

typedef struct { const char *dir; int64_t brk; uint64_t sent; int files; } srv;
static int fetch(void *u, const char *rel, uint64_t from, uint64_t *start, int64_t *total, int *http, const ds_val *cond, ds_val *got, ds_sink sink, void *su) {
    srv *s = u; (void)cond; if (got) memset(got, 0, sizeof *got);
    if (!s->dir) return DS_ERR_NET;
    char p[2048]; snprintf(p, sizeof p, "%s/%s", s->dir, rel);
    FILE *f = fopen(p, "rb"); if (!f) { *http = 404; return DS_ERR_SERVER; }
    s->files++; printf("fetch %s %llu\n", rel, (unsigned long long)from);
    fx_seek(f, 0, SEEK_END); *total = fx_tell(f);
    if ((int64_t)from > *total) { fclose(f); *http = 416; return DS_ERR_SERVER; }
    fx_seek(f, (int64_t)from, SEEK_SET); *start = from; *http = from ? 206 : 200;
    char b[7919]; size_t k;   // an odd size: writes that do not line up with anything
    while ((k = fread(b, 1, sizeof b, f)) > 0) {
        if (s->brk >= 0 && (int64_t)(s->sent + k) > s->brk) { size_t m = (size_t)(s->brk - (int64_t)s->sent); if (m) sink(su, b, m); s->sent += m; fclose(f); return DS_ERR_NET; }
        if (sink(su, b, k)) { fclose(f); return DS_ERR_NET; }
        s->sent += k;
    }
    fclose(f); return 0;
}
static uint64_t freeb(void *u, const char *root) { (void)root; return *(uint64_t *)u; }
static void logf_(void *u, const char *m) { (void)u; printf("log: %s\n", m); }
static const char *NAMES[] = {"DONE", "UPTODATE", "OFFLINE", "ERR_NET", "ERR_SERVER", "ERR_PACKAGE", "ERR_SPACE", "ERR_DATA", "ERR_DISK", "ERR_LOCAL"};

#ifdef DS_PTR_MAX
static size_t unhex(const char *s, uint8_t *out, size_t cap) {   // (the tests give well-formed hex)
    size_t n = strlen(s) / 2; if (n > cap) n = cap;
    for (size_t i = 0; i < n; i++) { unsigned v = 0; if (sscanf(s + 2 * i, "%2x", &v) != 1) v = 0; out[i] = (uint8_t)v; }
    return n;
}
static int extra(int argc, char **argv) {   // the pointer's and the asset's modes; -1: not one of them
    if (argc == 4 && !strcmp(argv[1], "--hmac")) {
        static uint8_t k[4096], m[8192]; uint8_t t[32]; size_t kn = unhex(argv[2], k, sizeof k), mn = unhex(argv[3], m, sizeof m);
        ds_hmac h; ds_hmac_init(&h, k, kn); ds_hmac_update(&h, m, mn); ds_hmac_final(&h, t);
        for (int i = 0; i < 32; i++) printf("%02x", t[i]);
        printf("\n"); return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "--pointer")) {
        uint8_t k[32] = {0}; unhex(argv[2], k, sizeof k); size_t n = 0; char *t = ds_read(argv[3], &n, 1 << 20);
        char root[1024], cred[512]; const char *why = "";
        if (!t) { printf("bad cannot read\n"); return 0; }
        if (ds_pointer_open(t, n, k, root, sizeof root, cred, sizeof cred, &why)) printf("bad %s\n", why);
        else printf("ok %s\ncred %s\n", root, cred);
        free(t); return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--source")) {
        size_t n = 0; char *t = ds_read(argv[2], &n, 1 << 20); static ds_src s;
        if (!t || ds_source((const uint8_t *)t, n, &s)) { printf("bad\n"); free(t); return 0; }
        printf("v%d\n%s\n%s\n%s\n", s.version, s.url, s.cred, s.pointer);
        for (int i = 0; i < 32; i++) printf("%02x", s.key[i]);
        printf("\n%s\n", s.sub); free(t); return 0;
    }
    return -1;
}
#endif

int main(int argc, char **argv) {
    setlocale(LC_ALL, ".UTF8");   // Windows' UCRT: narrow file names are UTF-8, as on Android (elsewhere the call does nothing)
#ifdef DS_PTR_MAX
    { int r = extra(argc, argv); if (r >= 0) return r; }
#endif
    if (argc < 4) { fprintf(stderr, "usage: datasync_fixture <server|-|none> <root> <package> [--break N] [--free N]\n"); return 2; }
    int none = !strcmp(argv[1], "none");
    srv s = {strcmp(argv[1], "-") && !none ? argv[1] : NULL, -1, 0, 0}; uint64_t fr = 0; int use_free = 0, verify = 0;
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--verify")) verify = 1;
        else if (!strcmp(argv[i], "--break") && i + 1 < argc) s.brk = atoll(argv[++i]);
        else if (!strcmp(argv[i], "--free") && i + 1 < argc) { fr = (uint64_t)atoll(argv[++i]); use_free = 1; }
    }
    ds_ctx c = {.root = argv[2], .package = argv[3], .fetch = none ? NULL : fetch, .fu = &s, .free_bytes = use_free ? freeb : NULL, .bu = &fr, .log = logf_};
#ifdef DS_VERIFY
    c.verify = verify;
#else
    (void)verify;
#endif
    int rc = ds_sync(&c);
    unsigned long long need = 0, fr0 = 0;
#ifdef DS_FORMAT   // (a datasync.h from before these: 0)
    need = c.space_need; fr0 = c.space_free;
#endif
    printf("result %s fetched_files %d fetched_bytes %llu space_need %llu space_free %llu%s%s\n", rc >= 0 && rc < (int)(sizeof NAMES / sizeof *NAMES) ? NAMES[rc] : "?",
           s.files, (unsigned long long)s.sent, need, fr0, c.err[0] ? " err " : "", c.err);
    return 0;
}
