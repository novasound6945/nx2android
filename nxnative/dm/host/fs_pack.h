// The RomFS's small files read in place from the data pack's packs (android.tsv romfs_packs: porter/datapack.py): uncompressed tar files packs/pNNNN.tar, never
// unpacked, and their index packs/romfs.nxbi, which says for every file the pack it is in, where its bytes start and how many there are. The index is mapped read-only
// and searched where it lies (no allocation per file: Fire Emblem Engage has 205,000 of them, 38,000 in one folder).
// The index, NXBI version 1 (little-endian, written by porter/datapack.py write_index):
//   header, 32 bytes: "NXBI", u32 version (1), u32 npacks, u32 nfiles, u64 names_bytes, u64 0
//   the packs, npacks x 16 bytes: u64 the pack file's size, u64 0
//   the files, nfiles x 24 bytes: u32 name_off, u16 name_len, u16 pack, u64 data_off, u64 size; sorted by their names' bytes (memcmp, a name that is the start of
//     another first), each name once
//   the names, names_bytes bytes, without NULs. A name is the file's path from the RomFS's root, '/'-separated, without a '/' at either end ("Data/a.bin"), in the case
//     the game asks for it in (names are compared byte for byte: "A.bin" and "a.bin" are two files)
// A pack is the file p<pack, 4 digits>.tar beside the index; a file's bytes are data_off..data_off+size in it (data_off is past the file's tar header, which
// pk_verify reads back). Folders are not listed: a folder is the names that start with its path and a '/'.
// No host or Android code here (POSIX only), so tests/test_fs_pack.py builds it on the computer (tests/fs_pack_fixture.c).
// Use: pk_load the index once; pk_find a path (pk_ent: its pack, start, size); pk_open the pack (a dup of the pack's descriptor, which the index keeps open from its
// first use: close it when done); pk_pread within the file. pk_is_dir, pk_dir_begin / pk_dir_next, pk_count: the folders. All of them can be called from any thread
// at once (pk_open and pk_verify take a lock; the others only read the mapping); pk_free when no call is running.
#ifndef FS_PACK_H
#define FS_PACK_H
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

_Static_assert(sizeof(off_t) >= 8, "fs_pack.h needs a 64-bit off_t (packs and their offsets pass 2 GB)");

#define PK_MAGIC "NXBI"
#define PK_VERSION 1u
#define PK_HEAD 32u
#define PK_PACK_REC 16u
#define PK_ENT_REC 24u
#define PK_PACKS_MAX 65536u   // the pack field is 16 bits
#define PK_TAR_BLOCK 512u

typedef struct {   // one file of the index (pk_find)
    uint32_t pack;       // its pack's number (p%04d.tar)
    uint64_t data_off;   // where its bytes start in the pack
    uint64_t size;       // its bytes
} pk_ent;

typedef struct {
    const uint8_t *map; size_t map_len;     // the index, mapped read-only
    uint32_t npacks, nfiles;
    const uint8_t *packs, *ents;            // the pack and file records in the mapping
    const char *names; uint64_t names_bytes;
    char *dir;                              // the index's folder: the packs are there
    pthread_mutex_t mu; int *fd;            // each pack's descriptor, -1 until its first pk_open
} pk_t;

typedef struct {   // a folder's walk (pk_dir_begin, pk_dir_next): its entries are the files [i, end), which all start with the folder's path and a '/' (plen bytes)
    uint32_t i, end, plen;
} pk_cur;

static inline uint16_t pk_rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t pk_rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline uint64_t pk_rd64(const uint8_t *p) { return (uint64_t)pk_rd32(p) | (uint64_t)pk_rd32(p + 4) << 32; }

static inline const uint8_t *pk_rec(const pk_t *pk, uint32_t i) { return pk->ents + (size_t)i * PK_ENT_REC; }
static inline const char *pk_name(const pk_t *pk, uint32_t i, size_t *len) { const uint8_t *r = pk_rec(pk, i); *len = pk_rd16(r + 4); return pk->names + pk_rd32(r); }
static inline uint64_t pk_pack_size(const pk_t *pk, uint32_t k) { return pk_rd64(pk->packs + (size_t)k * PK_PACK_REC); }

static void pk_err(char *err, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void pk_err(char *err, size_t cap, const char *fmt, ...) {
    if (!err || !cap) return;
    va_list a; va_start(a, fmt); vsnprintf(err, cap, fmt, a); va_end(a);
}

// a name against key a (alen bytes) followed by one more byte c (c < 0: none), as the index sorts them: <0, 0, >0
static int pk_cmp(const char *n, size_t nlen, const char *a, size_t alen, int c) {
    size_t m = nlen < alen ? nlen : alen;
    int r = m ? memcmp(n, a, m) : 0;
    if (r) return r;
    if (nlen < alen) return -1;
    if (c < 0) return nlen > alen;
    if (nlen == alen) return -1;   // the name is the key's start: before key + c
    unsigned char b = (unsigned char)n[alen];
    return b < (unsigned)c ? -1 : b > (unsigned)c;
}

// the first of the files [lo, hi) not before a + c (pk_cmp)
static uint32_t pk_lower(const pk_t *pk, uint32_t lo, uint32_t hi, const char *a, size_t alen, int c) {
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2; size_t nl; const char *n = pk_name(pk, mid, &nl);
        if (pk_cmp(n, nl, a, alen, c) < 0) lo = mid + 1; else hi = mid;
    }
    return lo;
}

static void pk_free(pk_t *pk) {
    if (!pk) return;
    if (pk->fd) for (uint32_t k = 0; k < pk->npacks; k++) if (pk->fd[k] >= 0) close(pk->fd[k]);
    if (pk->map) munmap((void *)pk->map, pk->map_len);
    pthread_mutex_destroy(&pk->mu);
    free(pk->fd); free(pk->dir); free(pk);
}

// The index at index_path, checked: its header, its size against its counts, and every file's record (its name inside the names and free of NULs, of empty parts and
// of a '/' at either end; its pack one of the index's; its bytes inside that pack, past a tar header). That the files are sorted, and that each record points at its
// file's tar header, is pk_verify's (it reads every pack). NULL with the reason in err for an index that is not one
static pk_t *pk_load(const char *index_path, char *err, size_t errcap) {
    int fd = open(index_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { pk_err(err, errcap, "cannot open %s (%s)", index_path, strerror(errno)); return NULL; }
    struct stat st;
    if (fstat(fd, &st)) { pk_err(err, errcap, "cannot stat %s (%s)", index_path, strerror(errno)); close(fd); return NULL; }
    uint64_t len = (uint64_t)st.st_size;
    if (len < PK_HEAD) { pk_err(err, errcap, "%s: %llu bytes, shorter than the header", index_path, (unsigned long long)len); close(fd); return NULL; }
    if (len != (uint64_t)(size_t)len) { pk_err(err, errcap, "%s: too big to map", index_path); close(fd); return NULL; }
    void *m = mmap(NULL, (size_t)len, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) { pk_err(err, errcap, "cannot map %s (%s)", index_path, strerror(errno)); return NULL; }
    pk_t *pk = calloc(1, sizeof *pk);
    if (!pk) { munmap(m, (size_t)len); pk_err(err, errcap, "out of memory"); return NULL; }
    pthread_mutex_init(&pk->mu, NULL);
    pk->map = m; pk->map_len = (size_t)len;
    const uint8_t *h = pk->map;
    uint32_t ver = pk_rd32(h + 4);
    pk->npacks = pk_rd32(h + 8); pk->nfiles = pk_rd32(h + 12); pk->names_bytes = pk_rd64(h + 16);
    if (memcmp(h, PK_MAGIC, 4)) { pk_err(err, errcap, "%s: not an NXBI index", index_path); goto bad; }
    if (ver != PK_VERSION) { pk_err(err, errcap, "%s: NXBI version %u, this host reads version %u", index_path, ver, PK_VERSION); goto bad; }
    if (pk_rd64(h + 24)) { pk_err(err, errcap, "%s: the header's reserved field is not 0", index_path); goto bad; }
    if (pk->npacks > PK_PACKS_MAX) { pk_err(err, errcap, "%s: %u packs, more than a file record can name", index_path, pk->npacks); goto bad; }
    uint64_t want = (uint64_t)PK_HEAD + (uint64_t)pk->npacks * PK_PACK_REC + (uint64_t)pk->nfiles * PK_ENT_REC;
    if (pk->names_bytes > len || want + pk->names_bytes != len) {
        pk_err(err, errcap, "%s: %llu bytes, where its counts (%u packs, %u files, %llu bytes of names) make %llu", index_path, (unsigned long long)len,
               pk->npacks, pk->nfiles, (unsigned long long)pk->names_bytes, (unsigned long long)(want + pk->names_bytes));
        goto bad;
    }
    pk->packs = h + PK_HEAD; pk->ents = pk->packs + (size_t)pk->npacks * PK_PACK_REC; pk->names = (const char *)(pk->ents + (size_t)pk->nfiles * PK_ENT_REC);
    for (uint32_t k = 0; k < pk->npacks; k++)
        if (pk_rd64(pk->packs + (size_t)k * PK_PACK_REC + 8)) { pk_err(err, errcap, "%s: pack %u's reserved field is not 0", index_path, k); goto bad; }
    for (uint32_t i = 0; i < pk->nfiles; i++) {
        const uint8_t *r = pk_rec(pk, i);
        uint64_t off = pk_rd32(r), nl = pk_rd16(r + 4), dat = pk_rd64(r + 8), size = pk_rd64(r + 16); uint32_t pack = pk_rd16(r + 6);
        if (!nl || off + nl > pk->names_bytes) { pk_err(err, errcap, "%s: file %u's name (%llu bytes at %llu) is not inside the names (%llu bytes)", index_path, i,
                                                        (unsigned long long)nl, (unsigned long long)off, (unsigned long long)pk->names_bytes); goto bad; }
        const char *n = pk->names + off;
        if (n[0] == '/' || n[nl - 1] == '/' || memchr(n, 0, (size_t)nl)) { pk_err(err, errcap, "%s: file %u's name is not a path from the RomFS's root", index_path, i); goto bad; }
        for (uint64_t j = 1; j < nl; j++) if (n[j] == '/' && n[j - 1] == '/') { pk_err(err, errcap, "%s: file %u's name has an empty part", index_path, i); goto bad; }
        if (pack >= pk->npacks) { pk_err(err, errcap, "%s: file %u (%.*s) is in pack %u of %u", index_path, i, (int)nl, n, pack, pk->npacks); goto bad; }
        uint64_t psz = pk_pack_size(pk, pack);
        if (dat < PK_TAR_BLOCK || dat % PK_TAR_BLOCK || dat > psz || size > psz - dat) {
            pk_err(err, errcap, "%s: file %u (%.*s): %llu bytes at %llu do not fit in pack %u (%llu bytes, past a tar header)", index_path, i, (int)nl, n,
                   (unsigned long long)size, (unsigned long long)dat, pack, (unsigned long long)psz);
            goto bad;
        }
    }
    const char *slash = strrchr(index_path, '/');
    size_t dl = slash ? (size_t)(slash - index_path) + (slash == index_path) : 1;   // "/" for an index in the root, "." for one without a folder
    if ((pk->dir = malloc(dl + 1))) { memcpy(pk->dir, slash ? index_path : ".", dl); pk->dir[dl] = 0; }
    pk->fd = malloc(sizeof *pk->fd * (pk->npacks ? pk->npacks : 1));
    if (!pk->dir || !pk->fd) { pk_err(err, errcap, "out of memory"); goto bad; }
    for (uint32_t k = 0; k < pk->npacks; k++) pk->fd[k] = -1;
    return pk;
bad:
    pk_free(pk);
    return NULL;
}

// whether path (from the RomFS's root, no '/' at either end) is a file of the index: 1 and its record in out, else 0. Exact bytes: no case folding
static int pk_find(const pk_t *pk, const char *path, pk_ent *out) {
    size_t len = strlen(path);
    if (!len || !pk->nfiles) return 0;
    uint32_t i = pk_lower(pk, 0, pk->nfiles, path, len, -1);
    if (i >= pk->nfiles) return 0;
    size_t nl; const char *n = pk_name(pk, i, &nl);
    if (nl != len || memcmp(n, path, len)) return 0;
    if (out) { const uint8_t *r = pk_rec(pk, i); out->pack = pk_rd16(r + 6); out->data_off = pk_rd64(r + 8); out->size = pk_rd64(r + 16); }
    return 1;
}

// the files under folder dir ("" the root): [*lo, *hi); 0 when there are none
static int pk_range(const pk_t *pk, const char *dir, uint32_t *lo, uint32_t *hi, uint32_t *plen) {
    size_t len = strlen(dir);
    if (!len) { *lo = 0; *hi = pk->nfiles; *plen = 0; return pk->nfiles > 0; }
    if (len > 0xFFFF) return 0;
    *lo = pk_lower(pk, 0, pk->nfiles, dir, len, '/');
    *hi = pk_lower(pk, *lo, pk->nfiles, dir, len, '0');   // '0' is the byte after '/': past every name that starts with dir + '/'
    *plen = (uint32_t)len + 1;
    return *lo < *hi;
}

// whether dir is a folder of the index: a file's name starts with dir + '/'. "" is the root, a folder when the index has a file
static int pk_is_dir(const pk_t *pk, const char *dir) {
    uint32_t lo, hi, plen;
    return pk_range(pk, dir, &lo, &hi, &plen);
}

// A walk of folder dir's own entries (pk_dir_next): 1, or 0 (cur empty) when dir is not a folder of the index
static int pk_dir_begin(const pk_t *pk, const char *dir, pk_cur *cur) {
    uint32_t lo, hi, plen;
    if (!pk_range(pk, dir, &lo, &hi, &plen)) { cur->i = cur->end = cur->plen = 0; return 0; }
    cur->i = lo; cur->end = hi; cur->plen = plen;
    return 1;
}

// The walk's next entry, in the index's order (a folder where its first file is): 1 with its name (name_len bytes, not NUL-terminated: it points into the index),
// whether it is a folder, and a file's size (0 for a folder); 0 at the end. Each entry comes once: a folder's files are skipped over in one search
static int pk_dir_next(const pk_t *pk, pk_cur *cur, const char **name, size_t *name_len, int *is_dir, uint64_t *size) {
    if (cur->i >= cur->end) return 0;
    size_t nl; const char *n = pk_name(pk, cur->i, &nl);
    const char *s = n + cur->plen; size_t sl = nl - cur->plen;
    const char *sl_ = memchr(s, '/', sl);
    if (!sl_) {
        if (name) *name = s;
        if (name_len) *name_len = sl;
        if (is_dir) *is_dir = 0;
        if (size) *size = pk_rd64(pk_rec(pk, cur->i) + 16);
        cur->i++;
        return 1;
    }
    size_t cl = (size_t)(sl_ - s);
    if (name) *name = s;
    if (name_len) *name_len = cl;
    if (is_dir) *is_dir = 1;
    if (size) *size = 0;
    cur->i = pk_lower(pk, cur->i + 1, cur->end, n, cur->plen + cl, '0');   // past every name that starts with this folder's path + '/' (they are this one's prefix + child)
    return 1;
}

// the number of folder dir's own files (want_files), folders (want_dirs) or both; 0 for a dir that is not a folder of the index
static int pk_count(const pk_t *pk, const char *dir, int want_files, int want_dirs) {
    pk_cur c; int n = 0, d;
    if (!pk_dir_begin(pk, dir, &c)) return 0;
    while (pk_dir_next(pk, &c, NULL, NULL, &d, NULL)) if (d ? want_dirs : want_files) n++;
    return n;
}

// pack k's descriptor, opened at its first use and kept (pk->mu held); -1 with errno for one that cannot be opened or whose size is not the index's
static int pk_pack_fd(pk_t *pk, uint32_t k) {
    if (k >= pk->npacks) { errno = EINVAL; return -1; }
    if (pk->fd[k] >= 0) return pk->fd[k];
    char p[4096];
    if (snprintf(p, sizeof p, "%s/p%04u.tar", pk->dir, k) >= (int)sizeof p) { errno = ENAMETOOLONG; return -1; }
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st)) { int e = errno; close(fd); errno = e; return -1; }
    if ((uint64_t)st.st_size != pk_pack_size(pk, k)) { close(fd); errno = EIO; return -1; }   // another pack than the index's (a half download, an older port's)
    pk->fd[k] = fd;
    return fd;
}

// A descriptor to read e's file with (pk_pread): a dup of its pack's (close-on-exec), the pack opened at its first use and checked against the index's size. 0, or -1
// with errno (EIO: the pack's size is not the index's). Thread-safe
static int pk_open(pk_t *pk, const pk_ent *e, int *fd) {
    pthread_mutex_lock(&pk->mu);
    int base = pk_pack_fd(pk, e->pack), d = -1, err = errno;
    if (base >= 0) { d = fcntl(base, F_DUPFD_CLOEXEC, 0); err = errno; }
    pthread_mutex_unlock(&pk->mu);
    if (d < 0) { errno = err; return -1; }
    *fd = d;
    return 0;
}

// up to n of e's bytes from its offset off, through fd (pk_open): one pread, its result as it is (a short read, or -1 with errno EINTR, is the caller's to go on
// from); 0 at or past the file's end, and never a byte past it
static ssize_t pk_pread(int fd, const pk_ent *e, void *buf, size_t n, uint64_t off) {
    if (off >= e->size) return 0;
    if (n > e->size - off) n = (size_t)(e->size - off);
    return pread(fd, buf, n, (off_t)(e->data_off + off));
}

static uint64_t pk_octal(const uint8_t *p, size_t n, int *ok) {   // a tar header's number field: octal digits, after spaces, ending at a space or NUL
    uint64_t v = 0; size_t i = 0; *ok = 0;
    while (i < n && p[i] == ' ') i++;
    size_t d = i;
    for (; i < n && p[i] >= '0' && p[i] <= '7'; i++) { if (v >> 61) return 0; v = v << 3 | (uint64_t)(p[i] - '0'); }
    if (i == d || (i < n && p[i] != ' ' && p[i] != 0)) return 0;
    *ok = 1;
    return v;
}

// Every file of the index checked against the packs (DM_ROMFS_PACK_VERIFY: slow, it reads one tar header per file): the files sorted, each name once, and the tar
// header before each file's bytes a regular file's whose size is the index's and whose name is "RomFS/" + the file's (its first 100 bytes for a longer name: tar
// keeps the whole name in a GNU long-name entry before it). 0, or -1 with the first fault in err
static int pk_verify(pk_t *pk, char *err, size_t errcap) {
    static const char pre[] = "RomFS/";
    const size_t pl = sizeof pre - 1;
    for (uint32_t i = 0; i < pk->nfiles; i++) {
        size_t nl; const char *n = pk_name(pk, i, &nl);
        if (i) {
            size_t ql; const char *q = pk_name(pk, i - 1, &ql);
            if (pk_cmp(q, ql, n, nl, -1) >= 0) { pk_err(err, errcap, "files %u (%.*s) and %u (%.*s) are not in order", i - 1, (int)ql, q, i, (int)nl, n); return -1; }
        }
        const uint8_t *r = pk_rec(pk, i);
        uint32_t pack = pk_rd16(r + 6); uint64_t dat = pk_rd64(r + 8), size = pk_rd64(r + 16);
        pthread_mutex_lock(&pk->mu);
        int fd = pk_pack_fd(pk, pack), e = errno;
        pthread_mutex_unlock(&pk->mu);
        if (fd < 0) { pk_err(err, errcap, "pack %u of file %u (%.*s): %s", pack, i, (int)nl, n, e == EIO ? "its size is not the index's" : strerror(e)); return -1; }
        uint8_t h[PK_TAR_BLOCK];
        ssize_t got;
        do got = pread(fd, h, sizeof h, (off_t)(dat - PK_TAR_BLOCK)); while (got < 0 && errno == EINTR);
        if (got != (ssize_t)sizeof h) { pk_err(err, errcap, "file %u (%.*s): cannot read its tar header in pack %u", i, (int)nl, n, pack); return -1; }
        int ok; uint64_t tsz = pk_octal(h + 124, 12, &ok);
        if (!ok || tsz != size || (h[156] != '0' && h[156] != 0)) {
            pk_err(err, errcap, "file %u (%.*s): the tar header at %llu in pack %u is not a regular file of %llu bytes", i, (int)nl, n,
                   (unsigned long long)(dat - PK_TAR_BLOCK), pack, (unsigned long long)size);
            return -1;
        }
        size_t full = pl + nl, cmp = full < 100 ? full : 100, a = cmp < pl ? cmp : pl;   // the header's name field: 100 bytes, NUL-padded when shorter
        if (memcmp(h, pre, a) || memcmp(h + a, n, cmp - a) || (full < 100 && h[full] != 0)) {
            pk_err(err, errcap, "file %u (%.*s): the tar header at %llu in pack %u is another file's (%.100s)", i, (int)nl, n, (unsigned long long)(dat - PK_TAR_BLOCK), pack, (const char *)h);
            return -1;
        }
    }
    return 0;
}

#endif
