// storage.h: where the app keeps its game data (the data root), as pure decisions and POSIX file work the computer tests (tests/test_storage.py, tests/storage_fixture.c).
// Android gives an app a files folder of its own on every shared volume (Context.getExternalFilesDirs: the first is internal storage, /storage/emulated/0/Android/data/
// <package>/files; the others are on SD cards or USB drives, /storage/<volume id>/Android/data/<package>/files), none of them needing a permission. The player chooses
// which one holds the game data (the launcher's App settings, storage_android.c); the choice is a file in the app's internal storage (DM_ST_FILE: root=<folder>), so it
// survives a card taken out. What stays in the internal files folder whatever the choice (dm_st_kept): the log, the save manager's folder (saves/: exports, the cloud's
// state) and the cloud link (sync.txt), and the pointer porter reads to push data where the app reads it (DM_ST_POINTER). The save itself is in internal storage
// (DM_SAVE), never on a card. Moving the data: a marker in internal storage (DM_ST_MOVE: from=, to=) is written first, the data copied (sizes and times kept) and checked,
// the choice written (the move's commit), and the old copy deleted; a move cut short is undone or finished at the next start from the marker (dm_st_recover).
#ifndef DM_STORAGE_H
#define DM_STORAGE_H
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <unistd.h>

#define DM_ST_FILE "nx2a_storage.txt"         // internal storage: root=<the data root>
#define DM_ST_MOVE "nx2a_storage_move.txt"    // internal storage: a move under way (from=, to=)
#define DM_ST_POINTER "nx2a_root.txt"         // the internal files folder: root=<the data root> while it is not that folder (porter install, porter driver read it)
#define DM_ST_MARGIN (64ull << 20)            // room a move leaves on the card beyond the data
#define DM_ST_FILE_SLACK 4096ull              // and for each file (the file system's rounding)
#define DM_ST_NVOL 6
typedef struct { char path[512]; int removable, mounted; uint64_t free, total; } dm_vol;   // a volume's files folder of the app (path), its free bytes

// the names in the internal files folder that never move (the log is open while the app runs; the save's folder and the cloud link stay where an SD card cannot take them;
// devtools and porter read the log and write ctl.txt there)
static int dm_st_kept(const char *name) {
    static const char *const k[] = {"log.txt", "log.prev.txt", "saves", "sync.txt", "ctl.txt", DM_ST_POINTER, NULL};   // (ctl.txt: the development tools' key presses)
    for (int i = 0; k[i]; i++) if (!strcmp(name, k[i])) return 1;
    return 0;
}
// "key=value" lines: the value of key (trailing spaces and line ends cut) into out; 0 when there is none (or it does not fit)
static int dm_st_value(const char *txt, const char *key, char *out, size_t cap) {
    size_t kn = strlen(key);
    for (const char *p = txt; p && *p; ) {
        const char *e = strchr(p, '\n'); size_t n = e ? (size_t)(e - p) : strlen(p);
        while (n && (p[n - 1] == '\r' || p[n - 1] == ' ')) n--;
        if (n > kn && !strncmp(p, key, kn) && p[kn] == '=') { n -= kn + 1; if (n >= cap) return 0; memcpy(out, p + kn + 1, n); out[n] = 0; return 1; }
        p = e ? e + 1 : NULL;
    }
    return 0;
}
static char *dm_st_read(const char *path, size_t max) {   // a small text file whole (NUL-terminated), NULL when it cannot be read
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    char *b = malloc(max + 1); size_t n = b ? fread(b, 1, max, f) : 0; fclose(f);
    if (b) b[n] = 0;
    return b;
}
static int dm_st_write(const char *path, const char *text) {   // through <path>.new, fsync'd, renamed: 0 or -errno
    char t[1100]; if ((size_t)snprintf(t, sizeof t, "%s.new", path) >= sizeof t) return -ENAMETOOLONG;
    int fd = open(t, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0660); if (fd < 0) return -errno;
    size_t n = strlen(text); int r = write(fd, text, n) == (ssize_t)n ? 0 : -EIO;
    if (!r && fsync(fd)) r = -errno;
    if (close(fd) && !r) r = -errno;
    if (!r && rename(t, path)) r = -errno;
    if (r) unlink(t);
    return r;
}
static int dm_st_is_dir(const char *p) { struct stat st; return p && p[0] && !stat(p, &st) && S_ISDIR(st.st_mode); }
static int dm_st_exists(const char *dir, const char *name) { char p[1100]; struct stat st; snprintf(p, sizeof p, "%s/%s", dir, name); return !stat(p, &st); }
// a folder that holds game data: a pack (its manifest, the download's list), or the parts a pack is made of
static int dm_st_has_data(const char *dir) {
    static const char *const n[] = {"nx2a-data.json", "files.json", "mods", "RomFS", "bundles", "shaders.dmsv", NULL};
    for (int i = 0; n[i]; i++) if (dm_st_exists(dir, n[i])) return 1;
    return 0;
}
static uint64_t dm_st_free(const char *dir) { struct statvfs v; return statvfs(dir, &v) ? 0 : (uint64_t)v.f_bavail * v.f_frsize; }

// ---- the choice at the start ----
// v[0..n-1]: the app's files folders (v[0] internal storage), with whether each is mounted; saved: the choice kept ("" none); has_data[i]: v[i] holds game data;
// primary_pack: the internal files folder holds a whole pack's manifest (porter install pushes it last). vol: the data root's index; missing: the saved root is not
// mounted (an SD card taken out: the start asks, dm_st_choose gives vol 0 only for the log's sake); save: the choice is to be written; adopt: an install put a pack into
// internal storage while the data root was on a card (an older porter, or a push by hand): internal storage is used, since that pack is the newest
typedef struct { int vol, missing, save, adopt; } dm_st_pick;
static dm_st_pick dm_st_choose(const dm_vol *v, int n, const char *saved, const int *has_data, int primary_pack) {
    dm_st_pick p = {0, 0, 0, 0};
    if (n <= 0) return p;
    if (saved && saved[0]) {
        for (int i = 0; i < n; i++) if (!strcmp(v[i].path, saved) && v[i].mounted) {
            if (i && primary_pack) { p.save = p.adopt = 1; return p; }
            p.vol = i; return p;
        }
        p.missing = 1; return p;
    }
    p.save = 1;   // no choice yet: data already in a folder keeps it there; a first start takes the volume with the most room
    if (has_data[0]) return p;
    for (int i = 1; i < n; i++) if (v[i].mounted && has_data[i]) { p.vol = i; return p; }
    for (int i = 1; i < n; i++) if (v[i].mounted && v[i].free > v[p.vol].free) p.vol = i;
    return p;
}

// ---- moving the data root (a thread of the launcher's: storage_android.c) ----
typedef struct {
    uint64_t done, total; long files;
    volatile int *cancel;                              // set: the copy stops at its next block (-ECANCELED)
    void (*note)(void *u, uint64_t done, uint64_t total); void *u;   // progress, at most every 4 MB
    uint64_t noted; char err[300];                     // the first failure's path
} dm_mv;
static void dm_st_rm_tree(const char *p) {
    struct stat st; if (lstat(p, &st)) return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(p); struct dirent *e;
        while (d && (e = readdir(d))) { if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue; char c[1200]; snprintf(c, sizeof c, "%s/%s", p, e->d_name); dm_st_rm_tree(c); }
        if (d) closedir(d);
        rmdir(p);
    } else unlink(p);
}
// the bytes and files of a tree; top: dir is a files folder, whose kept names do not count (they do not move)
static void dm_st_size(const char *dir, int top, uint64_t *bytes, long *files) {
    DIR *d = opendir(dir); struct dirent *e; if (!d) return;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || (top && dm_st_kept(e->d_name))) continue;
        char p[1200]; struct stat st; snprintf(p, sizeof p, "%s/%s", dir, e->d_name); if (lstat(p, &st)) continue;
        if (S_ISDIR(st.st_mode)) dm_st_size(p, 0, bytes, files); else if (S_ISREG(st.st_mode)) { *bytes += (uint64_t)st.st_size; (*files)++; }
    }
    closedir(d);
}
static int dm_st_copy_file(const char *src, const char *dst, const struct stat *ss, dm_mv *m) {   // 0 or -errno; its times kept (a list's checks go by size and time)
    int in = open(src, O_RDONLY | O_CLOEXEC); if (in < 0) { snprintf(m->err, sizeof m->err, "%s", src); return -errno; }
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666); if (out < 0) { int e = errno; close(in); snprintf(m->err, sizeof m->err, "%s", dst); return -e; }
    static __thread char *buf; if (!buf) buf = malloc(1 << 20);
    int r = buf ? 0 : -ENOMEM; ssize_t n = 0;
    while (!r && (n = read(in, buf, 1 << 20)) > 0) {
        for (ssize_t o = 0; o < n && !r;) { ssize_t w = write(out, buf + o, (size_t)(n - o)); if (w < 0) { r = -errno; snprintf(m->err, sizeof m->err, "%s", dst); } else o += w; }
        m->done += (uint64_t)n;
        if (m->note && m->done - m->noted >= (4u << 20)) { m->noted = m->done; m->note(m->u, m->done, m->total); }
        if (m->cancel && *m->cancel) r = -ECANCELED;
    }
    if (!r && n < 0) { r = -errno; snprintf(m->err, sizeof m->err, "%s", src); }
#ifdef __APPLE__
    if (!r) { struct timespec t[2] = {ss->st_atimespec, ss->st_mtimespec}; futimens(out, t); }   // (the computer's test)
#else
    if (!r) { struct timespec t[2] = {ss->st_atim, ss->st_mtim}; futimens(out, t); }
#endif
    close(in); if (close(out) && !r) { r = -errno; snprintf(m->err, sizeof m->err, "%s", dst); }
    return r;
}
// src's tree into dst. fix 0: every file copied; 1: only those whose size in dst is not src's (the check after the copy: a file the app wrote meanwhile, the
// shader cache); *bad counts them. top: src is a files folder (its kept names stay)
static int dm_st_copy_tree(const char *src, const char *dst, int top, int fix, long *bad, dm_mv *m) {
    if (mkdir(dst, 0775) && errno != EEXIST) { snprintf(m->err, sizeof m->err, "%s", dst); return -errno; }
    DIR *d = opendir(src); if (!d) { snprintf(m->err, sizeof m->err, "%s", src); return -errno; }
    struct dirent *e; int r = 0;
    while (!r && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || (top && dm_st_kept(e->d_name))) continue;
        if (top && !strcmp(e->d_name, DM_ST_POINTER)) continue;
        char s[1200], t[1200]; struct stat ss, ts; snprintf(s, sizeof s, "%s/%s", src, e->d_name); snprintf(t, sizeof t, "%s/%s", dst, e->d_name);
        if (lstat(s, &ss)) continue;   // (gone meanwhile)
        if (S_ISDIR(ss.st_mode)) r = dm_st_copy_tree(s, t, 0, fix, bad, m);
        else if (S_ISREG(ss.st_mode)) {
            if (fix && !stat(t, &ts) && S_ISREG(ts.st_mode) && ts.st_size == ss.st_size) continue;
            if (fix && bad) (*bad)++;
            r = dm_st_copy_file(s, t, &ss, m);
        }
        if (!r && m->cancel && *m->cancel) r = -ECANCELED;
    }
    closedir(d);
    return r;
}
// the moving names: a files folder's top-level entries but the kept ones, removed from dir (the old copy once the new one is the root, or a copy cut short)
static void dm_st_remove_moving(const char *dir, const char *names_from) {
    DIR *d = opendir(names_from); struct dirent *e; if (!d) return;
    char (*n)[256] = NULL; int k = 0, cap = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || dm_st_kept(e->d_name)) continue;
        if (k == cap) { cap = cap ? cap * 2 : 64; void *g = realloc(n, (size_t)cap * sizeof *n); if (!g) break; n = g; }
        snprintf(n[k++], sizeof *n, "%s", e->d_name);
    }
    closedir(d);
    for (int i = 0; i < k; i++) { char p[1200]; snprintf(p, sizeof p, "%s/%s", dir, n[i]); dm_st_rm_tree(p); }
    free(n);
}
// The whole move's copy: what dst has of src's names first removed (a copy cut short before), everything copied, then checked by size until it holds (at most 3
// rounds: the app's own files that changed meanwhile copied again). 0, or -errno (-ECANCELED: cancelled; -EAGAIN: files kept changing), with what was copied removed
static int dm_st_move_copy(const char *src, const char *dst, dm_mv *m) {
    dm_st_remove_moving(dst, src);
    long bad = 0; int r = dm_st_copy_tree(src, dst, 1, 0, NULL, m);
    for (int round = 0; !r && round < 3; round++) { bad = 0; r = dm_st_copy_tree(src, dst, 1, 1, &bad, m); if (!r && !bad) break; }
    if (!r && bad) r = -EAGAIN;
    if (!r) sync();
    if (r) dm_st_remove_moving(dst, src);
    return r;
}
// The start, before the root is chosen: a move the app did not finish (DM_ST_MOVE in internal storage). The choice already names its target: the old copy is deleted
// (what was left of it, and what the app wrote there after the commit); else the move did not commit: its partial copy is removed and the data stays where it was.
// Returns 1 when there was a marker
static int dm_st_recover(const char *internal, const char *saved_root) {
    char mp[700], from[600] = "", to[600] = ""; snprintf(mp, sizeof mp, "%s/%s", internal, DM_ST_MOVE);
    char *t = dm_st_read(mp, 4096); if (!t) return 0;
    int ok = dm_st_value(t, "from", from, sizeof from) && dm_st_value(t, "to", to, sizeof to); free(t);
    if (!ok) { unlink(mp); return 1; }
    int done = saved_root && !strcmp(saved_root, to), keep = 0;   // keep: the folder to clean is on a card that is not in now: the next start does it
    if (done) { if (dm_st_is_dir(from)) dm_st_remove_moving(from, from); else keep = 1; }
    else if (dm_st_is_dir(to) && dm_st_is_dir(from)) dm_st_remove_moving(to, from);
    else keep = dm_st_is_dir(from) || dm_st_is_dir(to);   // (the source gone too: nothing tells which names were copied; the marker waits for it)
    if (!keep) unlink(mp);
    return 1;
}
#endif
