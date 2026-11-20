// The save's cloud sync, its parts that need no Android (savecloud_android.c is the transport, the threads and the screens; nxnative/dm/host/tests/t_savesync.c tests these
// on the computer): the save as one deterministic tar, that tar read back with every path checked, sync.txt and the sync state, the server's meta, and the start's decision.
//
// The body the server keeps (PUT/GET /api/saves/g/<package>/data) is opaque to it: a ustar archive the app writes so the same save is always the same bytes, and its
// SHA-256 is therefore the save's identity (the state's sha256, the server's meta.sha256, X-Save-Sha256):
//   save/            the committed save tree ($DM_SAVE: never <DM_SAVE>.work, a transaction in flight is not a save)
//   save/<path>      its folders (typeflag 5, mode 0755) and files (typeflag 0, mode 0644)
//   device/...       the device save (<DM_SAVE>_device), only when it has entries (as saves_android.c dm_saves_export copies it)
// Every entry's mtime, uid and gid are 0, uname and gname empty, and the entries are sorted by their names (strcmp), so neither the file system's order nor the times a
// commit wrote change the bytes. A name longer than ustar's 100 bytes goes in its prefix field (split at a /); one that does not fit both is refused (the save is not sent).
// Reading one back (ss_untar) checks the whole archive before writing a file: only folders and regular files, names under save/ or device/ with no empty, . or .. part
// (no absolute path, no backslash), each header's checksum, sizes inside the body, at most SS_MAX_BODY bytes and SS_MAX_ENTRIES entries in all. A link of any kind, a device
// node or a FIFO refuses the archive: nothing is written.
#pragma once
#include "datasync.h"   // ds_sha (SHA-256), the small JSON reader (ds_str, ds_skip, ds_u64), ds_rel_ok, ds_mkdirs
#include <fcntl.h>
#include <time.h>

#define SS_MAX_BODY (64u << 20)    // the server's limit for a body, and the app's for a tar it makes or reads
#define SS_MAX_ENTRIES 50000

// ---- a growing buffer (malloc) ----
typedef struct { uint8_t *p; size_t n, cap; int over; } ss_buf;   // over: something did not fit SS_MAX_BODY (or memory ran out)
static void ss_buf_free(ss_buf *b) { free(b->p); memset(b, 0, sizeof *b); }
static int ss_buf_put(ss_buf *b, const void *d, size_t n) {
    if (b->over) return -1;
    if (b->n + n > SS_MAX_BODY) { b->over = 1; return -1; }
    if (b->n + n > b->cap) {
        size_t c = b->cap ? b->cap : 64 << 10; while (c < b->n + n) c *= 2;
        uint8_t *q = realloc(b->p, c); if (!q) { b->over = 1; return -1; }
        b->p = q; b->cap = c;
    }
    memcpy(b->p + b->n, d, n); b->n += n; return 0;
}
static void ss_sha256(const void *p, size_t n, char out[65]) { ds_sha s; ds_sha_init(&s); ds_sha_update(&s, p, n); ds_sha_hex(&s, out); }

// ---- the tar ----
typedef struct { char *name; char *src; int dir; uint64_t size; } ss_ent;   // name: as in the archive (a folder's ends in /); src: the file to read
typedef struct { ss_ent *e; int n, cap; int64_t newest; int err; } ss_list;
static int ss_list_add(ss_list *l, const char *name, const char *src, int dir, uint64_t size) {
    if (l->n >= SS_MAX_ENTRIES) { l->err = E2BIG; return -1; }
    if (l->n == l->cap) { int c = l->cap ? l->cap * 2 : 256; ss_ent *q = realloc(l->e, (size_t)c * sizeof *q); if (!q) { l->err = ENOMEM; return -1; } l->e = q; l->cap = c; }
    ss_ent *x = &l->e[l->n]; x->name = strdup(name); x->src = src ? strdup(src) : NULL; x->dir = dir; x->size = size;
    if (!x->name || (src && !x->src)) { free(x->name); free(x->src); l->err = ENOMEM; return -1; }
    l->n++; return 0;
}
static void ss_list_free(ss_list *l) { for (int i = 0; i < l->n; i++) { free(l->e[i].name); free(l->e[i].src); } free(l->e); memset(l, 0, sizeof *l); }
static int ss_has_entries(const char *p) {
    DIR *d = opendir(p); struct dirent *e; int n = 0; if (!d) return 0;
    while (!n && (e = readdir(d))) n = strcmp(e->d_name, ".") && strcmp(e->d_name, "..");
    closedir(d); return n;
}
// dir's tree under the archive name pre (ending in /); symbolic links and anything that is neither a folder nor a regular file are left out, as saves_android.c copy_tree does
static int ss_walk(ss_list *l, const char *dir, const char *pre) {
    DIR *d = opendir(dir); struct dirent *e; if (!d) { l->err = errno; return -1; }
    int r = 0;
    while (!r && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char src[2048], name[1024]; struct stat st;
        if ((size_t)snprintf(src, sizeof src, "%s/%s", dir, e->d_name) >= sizeof src || (size_t)snprintf(name, sizeof name, "%s%s", pre, e->d_name) >= sizeof name - 1) { l->err = ENAMETOOLONG; r = -1; break; }
        if (lstat(src, &st)) { l->err = errno; r = -1; break; }
        if ((int64_t)st.st_mtime > l->newest) l->newest = (int64_t)st.st_mtime;
        if (S_ISDIR(st.st_mode)) { strcat(name, "/"); r = ss_list_add(l, name, NULL, 1, 0); if (!r) r = ss_walk(l, src, name); }
        else if (S_ISREG(st.st_mode)) r = ss_list_add(l, name, src, 0, (uint64_t)st.st_size);
    }
    closedir(d); return r;
}
static int ss_ent_cmp(const void *a, const void *b) { return strcmp(((const ss_ent *)a)->name, ((const ss_ent *)b)->name); }
static void ss_octal(char *f, size_t w, uint64_t v) { f[w - 1] = 0; for (size_t i = w - 1; i-- > 0;) { f[i] = (char)('0' + (v & 7)); v >>= 3; } }   // w - 1 digits and a NUL
static int ss_header(uint8_t h[512], const char *name, int dir, uint64_t size) {   // 0, or -1 for a name ustar cannot hold
    memset(h, 0, 512); size_t n = strlen(name);
    if (n <= 100) memcpy(h, name, n);
    else {   // prefix (up to 155) / name (up to 100), split at a slash
        size_t cut = 0; for (size_t i = 0; i < n && i <= 155; i++) if (name[i] == '/' && n - i - 1 <= 100 && n - i - 1 > 0) { cut = i; break; }
        if (!cut) return -1;
        memcpy(h + 345, name, cut); memcpy(h, name + cut + 1, n - cut - 1);
    }
    ss_octal((char *)h + 100, 8, dir ? 0755 : 0644); ss_octal((char *)h + 108, 8, 0); ss_octal((char *)h + 116, 8, 0);
    ss_octal((char *)h + 124, 12, dir ? 0 : size); ss_octal((char *)h + 136, 12, 0);
    h[156] = dir ? '5' : '0'; memcpy(h + 257, "ustar", 6); h[263] = '0'; h[264] = '0';
    ss_octal((char *)h + 329, 8, 0); ss_octal((char *)h + 337, 8, 0);
    memset(h + 148, ' ', 8); unsigned sum = 0; for (int i = 0; i < 512; i++) sum += h[i];
    ss_octal((char *)h + 148, 7, sum); h[155] = ' ';   // six digits, a NUL, a space: as GNU tar and Python's tarfile write it
    return 0;
}
static int ss_read_into(ss_buf *b, const char *path, uint64_t size) {   // the file's bytes; -EAGAIN when it is not the size it had (a commit replaced it while it was read)
    int fd = open(path, O_RDONLY | O_CLOEXEC); if (fd < 0) return errno == ENOENT ? -EAGAIN : -errno;
    uint8_t t[65536]; uint64_t got = 0; ssize_t k; int r = 0;
    while ((k = read(fd, t, sizeof t)) > 0) { got += (uint64_t)k; if (got > size || ss_buf_put(b, t, (size_t)k)) { r = got > size ? -EAGAIN : -E2BIG; break; } }
    if (!r && k < 0) r = -errno;
    close(fd);
    if (!r && got != size) r = -EAGAIN;
    return r;
}
// The save (and the device save) as the archive: 0 with out (and *newest, the newest mtime in the trees, 0 when there is none), 1 when neither has anything (no local save:
// out stays empty), else -errno with why in err (-E2BIG: more than SS_MAX_BODY; -EAGAIN: a file changed while it was read, so the caller reads the tree again)
static int ss_tar_tree(const char *save, const char *dev, ss_buf *out, int64_t *newest, char *err, size_t ecap) {
    memset(out, 0, sizeof *out); if (newest) *newest = 0; err[0] = 0;
    int has_save = save && save[0] && ss_has_entries(save), has_dev = dev && dev[0] && ss_has_entries(dev);
    if (!has_save && !has_dev) return 1;
    ss_list l = {0}; int r = 0;
    if (ss_list_add(&l, "save/", NULL, 1, 0)) r = -1;
    if (!r && has_save && ss_walk(&l, save, "save/")) r = -1;
    if (!r && has_dev && (ss_list_add(&l, "device/", NULL, 1, 0) || ss_walk(&l, dev, "device/"))) r = -1;
    if (r) { r = l.err == ENOENT ? -EAGAIN : -(l.err ? l.err : EIO); snprintf(err, ecap, "reading the save's tree failed (%s)", strerror(-r)); ss_list_free(&l); return r; }
    qsort(l.e, (size_t)l.n, sizeof *l.e, ss_ent_cmp);
    uint8_t h[512], z[512] = {0};
    for (int i = 0; i < l.n && !r; i++) {
        const ss_ent *x = &l.e[i];
        if (ss_header(h, x->name, x->dir, x->size)) { snprintf(err, ecap, "a name too long for the archive: %.200s", x->name); r = -ENAMETOOLONG; break; }
        if (ss_buf_put(out, h, 512)) { r = -E2BIG; break; }
        if (x->dir) continue;
        if ((r = ss_read_into(out, x->src, x->size))) { if (r != -E2BIG) snprintf(err, ecap, "reading %.200s failed (%s)", x->name, strerror(-r)); break; }
        size_t pad = (size_t)((512 - x->size % 512) % 512); if (pad && ss_buf_put(out, z, pad)) { r = -E2BIG; break; }
    }
    if (!r && (ss_buf_put(out, z, 512) || ss_buf_put(out, z, 512))) r = -E2BIG;
    if (r == -E2BIG || (r && out->over)) { r = -E2BIG; snprintf(err, ecap, "the save is bigger than %u MiB", SS_MAX_BODY >> 20); }
    if (newest) *newest = l.newest;
    ss_list_free(&l);
    if (r) ss_buf_free(out);
    return r;
}
static void ss_rm_tree(const char *p) {
    struct stat st; if (lstat(p, &st)) return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(p); struct dirent *e;
        while (d && (e = readdir(d))) { if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue; char c[2048]; snprintf(c, sizeof c, "%s/%s", p, e->d_name); ss_rm_tree(c); }
        if (d) closedir(d);
        rmdir(p);
    } else unlink(p);
}
static int ss_oct_read(const uint8_t *f, size_t w, uint64_t *v) {   // an octal field (spaces and NULs around it); -1 for anything else
    size_t i = 0; uint64_t x = 0; int digits = 0;
    while (i < w && f[i] == ' ') i++;
    for (; i < w && f[i] >= '0' && f[i] <= '7'; i++, digits++) { if (x >> 60) return -1; x = x << 3 | (uint64_t)(f[i] - '0'); }
    for (; i < w; i++) if (f[i] != ' ' && f[i] != 0) return -1;
    if (!digits) return -1;
    *v = x; return 0;
}
// One header: its kind (0 a file, 1 a folder, 2 the end), the name (prefix/name joined) and the size; -1 with why for anything the app does not take
static int ss_tar_entry(const uint8_t *h, char *name, size_t cap, uint64_t *size, const char **why) {
    int zero = 1; for (int i = 0; i < 512; i++) if (h[i]) { zero = 0; break; }
    if (zero) return 2;
    uint64_t sum = 0; unsigned calc = 0; for (int i = 0; i < 512; i++) calc += i >= 148 && i < 156 ? ' ' : h[i];
    if (ss_oct_read(h + 148, 8, &sum) || sum != calc) { *why = "a header's checksum is wrong"; return -1; }
    char t = (char)h[156];
    if (t != '0' && t != 0 && t != '5') { *why = "an entry that is not a file or a folder (a link, a device, a FIFO, an extension)"; return -1; }
    if (ss_oct_read(h + 124, 12, size)) { *why = "a size that is not a number"; return -1; }
    char nm[101], pf[156]; memcpy(nm, h, 100); nm[100] = 0; memcpy(pf, h + 345, 155); pf[155] = 0;
    if (memcmp(h + 257, "ustar", 5)) pf[0] = 0;   // (a pre-POSIX header has no prefix field)
    if ((size_t)snprintf(name, cap, "%s%s%s", pf, pf[0] ? "/" : "", nm) >= cap) { *why = "a name too long"; return -1; }
    size_t n = strlen(name); int dir = t == '5';
    if (dir) { if (*size) { *why = "a folder with a size"; return -1; } }
    else if (n && name[n - 1] == '/') { *why = "a file whose name ends in /"; return -1; }
    while (n && name[n - 1] == '/') name[--n] = 0;   // (the folder's own slash)
    const char *rel = !strncmp(name, "save", 4) && (!name[4] || name[4] == '/') ? name + 4 : !strncmp(name, "device", 6) && (!name[6] || name[6] == '/') ? name + 6 : NULL;
    if (!rel) { *why = "a name outside save/ and device/"; return -1; }
    if (!*rel && !dir) { *why = "save or device as a file"; return -1; }
    if (*rel && !ds_rel_ok(rel + 1)) { *why = "a path with an empty, . or .. part (or a backslash)"; return -1; }
    return dir;
}
// The archive into the folders dst_save (save/) and dst_dev (device/: made only when the archive has the device save). Both must not exist. The whole archive is checked first;
// 0, or -1 with why in err (then nothing was written, or what was written is removed again)
static int ss_untar(const uint8_t *p, size_t n, const char *dst_save, const char *dst_dev, char *err, size_t ecap) {
    err[0] = 0; const char *why = NULL; char name[512]; uint64_t size, total = 0; int entries = 0, dev = 0, end = 0;
    if (n > SS_MAX_BODY) { snprintf(err, ecap, "the archive is bigger than %u MiB", SS_MAX_BODY >> 20); return -1; }
    for (size_t o = 0; o + 512 <= n;) {   // the check
        int k = ss_tar_entry(p + o, name, sizeof name, &size, &why);
        if (k < 0) { snprintf(err, ecap, "the archive is refused: %s", why); return -1; }
        if (k == 2) { end = 1; break; }
        if (++entries > SS_MAX_ENTRIES) { snprintf(err, ecap, "the archive has more than %d entries", SS_MAX_ENTRIES); return -1; }
        size_t blocks = (size_t)((size + 511) / 512);
        if (size > SS_MAX_BODY || o + 512 + blocks * 512 > n) { snprintf(err, ecap, "the archive is refused: an entry runs past its end"); return -1; }
        total += size; if (total > SS_MAX_BODY) { snprintf(err, ecap, "the archive's files are bigger than %u MiB", SS_MAX_BODY >> 20); return -1; }
        if (!strncmp(name, "device", 6)) dev = 1;
        o += 512 + blocks * 512;
    }
    if (!end) { snprintf(err, ecap, "the archive is refused: it has no end (cut short)"); return -1; }
    if (!entries) { snprintf(err, ecap, "the archive is empty"); return -1; }
    if (mkdir(dst_save, 0770)) { snprintf(err, ecap, "%s cannot be made (%s)", dst_save, strerror(errno)); return -1; }
    if (dev && mkdir(dst_dev, 0770)) { snprintf(err, ecap, "%s cannot be made (%s)", dst_dev, strerror(errno)); ss_rm_tree(dst_save); return -1; }
    int r = 0;
    for (size_t o = 0; o + 512 <= n && !r;) {   // the writing (each entry was checked above)
        int k = ss_tar_entry(p + o, name, sizeof name, &size, &why);
        if (k == 2) break;
        int isdev = !strncmp(name, "device", 6); const char *rel = name + (isdev ? 6 : 4); char path[2048];
        if ((size_t)snprintf(path, sizeof path, "%s%s", isdev ? dst_dev : dst_save, rel) >= sizeof path) { snprintf(err, ecap, "a path too long"); r = -1; break; }
        if (*rel) {
            if (ds_mkdirs(path)) { snprintf(err, ecap, "the folders of %s cannot be made (%s)", path, strerror(errno)); r = -1; break; }
            if (k == 1) { if (mkdir(path, 0770) && errno != EEXIST) { snprintf(err, ecap, "%s cannot be made (%s)", path, strerror(errno)); r = -1; } }
            else {
                int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0660);   // (EEXIST: two names that differ only by case, on a case-insensitive file system)
                if (fd < 0) { snprintf(err, ecap, "%s cannot be written (%s)", path, strerror(errno)); r = -1; break; }
                const uint8_t *d = p + o + 512; uint64_t left = size;
                while (left && !r) { ssize_t w = write(fd, d, left > (1u << 20) ? (1u << 20) : (size_t)left); if (w <= 0) { snprintf(err, ecap, "%s cannot be written (%s)", path, strerror(errno)); r = -1; } else { d += w; left -= (uint64_t)w; } }
                if (!r && fsync(fd)) { snprintf(err, ecap, "%s cannot be written (%s)", path, strerror(errno)); r = -1; }
                if (close(fd) && !r) { snprintf(err, ecap, "%s cannot be written (%s)", path, strerror(errno)); r = -1; }
            }
        }
        o += 512 + (size_t)((size + 511) / 512) * 512;
    }
    if (r) { ss_rm_tree(dst_save); if (dev) ss_rm_tree(dst_dev); }
    return r;
}

// ---- sync.txt (the files folder: adb pushes it): url=, token=, device= as KEY=VALUE lines (# comments). Without url or token the sync is off ----
typedef struct { char url[512], token[512], device[64]; } ss_cfg;
static void ss_trim(char *s) { char *e = s + strlen(s); while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0; char *b = s; while (*b == ' ' || *b == '\t') b++; if (b != s) memmove(s, b, strlen(b) + 1); }
static int ss_cfg_text(const char *txt, ss_cfg *c) {   // 1: on (an http(s):// address and a token), 0: off
    memset(c, 0, sizeof *c); char line[1024];
    for (const char *s = txt; *s;) {
        const char *e = strchr(s, '\n'); size_t k = e ? (size_t)(e - s) : strlen(s); if (k >= sizeof line) k = sizeof line - 1;
        memcpy(line, s, k); line[k] = 0; s += k + (e != NULL); if (!e) s += strlen(s);
        ss_trim(line); if (!line[0] || line[0] == '#') continue;
        char *eq = strchr(line, '='); if (!eq) continue; *eq = 0; char *v = eq + 1; ss_trim(line); ss_trim(v);
        if (!strcmp(line, "url")) snprintf(c->url, sizeof c->url, "%s", v);
        else if (!strcmp(line, "token")) snprintf(c->token, sizeof c->token, "%s", v);
        else if (!strcmp(line, "device")) snprintf(c->device, sizeof c->device, "%s", v);
    }
    size_t L = strlen(c->url); while (L && c->url[L - 1] == '/') c->url[--L] = 0;
    for (const char *q = c->token; *q; q++) if ((unsigned char)*q <= 0x20 || (unsigned char)*q >= 0x7f) c->token[0] = 0;   // a header value: printable ASCII
    if (!c->device[0]) snprintf(c->device, sizeof c->device, "android");
    int ok = (!strncmp(c->url, "https://", 8) || !strncmp(c->url, "http://", 7)) && ds_url_text(c->url) && c->token[0];
    if (!ok) { memset(c->token, 0, sizeof c->token); return 0; }
    return 1;
}

// ---- the sync state (saves/sync-state.txt, KEY=VALUE lines) ----
// seq, sha256: the server's seq the save was last the same as, and the archive's hash then ("" for no save); pending: an upload is owed; conflict: the server moved on
// while this device's save changed too (cseq, ctime, cdevice: the server's meta then); force, fbase: the player chose this device's save over the server's newer one (the
// next upload's base is fbase); auth: the server refused the token; last_up: when the save last went up (Unix seconds). have: the file was there (a first setup has none).
// offline_play (op_seq, op_sha: the last sync's seq and hash then): a run played without the sync (B at the start, a trial build, the game's sync switched off): the next
// start with the sync asks before this device's save goes up, if it changed since. off: the player switched this game's sync off (the pause menu)
typedef struct { int have; uint64_t seq; char sha[65]; int pending, conflict, force, auth; uint64_t fbase, cseq; int64_t ctime, last_up; char cdevice[64];
                 int offline_play, off; uint64_t op_seq; char op_sha[65]; } ss_state;
static int ss_hex64(const char *s) { if (strlen(s) != 64) return 0; for (; *s; s++) if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f'))) return 0; return 1; }
static void ss_state_text(const char *txt, ss_state *s) {
    memset(s, 0, sizeof *s); s->have = 1; char line[256];
    for (const char *p = txt; *p;) {
        const char *e = strchr(p, '\n'); size_t k = e ? (size_t)(e - p) : strlen(p); if (k >= sizeof line) k = sizeof line - 1;
        memcpy(line, p, k); line[k] = 0; p += k + (e != NULL); if (!e) p += strlen(p);
        ss_trim(line); char *eq = strchr(line, '='); if (!eq) continue; *eq = 0; const char *v = eq + 1;
        if (!strcmp(line, "seq")) s->seq = strtoull(v, NULL, 10);
        else if (!strcmp(line, "sha256")) { if (ss_hex64(v)) snprintf(s->sha, sizeof s->sha, "%s", v); }
        else if (!strcmp(line, "pending")) s->pending = atoi(v) != 0;
        else if (!strcmp(line, "conflict")) s->conflict = atoi(v) != 0;
        else if (!strcmp(line, "force")) s->force = atoi(v) != 0;
        else if (!strcmp(line, "fbase")) s->fbase = strtoull(v, NULL, 10);
        else if (!strcmp(line, "auth")) s->auth = atoi(v) != 0;
        else if (!strcmp(line, "cseq")) s->cseq = strtoull(v, NULL, 10);
        else if (!strcmp(line, "ctime")) s->ctime = strtoll(v, NULL, 10);
        else if (!strcmp(line, "cdevice")) snprintf(s->cdevice, sizeof s->cdevice, "%s", v);
        else if (!strcmp(line, "last_up")) s->last_up = strtoll(v, NULL, 10);
        else if (!strcmp(line, "offline_play")) s->offline_play = atoi(v) != 0;
        else if (!strcmp(line, "op_seq")) s->op_seq = strtoull(v, NULL, 10);
        else if (!strcmp(line, "op_sha256")) { if (ss_hex64(v)) snprintf(s->op_sha, sizeof s->op_sha, "%s", v); }
        else if (!strcmp(line, "off")) s->off = atoi(v) != 0;
    }
}
static int ss_state_format(const ss_state *s, char *out, size_t cap) {
    return snprintf(out, cap, "seq=%llu\nsha256=%s\npending=%d\nconflict=%d\ncseq=%llu\nctime=%lld\ncdevice=%s\nforce=%d\nfbase=%llu\nauth=%d\nlast_up=%lld\n"
                              "offline_play=%d\nop_seq=%llu\nop_sha256=%s\noff=%d\n",
        (unsigned long long)s->seq, s->sha, s->pending, s->conflict, (unsigned long long)s->cseq, (long long)s->ctime, s->cdevice, s->force, (unsigned long long)s->fbase, s->auth, (long long)s->last_up,
        s->offline_play, (unsigned long long)s->op_seq, s->op_sha, s->off);
}
static void ss_state_load(const char *path, ss_state *s) {   // no file: a first setup (have 0)
    size_t n = 0; char *t = ds_read(path, &n, 1 << 16);
    if (!t) { memset(s, 0, sizeof *s); return; }
    ss_state_text(t, s); free(t);
}
static int ss_state_save(const char *path, const ss_state *s) { char b[1024]; int n = ss_state_format(s, b, sizeof b); return n > 0 && (size_t)n < sizeof b ? ds_write(path, b, (size_t)n) : -1; }

// ---- the server's meta: {seq, sha256, size, time, device, gameVersion, build, session: {device, state, updated, dirty}} ----
// session.state: "open" (the start), "alive" (a heartbeat each minute while the game is on screen) or "closed" (onPause); dirty: that device has a save not yet uploaded
// (an older server sends no dirty: 0). updated: when the server took it
// time and updated: Unix seconds (milliseconds when the number is that large), or an ISO 8601 text ("2026-11-20T12:34:56Z", "...+09:00", fractions ignored)
typedef struct { uint64_t seq, size; int64_t files; char sha[65], device[64], game_version[32], build[64]; int64_t time; int session, s_open, s_dirty; char s_device[64]; int64_t s_updated; } ss_meta;   // s_open: open or alive; files: the save's file count (the meta's fileCount), -1 when the server keeps none
static int64_t ss_days(int y, int m, int d) { y -= m <= 2; int64_t era = (y >= 0 ? y : y - 399) / 400; int yoe = y - (int)era * 400, doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy; return era * 146097 + doe - 719468; }
static int ss_iso_time(const char *s, int64_t *out) {
    int Y, M, D, h = 0, mi = 0, se = 0, n = 0;
    if (sscanf(s, "%4d-%2d-%2d%n", &Y, &M, &D, &n) != 3 || M < 1 || M > 12 || D < 1 || D > 31) return -1;
    s += n;
    if (*s == 'T' || *s == ' ') { if (sscanf(s + 1, "%2d:%2d:%2d%n", &h, &mi, &se, &n) < 2) return -1; s += 1 + n; }
    if (*s == '.') { s++; while (*s >= '0' && *s <= '9') s++; }
    int64_t off = 0;
    if (*s == '+' || *s == '-') { int oh = 0, om = 0; if (sscanf(s + 1, "%2d:%2d", &oh, &om) < 1 && sscanf(s + 1, "%2d%2d", &oh, &om) < 1) return -1; off = (*s == '-' ? -1 : 1) * (int64_t)(oh * 3600 + om * 60); }
    *out = ss_days(Y, M, D) * 86400 + h * 3600 + mi * 60 + se - off; return 0;
}
static int ss_js_time(ds_js *j, int64_t *t) {
    ds_ws(j); if (j->p >= j->e) return -1;
    if (*j->p == '"') { char b[64]; if (ds_str(j, b, sizeof b)) return -1; if (ss_iso_time(b, t)) *t = 0; return 0; }
    const char *s = j->p; if (ds_skip(j, 1)) return -1;
    char b[32]; size_t k = (size_t)(j->p - s); if (k >= sizeof b) k = sizeof b - 1; memcpy(b, s, k); b[k] = 0;
    double v = strtod(b, NULL); if (v > 1e11) v /= 1000; *t = (int64_t)v; return 0;
}
static int ss_js_u64(ds_js *j, uint64_t *v) {   // a whole number (a fraction or exponent read as its whole part); null as 0
    ds_ws(j); const char *s = j->p; if (ds_skip(j, 1)) return -1;
    if (j->p - s >= 4 && !memcmp(s, "null", 4)) { *v = 0; return 0; }
    if (*s < '0' || *s > '9') return -1;
    *v = strtoull(s, NULL, 10); return 0;
}
static int ss_session_obj(ds_js *j, ss_meta *m) {
    char key[32], v[64]; ds_ws(j);
    if (j->e - j->p >= 4 && !memcmp(j->p, "null", 4)) { j->p += 4; return 0; }
    if (j->p >= j->e || *j->p++ != '{') return -1;
    m->session = 1; ds_ws(j); if (j->p < j->e && *j->p == '}') { j->p++; return 0; }
    for (;;) {
        if (ds_str(j, key, sizeof key)) return -1;
        ds_ws(j); if (j->p >= j->e || *j->p++ != ':') return -1;
        if (!strcmp(key, "device")) { if (ds_str(j, m->s_device, sizeof m->s_device)) return -1; }
        else if (!strcmp(key, "state")) { if (ds_str(j, v, sizeof v)) return -1; m->s_open = !strcmp(v, "open") || !strcmp(v, "alive"); }
        else if (!strcmp(key, "dirty")) { ds_ws(j); m->s_dirty = (j->e - j->p >= 4 && !memcmp(j->p, "true", 4)) || (j->p < j->e && *j->p == '1'); if (ds_skip(j, 1)) return -1; }
        else if (!strcmp(key, "updated")) { if (ss_js_time(j, &m->s_updated)) return -1; }
        else if (ds_skip(j, 1)) return -1;
        ds_ws(j); if (j->p >= j->e) return -1;
        if (*j->p == ',') { j->p++; continue; }
        if (*j->p++ != '}') return -1;
        return 0;
    }
}
static int ss_meta_obj(ds_js *j, ss_meta *m) {   // an object at j (null: -2)
    memset(m, 0, sizeof *m); m->files = -1; char key[32]; ds_ws(j);
    if (j->e - j->p >= 4 && !memcmp(j->p, "null", 4)) { j->p += 4; return -2; }
    if (j->p >= j->e || *j->p++ != '{') return -1;
    ds_ws(j); if (j->p < j->e && *j->p == '}') { j->p++; return -1; }
    for (;;) {
        if (ds_str(j, key, sizeof key)) return -1;
        ds_ws(j); if (j->p >= j->e || *j->p++ != ':') return -1;
        if (!strcmp(key, "seq")) { if (ss_js_u64(j, &m->seq)) return -1; }
        else if (!strcmp(key, "size")) { if (ss_js_u64(j, &m->size)) return -1; }
        else if (!strcmp(key, "fileCount")) { ds_ws(j); uint64_t v; if (j->e - j->p >= 4 && !memcmp(j->p, "null", 4)) { j->p += 4; } else { if (ss_js_u64(j, &v)) return -1; m->files = (int64_t)v; } }
        else if (!strcmp(key, "sha256")) { if (ds_str(j, m->sha, sizeof m->sha)) return -1; for (char *q = m->sha; *q; q++) if (*q >= 'A' && *q <= 'F') *q += 32; }
        else if (!strcmp(key, "time")) { if (ss_js_time(j, &m->time)) return -1; }
        else if (!strcmp(key, "device")) { ds_ws(j); if (j->p < j->e && *j->p == '"') { if (ds_str(j, m->device, sizeof m->device)) return -1; } else if (ds_skip(j, 1)) return -1; }
        else if (!strcmp(key, "gameVersion")) { ds_ws(j); if (j->p < j->e && *j->p == '"') { if (ds_str(j, m->game_version, sizeof m->game_version)) return -1; } else if (ds_skip(j, 1)) return -1; }
        else if (!strcmp(key, "build")) { ds_ws(j); if (j->p < j->e && *j->p == '"') { if (ds_str(j, m->build, sizeof m->build)) return -1; } else if (ds_skip(j, 1)) return -1; }
        else if (!strcmp(key, "session")) { if (ss_session_obj(j, m)) return -1; }
        else if (ds_skip(j, 1)) return -1;
        ds_ws(j); if (j->p >= j->e) return -1;
        if (*j->p == ',') { j->p++; continue; }
        if (*j->p++ != '}') return -1;
        break;
    }
    return ss_hex64(m->sha) || !m->seq ? 0 : -1;   // (a meta with a seq has its body's hash)
}
static int ss_meta_parse(const char *txt, size_t n, ss_meta *m) { ds_js j = {txt, txt + n}; return ss_meta_obj(&j, m) ? -1 : 0; }
// a 409's body {"current": meta|null}: 1 with the meta, 0 for null (the server has no save), -1 for anything else
static int ss_conflict_parse(const char *txt, size_t n, ss_meta *m) {
    ds_js j = {txt, txt + n}; char key[32]; int got = -1; memset(m, 0, sizeof *m);
    ds_ws(&j); if (j.p >= j.e || *j.p++ != '{') return -1;
    ds_ws(&j); if (j.p < j.e && *j.p == '}') return -1;
    for (;;) {
        if (ds_str(&j, key, sizeof key)) return -1;
        ds_ws(&j); if (j.p >= j.e || *j.p++ != ':') return -1;
        if (!strcmp(key, "current")) { int r = ss_meta_obj(&j, m); if (r == -1) return -1; got = r == -2 ? 0 : 1; }
        else if (ds_skip(&j, 1)) return -1;
        ds_ws(&j); if (j.p >= j.e) return -1;
        if (*j.p == ',') { j.p++; continue; }
        if (*j.p++ != '}') return -1;
        return got;
    }
}

// ---- the start's decision (before the game mounts its save) ----
enum { SS_NET_OK, SS_NET_NOTFOUND, SS_NET_UNAUTH, SS_NET_FAIL };   // GET meta: 200, 404, 401, anything else (offline, DNS, a timeout, 5xx)
enum { SS_GO_UPTODATE,   // the server has this device's save: the state takes the server's seq
       SS_GO_PENDING,    // this device's save is to go up (the server has an older one of this line, or none)
       SS_GO_NOTHING,    // neither has a save
       SS_GO_DOWNLOAD,   // the server's is newer and this device's did not change since the last sync (or it has none): take the server's
       SS_GO_CONFLICT,   // both changed since the last sync (or a first setup finds a save on both): the player chooses, nothing is overwritten by itself
       SS_GO_OFFLINE,    // the server did not answer: the game starts with the device's save
       SS_GO_AUTH,       // the server refused the token: as offline, and the menu says so
       SS_GO_ASK_OFFLINE };   // a run without the sync changed this device's save and the server did not move: the player chooses (upload it, or go back to the server's)
// local: this device's archive hash, NULL when it has no save. *pending: whether an upload is owed afterwards (a conflict or a question owes none until the player chooses;
// a run played without the sync owes none either: the next start with the sync asks). The caller clears offline_play after any decision with the server's answer but
// SS_GO_ASK_OFFLINE and SS_GO_CONFLICT (those once the player chose)
static int ss_decide(int net, const ss_meta *m, const ss_state *st, const char *local, int *pending) {
    int changed = local ? strcmp(local, st->sha) != 0 : st->sha[0] != 0;   // since the last sync
    *pending = 0;
    if (net == SS_NET_FAIL || net == SS_NET_UNAUTH) { *pending = local && (changed || st->pending || st->force) && !st->conflict && !st->offline_play; return net == SS_NET_FAIL ? SS_GO_OFFLINE : SS_GO_AUTH; }
    if (net == SS_NET_NOTFOUND || !m->seq) { *pending = local != NULL; return local ? SS_GO_PENDING : SS_GO_NOTHING; }
    if (local && !strcmp(local, m->sha)) return SS_GO_UPTODATE;   // the same save on both (also a first setup that found the same one)
    if (!local) return SS_GO_DOWNLOAD;                              // nothing here to lose
    if (!st->have) return SS_GO_CONFLICT;                           // a first setup with a save on both: the player chooses
    if (st->offline_play && changed) return m->seq == st->seq ? SS_GO_ASK_OFFLINE : SS_GO_CONFLICT;   // played without the sync: never uploaded by itself
    if (st->force && m->seq == st->fbase) { *pending = 1; return SS_GO_PENDING; }   // the player chose this device's save, and the server did not move since
    if (m->seq == st->seq) { *pending = changed; return changed ? SS_GO_PENDING : SS_GO_UPTODATE; }
    if (m->seq > st->seq && !changed) return SS_GO_DOWNLOAD;
    return SS_GO_CONFLICT;   // the server moved and this device's save changed too (or the server went back to an older seq: neither is overwritten by itself)
}
// whether the start's sync asks the server at all: not when the device has no network (dev_net 0; -1 not known asks), nor when the data's step of this start could
// not reach its server just now (data_unreached; an HTTP error is not that). Then the game starts at once with this device's save, as SS_NET_FAIL decides
// (SS_GO_OFFLINE: what changed is owed and goes up once there is a connection), without the meta request that could only time out and without the "cannot be
// reached" question. A first failure of the cloud's own request (the data's server answered) still asks
static int ss_start_offline(int dev_net, int data_unreached) { return dev_net == 0 || data_unreached; }
// whether this run syncs: SS_RUN_NONE no sync.txt (nothing at all, as before the sync existed); SS_RUN_SUPPRESSED a trial build (an APK without a data server, a debuggable
// one, env.txt's DM_SAVESYNC=off) or the game's sync switched off: no network, the run is marked offline_play; SS_RUN_SYNC the start's sync
enum { SS_RUN_NONE, SS_RUN_SUPPRESSED, SS_RUN_SYNC };
static int ss_run_mode(int cfg_on, int trial, int switched_off) { return !cfg_on ? SS_RUN_NONE : trial || switched_off ? SS_RUN_SUPPRESSED : SS_RUN_SYNC; }
// a run without the sync: marked so the next start with it asks before this device's save goes up (op_*: the last sync's seq and hash, kept as they were)
static void ss_mark_offline_play(ss_state *s) { if (!s->offline_play) { s->offline_play = 1; s->op_seq = s->seq; snprintf(s->op_sha, sizeof s->op_sha, "%s", s->sha); } s->have = 1; }
// Another device and the game, from the session's last signal (now: this device's clock, Unix seconds). The apps upload each save within seconds, so whether the cloud
// has the latest progress is whether that device's last save went up (dirty), not whether it closed the game: an app killed before its close leaves an open session
// that warns nobody once its heartbeats stop.
#define SS_DIRTY_DAYS 30      // a dirty signal older than this: that device is taken as gone
#define SS_ALIVE_SECS 180     // three heartbeats missed: not running any more
enum { SS_OTHER_NONE,         // no session, this device's, closed and clean, or open but silent (killed)
       SS_OTHER_RUNNING,      // running there now, its saves up: a note only, the start goes on
       SS_OTHER_UNSYNCED };   // that device has a save the cloud has not got: the warning before the decision (Start anyway / Cancel)
static int ss_other_device(const ss_meta *m, const char *me, int64_t now) {
    if (!m->session || !m->s_device[0] || !strcmp(m->s_device, me) || m->s_updated <= 0) return SS_OTHER_NONE;
    int64_t age = now - m->s_updated;
    if (age <= -86400) return SS_OTHER_NONE;   // (a clock a day or more off: nothing to go by)
    if (m->s_dirty && age < SS_DIRTY_DAYS * 86400) return SS_OTHER_UNSYNCED;
    if (m->s_open && age < SS_ALIVE_SECS) return SS_OTHER_RUNNING;
    return SS_OTHER_NONE;
}

// ---- times as the screens say them: "11월 20일 오후 9:12" / "Nov 20, 9:12 PM", and how long ago: "방금", "3분 전", "2시간 전", "어제", "3일 전" ----
static void ss_fmt_date(char *o, size_t n, int64_t t, int ko) {
    if (t <= 0) { snprintf(o, n, "-"); return; }
    time_t tt = (time_t)t; struct tm tm; localtime_r(&tt, &tm); int h12 = tm.tm_hour % 12 ? tm.tm_hour % 12 : 12;
    static const char *const mon[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    if (ko) snprintf(o, n, "%d월 %d일 %s %d:%02d", tm.tm_mon + 1, tm.tm_mday, tm.tm_hour < 12 ? "오전" : "오후", h12, tm.tm_min);
    else snprintf(o, n, "%s %d, %d:%02d %s", mon[tm.tm_mon], tm.tm_mday, h12, tm.tm_min, tm.tm_hour < 12 ? "AM" : "PM");
}
static void ss_fmt_ago(char *o, size_t n, int64_t t, int64_t now, int ko) {
    if (t <= 0) { snprintf(o, n, "-"); return; }
    int64_t d = now - t; if (d < 0) d = 0;
    if (d < 60) snprintf(o, n, ko ? "방금" : "just now");
    else if (d < 3600) snprintf(o, n, ko ? "%d분 전" : "%d min ago", (int)(d / 60));
    else if (d < 86400) snprintf(o, n, ko ? "%d시간 전" : "%d h ago", (int)(d / 3600));
    else if (d < 2 * 86400) snprintf(o, n, ko ? "어제" : "yesterday");
    else snprintf(o, n, ko ? "%d일 전" : "%d days ago", (int)(d / 86400));
}
// The archive's file count (folders not counted), as the start screen's card shows it beside the size; -1 for an archive ss_untar would not take
static int64_t ss_tar_files(const uint8_t *p, size_t n) {
    int64_t files = 0; char name[512]; const char *why = NULL;
    for (size_t o = 0; o + 512 <= n;) {
        uint64_t size = 0; int k = ss_tar_entry(p + o, name, sizeof name, &size, &why);
        if (k < 0) return -1;
        if (k == 2) return files;
        if (k == 0) files++;
        o += 512 + (size_t)((size + 511) / 512 * 512);
    }
    return -1;
}
static void ss_fmt_size(char *o, size_t n, uint64_t b) {   // "1.2 MB", "640 KB", "12 MB"
    double m = b / 1048576.0;
    if (b < 1024 * 1024) snprintf(o, n, "%llu KB", (unsigned long long)((b + 1023) / 1024));
    else snprintf(o, n, m < 10 ? "%.1f MB" : "%.0f MB", m);
}
// a query value: unreserved bytes as they are, the rest percent-encoded
static void ss_qenc(char *o, size_t n, const char *s) {
    size_t k = 0; if (!n) return;
    for (; *s && k + 4 < n; s++) { unsigned char c = (unsigned char)*s;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') o[k++] = (char)c;
        else k += (size_t)snprintf(o + k, n - k, "%%%02X", c); }
    o[k] = 0;
}

// ---- while the game runs (savecloud_android.c's upload thread): what a tick does, what an upload's answer does to the state, which corner notes show. No question is ever
// asked once the game runs: a conflict found then is kept for the next start's cards ----
// A run that started offline (or lost the network) has no open session on the server: once the network is back it opens one (dirty while a save is owed) and the heartbeats
// follow; an owed upload is tried at once then, its backoff forgotten. A run without the sync (B at the start, a trial build, the game's sync off) sends nothing, ever
typedef struct {
    int syncs;               // this run syncs (not skipped, not a trial build, not switched off, the token not refused)
    int net;                 // the device has a network now (ConnectivityManager): 1, 0, or -1 when not known
    int was_net;             // the last tick's net
    int sess_open;           // the server took this run's open (or a heartbeat)
    int owed, conflict;      // a save the cloud has not got; a conflict waits for the next start
    int committed;           // a burst of the game's commits ended (no new one for a poll: about 3 s after it saved) and has not gone up
    int64_t now, next_try, last_hb;   // monotonic ms: when an owed upload is tried again (the backoff), the last heartbeat
} ss_tick_in;
enum { SS_DO_UPLOAD = 1, SS_DO_OPEN = 2, SS_DO_ALIVE = 4, SS_DO_RETRY_NOW = 8 };   // SS_DO_RETRY_NOW: the backoff forgotten (the network came back)
static int ss_tick(const ss_tick_in *t) {
    if (!t->syncs) return 0;
    if (t->net == 0) return 0;   // known offline: nothing is tried (no request that can only time out)
    int back = t->net == 1 && t->was_net == 0, act = back ? SS_DO_RETRY_NOW : 0;
    if (!t->conflict && (t->committed || (t->owed && (back || t->now >= t->next_try)))) act |= SS_DO_UPLOAD;
    if (!t->sess_open) act |= SS_DO_OPEN;   // (an open that failed is tried at each tick the network is up, as a heartbeat would be)
    else if (t->now - t->last_hb >= 60000) act |= SS_DO_ALIVE;
    return act;
}
// An upload's answer (code: HTTP, 0 no answer) into the state; returns the note it calls for (SS_NOTE_*), the caller passes it to ss_up_end
enum { SS_NOTE_NONE, SS_NOTE_SAVED, SS_NOTE_WAITING, SS_NOTE_CONFLICT, SS_NOTE_OFF, SS_NOTE_TRIAL, SS_NOTE_N };
static int ss_after_put(ss_state *st, int code, uint64_t seq, const char *sha, const ss_meta *cur, int64_t now_s) {
    if (code == 200) { if (seq) st->seq = seq; snprintf(st->sha, sizeof st->sha, "%s", sha); st->pending = st->force = 0; st->last_up = now_s; return SS_NOTE_SAVED; }
    if (code == 409) {   // another device's save is newer: not overwritten, the next start asks
        st->conflict = 1; st->pending = 1; st->cseq = cur ? cur->seq : 0; st->ctime = cur ? cur->time : 0; snprintf(st->cdevice, sizeof st->cdevice, "%s", cur ? cur->device : "");
        return SS_NOTE_CONFLICT;
    }
    if (code == 401 || code == 403) { st->auth = 1; return SS_NOTE_NONE; }
    st->pending = 1; return code ? SS_NOTE_NONE : SS_NOTE_WAITING;
}
// The corner marks (savecloud_android.c dm_cloud_mark, drawn by gfx_vk_present.c ov_mark). Every upload shows: the spinner from its start, and each one that went up
// (200) its check. An upload that starts less than SS_JOIN_MS after the last one went up keeps the spinner turning (it never stops and starts again), and the check
// shows once SS_JOIN_MS passed with no new upload: a game that saves every second shows one spinner and one check, and no check is shown that the next upload's
// spinner would cut. Waiting (no answer: no network) and a conflict are notes of a change of state: never the same kind twice in a row nor within 5 minutes of the
// last one of its kind (a network that comes and goes, a retry: one note); a check in between is a change of state
#define SS_NOTE_GAP_MS (5 * 60 * 1000)
#define SS_JOIN_MS 3000
enum { SS_MARK_NONE, SS_MARK_BUSY, SS_MARK_SAVED, SS_MARK_WAITING, SS_MARK_CONFLICT };   // (savecloud.h DM_CLOUD_MARK_* in the same order)
#define SS_MARK_SAVED_MS 1500      // how long each outcome's mark shows (savecloud.h DM_CLOUD_MARK_*_MS, the drawing's fade)
#define SS_MARK_WAITING_MS 1500
#define SS_MARK_CONFLICT_MS 2500
typedef struct {
    int last; int64_t at[SS_NOTE_N];   // the last note's kind, when each kind last showed (monotonic ms)
    int busy; int64_t spin_t, ok_t;    // an upload runs; when the spinner began; when the last upload went up (0: no check owed)
    int mark; int64_t mark_t;          // a waiting or conflict note showing (SS_MARK_*), and since when
} ss_notes;
static int ss_note_ok(ss_notes *n, int kind, int64_t now_ms) {   // a waiting or conflict note: whether it shows
    if (kind <= SS_NOTE_NONE || kind >= SS_NOTE_N) return 0;
    if (kind == n->last || (n->at[kind] && now_ms - n->at[kind] < SS_NOTE_GAP_MS)) return 0;
    n->last = kind; n->at[kind] = now_ms ? now_ms : 1; return 1;
}
static void ss_up_begin(ss_notes *n, int64_t now_ms) {   // an upload starts: the spinner, or the one still turning after the last went up
    if (!(n->ok_t && now_ms - n->ok_t < SS_JOIN_MS)) n->spin_t = now_ms;
    n->ok_t = 0; n->busy = 1; n->mark = SS_MARK_NONE;
}
static void ss_up_end(ss_notes *n, int note, int64_t now_ms) {   // its answer's note (ss_after_put's SS_NOTE_*): a check owed, or a note when ss_note_ok lets it show
    n->busy = 0; n->ok_t = 0;
    if (note == SS_NOTE_SAVED) { n->ok_t = now_ms ? now_ms : 1; n->last = SS_NOTE_SAVED; n->at[SS_NOTE_SAVED] = n->ok_t; return; }
    if ((note == SS_NOTE_WAITING || note == SS_NOTE_CONFLICT) && ss_note_ok(n, note, now_ms)) { n->mark = note == SS_NOTE_WAITING ? SS_MARK_WAITING : SS_MARK_CONFLICT; n->mark_t = now_ms; }
}
static int ss_mark_now(ss_notes *n, int64_t now_ms, int64_t *age) {   // the mark to draw now (SS_MARK_*), *age: ms since it began
    if (n->busy) { *age = now_ms - n->spin_t; return SS_MARK_BUSY; }
    if (n->ok_t) {
        int64_t d = now_ms - n->ok_t;
        if (d < SS_JOIN_MS) { *age = now_ms - n->spin_t; return SS_MARK_BUSY; }   // (the spinner turns on, in case another upload follows)
        if (d - SS_JOIN_MS < SS_MARK_SAVED_MS) { *age = d - SS_JOIN_MS; return SS_MARK_SAVED; }
        n->ok_t = 0;
    }
    if (n->mark) {
        int64_t a = now_ms - n->mark_t;
        if (a < (n->mark == SS_MARK_WAITING ? SS_MARK_WAITING_MS : SS_MARK_CONFLICT_MS)) { *age = a; return n->mark; }
        n->mark = SS_MARK_NONE;
    }
    return SS_MARK_NONE;
}
