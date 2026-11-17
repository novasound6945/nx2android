// The RomFS pack under the rom mount (android.tsv romfs_packs: DM_ROMFS_PACK=<files>/packs/romfs.nxbi, the reader is fs_pack.h): the RomFS's small files are read where
// they lie in their tar packs, never unpacked. hle_fs.c includes this file twice: the first time (before the mounts) for the files, the second (before the directory
// calls, where dirh_t is known) for the directories; hle_fs.c itself only calls in, one line a call, so that its own code stays as upstream's.
// Every lookup goes: the overlay (DM_ROMFS_OVERLAY), then the RomFS folder (its large files, and any file put there on its own, which wins over the pack's), then the
// pack, only for a path the first two do not have (ENOENT / ENOTDIR). A folder lists the RomFS folder's entries and then the pack's that the folder does not have.
// Android only (the ports that have packs); elsewhere every call is a stub that hands back to hle_fs.c. The part that needs no host code (a path's key in the pack, a
// folder's pack entries less the folder's own) is built on the computer too: tests/fs_pack_fixture.c with HLE_FS_PACK_TEST, from tests/test_fs_pack.py
#ifndef HLE_FS_PACK_FILES
#define HLE_FS_PACK_FILES

#if defined(__ANDROID__) || defined(HLE_FS_PACK_TEST)
#include "fs_pack.h"
enum { FSPK_WANT_DIRS = 1, FSPK_WANT_FILES = 2 };   // a listing's mode bits: Horizon's OpenDirectoryMode (hle_fs.c DIRMODE_DIR, DIRMODE_FILE)
#define FSPK_NAME_MAX 0x300                          // the longest entry name a DirectoryEntry holds (name[0x301]); Horizon makes none longer

// a path's key in the pack from what follows "<mount>:" once it is normalized (hle_fs.c norm_rest: "" or "/a/b"): "a/b" ("" the root). 0 when it does not fit in n
static int fspk_key(const char *normed, char *out, size_t n) {
    while (*normed == '/') normed++;
    size_t l = strlen(normed);
    if (l + 1 > n) return 0;
    memcpy(out, normed, l + 1);
    return 1;
}

// name a (NUL-terminated) against b (blen bytes, no NUL) in strcmp's order (unsigned bytes, a name that starts another first): <0, 0, >0
static int fspk_name_cmp(const char *a, const char *b, size_t blen) {
    for (size_t i = 0; i < blen; i++) {
        unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (!x) return -1;
        if (x != y) return x < y ? -1 : 1;
    }
    return a[blen] ? 1 : 0;
}

// whether name (len bytes) is in set: n records of stride bytes, each starting with a const char * to a NUL-terminated name, sorted by strcmp (hle_fs.c dir_index's name_ent)
static int fspk_in_set(const void *set, size_t n, size_t stride, const char *name, size_t len) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int r = fspk_name_cmp(*(const char *const *)((const char *)set + mid * stride), name, len);
        if (!r) return 1;
        if (r < 0) lo = mid + 1; else hi = mid;
    }
    return 0;
}

// The next entry of a folder's pack walk (pk_dir_begin) that the listing shows: of a kind mode asks for, not in set (the folder's own names: the RomFS folder's entry wins),
// and with a name a DirectoryEntry holds. 1 with it (name: len bytes in the index, not NUL-terminated), 0 at the walk's end. Each call moves the walk on
static int fspk_next(const pk_t *pk, pk_cur *cur, const void *set, size_t nset, size_t stride, int mode, const char **name, size_t *len, int *is_dir, uint64_t *size) {
    const char *n; size_t nl; int d; uint64_t sz;
    while (pk_dir_next(pk, cur, &n, &nl, &d, &sz)) {
        if (!(mode & (d ? FSPK_WANT_DIRS : FSPK_WANT_FILES)) || nl > FSPK_NAME_MAX) continue;
        if (nset && fspk_in_set(set, nset, stride, n, nl)) continue;
        *name = n; *len = nl; *is_dir = d; *size = d ? 0 : sz;
        return 1;
    }
    return 0;
}

// the number of entries fspk_next gives from walk start to its end (start is not moved): what the listing adds for the pack
static int64_t fspk_count(const pk_t *pk, const pk_cur *start, const void *set, size_t nset, size_t stride, int mode) {
    pk_cur c = *start; const char *n; size_t nl; int d; uint64_t sz; int64_t k = 0;
    while (fspk_next(pk, &c, set, nset, stride, mode, &n, &nl, &d, &sz)) k++;
    return k;
}
#endif

#if defined(__ANDROID__) && !defined(HLE_FS_PACK_TEST)
_Static_assert((int)FSPK_WANT_DIRS == DIRMODE_DIR && (int)FSPK_WANT_FILES == DIRMODE_FILE, "the pack listing's mode bits are OpenDirectoryMode's");
static pk_t *_Atomic g_pk;                  // the index (DM_ROMFS_PACK), loaded at the first rom mount; NULL without one: every call below hands back at once
static uint8_t g_pk_mnt[MAX_MOUNT];         // mount index → the pack is under it (a rom mount; PATH_LOCK)
typedef struct { uint64_t base, size; } fspk_file_t;
static fspk_file_t g_pkf[65536]; static uint8_t g_pkf_on[65536];   // fd → a pack's file: its bytes' start in the pack and their count (as g_fmode, set at open)
#define FSPK() atomic_load_explicit(&g_pk, memory_order_acquire)

static void fspk_mount(const char *name) {   // MountRom made mount name: the index loaded (once), and the pack put under the mount
    const char *idx = getenv("DM_ROMFS_PACK");
    if (!idx || !*idx) return;
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER; static int tried;
    pthread_mutex_lock(&mu);
    if (!tried) {
        tried = 1; char err[512];
        pk_t *pk = pk_load(idx, err, sizeof err);
        if (!pk) LOG("[RED] fs: the RomFS pack index was not loaded (%s): the RomFS's small files will not be found", err);
        else {
            LOG("fs: RomFS pack index %s: %u files in %u packs, read in place under the rom mount after the RomFS folder", idx, pk->nfiles, pk->npacks);
            const char *v = getenv("DM_ROMFS_PACK_VERIFY");
            if (v && atoi(v) > 0) {
                uint64_t t0 = now_ns(); char verr[512];
                int bad = pk_verify(pk, verr, sizeof verr);
                double s = (now_ns() - t0) / 1e9;
                if (bad) LOG("[RED] fs: RomFS pack check (DM_ROMFS_PACK_VERIFY) failed after %.2f s: %s", s, verr);
                else LOG("fs: RomFS pack check (DM_ROMFS_PACK_VERIFY): the tar header of each of the %u files is the index's (%.2f s)", pk->nfiles, s);
            }
            atomic_store_explicit(&g_pk, pk, memory_order_release);
        }
    }
    pthread_mutex_unlock(&mu);
    if (!FSPK()) return;
    PATH_LOCK();
    for (int i = 0; i < g_nmnt; i++) if (!strcmp(g_mnt[i].name, name)) {
        if (g_pk_mnt[i]) break;
        g_pk_mnt[i] = 1;
        struct stat st; int there = !stat(g_mnt[i].dir, &st);
        LOG("fs mount %s: the RomFS pack is under it%s", name, there ? "" : " (no RomFS folder: every file comes from the pack)");
        if (g_mnt[i].overlay[0]) LOG("[RED] fs mount %s: DM_ROMFS_OVERLAY with a RomFS pack: the pack's small files open, but are not listed under an overlay", name);
        break;
    }
}

static int fspk_mount_of(const char *path) {   // the mount path is on, when the pack is under it, else -1 (PATH_LOCK held)
    const char *c = strchr(path, ':');
    if (!c) return -1;
    size_t ln = (size_t)(c - path);
    for (int i = 0; i < g_nmnt; i++) if (strlen(g_mnt[i].name) == ln && !strncmp(g_mnt[i].name, path, ln)) return g_pk_mnt[i] ? i : -1;
    return -1;
}
static int fspk_path_key(const char *path, char *key, size_t n) {   // path's key in the pack: what follows its "<mount>:", normalized as host_path_ex normalizes it
    char normed[1024];
    return norm_rest(strchr(path, ':') + 1, normed, sizeof normed) && fspk_key(normed, key, n);
}
static int fspk_absent(int e) { return e == ENOENT || e == ENOTDIR; }

// OpenFile: a file the RomFS folder does not have and the pack does, opened on a dup of its pack's descriptor; the handle is that fd, with the tables OpenFile keeps for a
// file. 1 with the result in res, or 0: OpenFile goes on as before (no pack, another mount, the RomFS folder has the path, or neither has it)
static int fspk_open(uint64_t *out, const char *path, int mode, Result *res) {
    if (!FSPK() || !path) return 0;
    PATH_LOCK();
    if (fspk_mount_of(path) < 0) return 0;
    char hp[1024], key[1024]; int ro = 0; pk_ent e;
    if (host_path_ex(path, hp, sizeof hp, &ro, 0) || !fspk_absent(errno)) return 0;
    if (!fspk_path_key(path, key, sizeof key) || !pk_find(FSPK(), key, &e)) return 0;
    if (mode & (MODE_WRITE | MODE_APPEND)) { LOG("fs write to a read-only mount: %s (a file of the RomFS pack)", path); *res = RES_FAIL; return 1; }   // as RW() for the RomFS folder's
    int fd;
    if (pk_open(FSPK(), &e, &fd)) {
        int er = errno; static _Atomic int said;
        if (atomic_fetch_add(&said, 1) < 16) LOG("[RED] fs open %s: its RomFS pack p%04u.tar cannot be opened (%s)", path, e.pack, er == EIO ? "its size is not the index's" : strerror(er));
        errno = er; *res = err_res("pack open", path); return 1;
    }
    if (fd >= (int)sizeof g_fmode) die("fd beyond the table");
    g_pkf[fd] = (fspk_file_t){e.data_off, e.size}; g_pkf_on[fd] = 1;
    g_fmode[fd] = (uint8_t)mode; g_fsave[fd] = 0; *out = (uint64_t)fd;   // the rest as OpenFile does after its open
    if (zon()) zf_close(fd);   // never a packed (zstd) file: the port refuses romfs zstd with packs
    fslog_init();
    if (g_fslog && fd < 4096) { pthread_mutex_lock(&g_fslog_lock); free(g_fdpath[fd]); g_fdpath[fd] = strdup(path); pthread_mutex_unlock(&g_fslog_lock); }
    if (g_flag.fs) LOG("fs open [%.2fs] %s mode=%d → fd %d (RomFS pack %u at %#llx, %llu bytes)", (now_ns() - g_t0_ns) / 1e9, path, mode, fd, e.pack, (unsigned long long)e.data_off, (unsigned long long)e.size);
    *res = RES_OK; return 1;
}
// ReadFile: a pack file's read moved to its bytes in the pack and cut at its end (an offset at or past it reads nothing, as pread at a file's end); the base added to *off,
// which ReadFile takes off again after its loop (its logs show the file's offset). 0 for any other fd, and for a negative offset (pread refuses it, as for a file)
static int64_t fspk_read_prep(uint64_t h, int64_t *off, size_t *sz) {
    int k = (int)h & 0xffff;
    if (!g_pkf_on[k] || *off < 0) return 0;
    uint64_t o = (uint64_t)*off, size = g_pkf[k].size;
    if (o >= size) *sz = 0;
    else if (*sz > size - o) *sz = (size_t)(size - o);
    *off += (int64_t)g_pkf[k].base;
    return (int64_t)g_pkf[k].base;
}
static int fspk_size(uint64_t h, int64_t *out) {   // GetFileSize of a pack file: its size (fstat would give the pack's)
    int k = (int)h & 0xffff;
    if (!g_pkf_on[k]) return 0;
    *out = (int64_t)g_pkf[k].size; return 1;
}
static void fspk_fd_forget(int fd) { g_pkf_on[fd & 0xffff] = 0; }   // the fd closed, or opened again as a file of the RomFS folder

// GetEntryType: a path the RomFS folder does not have (its root included, when there is no RomFS folder), as the pack has it: a file, or a folder (a name starts with it)
static int fspk_entry_type(int32_t *out, const char *path, Result *res) {
    if (!FSPK() || !path) return 0;
    PATH_LOCK();
    if (fspk_mount_of(path) < 0) return 0;
    char hp[1024], key[1024]; int ro = 0; struct stat st;
    if (host_path_ex(path, hp, sizeof hp, &ro, 0)) { if (!stat(hp, &st) || !fspk_absent(errno)) return 0; }
    else if (!fspk_absent(errno)) return 0;
    if (!fspk_path_key(path, key, sizeof key)) return 0;
    if (pk_find(FSPK(), key, NULL)) *out = ENT_FILE;
    else if (pk_is_dir(FSPK(), key)) *out = ENT_DIR;
    else return 0;
    *res = RES_OK; return 1;
}
#elif !defined(HLE_FS_PACK_TEST)   // no packs: hle_fs.c goes on as before
static inline void fspk_mount(const char *name) { (void)name; }
static inline int fspk_open(uint64_t *out, const char *path, int mode, Result *res) { (void)out; (void)path; (void)mode; (void)res; return 0; }
static inline int64_t fspk_read_prep(uint64_t h, int64_t *off, size_t *sz) { (void)h; (void)off; (void)sz; return 0; }
static inline int fspk_size(uint64_t h, int64_t *out) { (void)h; (void)out; return 0; }
static inline void fspk_fd_forget(int fd) { (void)fd; }
static inline int fspk_entry_type(int32_t *out, const char *path, Result *res) { (void)out; (void)path; (void)res; return 0; }
#endif

#else   // the second include: directories

#if defined(__ANDROID__)
// A listing of a folder the pack has: a dirh_t whose ro is FSPK_DIRH, the first member of an fspk_dirh_t (ReadDirectory, DirCount and CloseDirectory hand it here). d is the
// RomFS folder's own (NULL when it has none: a folder only in the pack); its entries come first, as ReadDirectory lists them, then the pack's that the folder's names
// (its directory index: no stat per entry) do not have
enum { FSPK_DIRH = 2 };
typedef struct { dirh_t h; pk_cur start, cur; int real_done; const name_ent *set; size_t nset; } fspk_dirh_t;

static int fspk_opendir(uint64_t *out, const char *path, int mode, Result *res) {
    if (!FSPK() || !path) return 0;
    PATH_LOCK();
    int i = fspk_mount_of(path);
    if (i < 0 || g_mnt[i].overlay[0]) return 0;   // under an overlay the listing is dir_snapshot's, as before (fspk_mount said so)
    char key[1024], hp[1024]; int ro = 0; DIR *d = NULL;
    if (!fspk_path_key(path, key, sizeof key) || !pk_is_dir(FSPK(), key)) return 0;   // not a folder of the pack: as before
    if (host_path_ex(path, hp, sizeof hp, &ro, 0)) { if (!(d = opendir(hp)) && !fspk_absent(errno)) return 0; }
    else if (!fspk_absent(errno)) return 0;
    fspk_dirh_t *w = calloc(1, sizeof *w);
    if (!w) { if (d) closedir(d); errno = ENOMEM; *res = err_res("opendir", path); return 1; }
    w->h.d = d; strlcpy(w->h.path, d ? hp : "", sizeof w->h.path); w->h.mode = mode; w->h.ro = FSPK_DIRH;
    if (d) {
        const dir_index *x = dir_index_get(hp);
        if (x) { w->set = x->e; w->nset = x->n; }
        else { static _Atomic int said; if (!atomic_fetch_add(&said, 1)) LOG("[RED] fs opendir %s: the RomFS folder's names could not be indexed: an entry both it and the pack have is listed twice", path); }
    }
    pk_dir_begin(FSPK(), key, &w->start); w->cur = w->start;
    *out = (uint64_t)&w->h; *res = RES_OK; return 1;
}
static int fspk_readdir(dirh_t *h, int64_t *count, DirectoryEntry *ents, int64_t max, Result *res) {
    if (h->ro != FSPK_DIRH) return 0;
    fspk_dirh_t *w = (fspk_dirh_t *)h; int64_t n = 0;
    while (h->d && !w->real_done && n < max) {   // the RomFS folder's entries, as ReadDirectory lists them
        struct dirent *e = readdir(h->d);
        if (!e) { w->real_done = 1; break; }
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        struct stat st; int isdir;
        if (e->d_type == DT_DIR) isdir = 1;
        else { if (fstatat(dirfd(h->d), e->d_name, &st, 0)) continue; isdir = S_ISDIR(st.st_mode); zf_size(dirfd(h->d), e->d_name, &st); }
        if (!(h->mode & (isdir ? DIRMODE_DIR : DIRMODE_FILE))) continue;
        DirectoryEntry *o = &ents[n++]; memset(o, 0, sizeof *o);
        if (!guest_name(h->path, e->d_name, o->name, sizeof o->name)) { char c[1280]; snprintf(c, sizeof c, "%s/%s", h->path, e->d_name); *count = n - 1; *res = err_res("directory name", c); return 1; }
        o->type = isdir ? ENT_DIR : ENT_FILE; o->size = isdir ? 0 : st.st_size;
    }
    const char *nm; size_t nl; int isdir; uint64_t sz;
    while (n < max && fspk_next(FSPK(), &w->cur, w->set, w->nset, sizeof *w->set, h->mode, &nm, &nl, &isdir, &sz)) {   // then the pack's
        DirectoryEntry *o = &ents[n++]; memset(o, 0, sizeof *o);
        memcpy(o->name, nm, nl); o->type = isdir ? ENT_DIR : ENT_FILE; o->size = (int64_t)sz;
    }
    *count = n; *res = RES_OK; return 1;
}
static int fspk_dircount(dirh_t *h, int64_t *out, Result *res) {   // as many as the listing gives from its start: the RomFS folder's (counted as DirCount counts) and the pack's
    if (!h || h->ro != FSPK_DIRH) return 0;
    fspk_dirh_t *w = (fspk_dirh_t *)h; int64_t n = 0;
    if (h->d) {
        DIR *d = opendir(h->path);
        if (!d) { *res = err_res("opendir", h->path); return 1; }
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char c[1280]; snprintf(c, sizeof c, "%s/%s", h->path, e->d_name);
            struct stat st; if (stat(c, &st)) continue;
            if (h->mode & (S_ISDIR(st.st_mode) ? DIRMODE_DIR : DIRMODE_FILE)) n++;
        }
        closedir(d);
    }
    *out = n + fspk_count(FSPK(), &w->start, w->set, w->nset, sizeof *w->set, h->mode); *res = RES_OK; return 1;
}
static int fspk_closedir(uint64_t hh) {
    dirh_t *h = (dirh_t *)hh;
    if (h->ro != FSPK_DIRH) return 0;
    if (h->d) closedir(h->d);
    free((fspk_dirh_t *)h); return 1;
}
#else
static inline int fspk_opendir(uint64_t *out, const char *path, int mode, Result *res) { (void)out; (void)path; (void)mode; (void)res; return 0; }
static inline int fspk_readdir(dirh_t *h, int64_t *count, DirectoryEntry *ents, int64_t max, Result *res) { (void)h; (void)count; (void)ents; (void)max; (void)res; return 0; }
static inline int fspk_dircount(dirh_t *h, int64_t *out, Result *res) { (void)h; (void)out; (void)res; return 0; }
static inline int fspk_closedir(uint64_t hh) { (void)hh; return 0; }
#endif

#endif
