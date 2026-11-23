// env_path.h: an env.txt value that is a path into another app's files folder, or into the app's own one on another volume, read as the data's folder (front_android.c env_file).
// A data pack's env.txt names some of its own files by absolute path (DM_SHARED_FONT, DM_AOC_DIR, DM_ROMFS_OVERLAY: /sdcard/Android/data/<package>/files/...), with the
// package it was ported with. An APK renamed by porter apk-update --package reads that pack as it is on the server (the old app reads it too): its files are in its own
// folder, so a value whose path starts with /<anything>/Android/data/<another package>/files (then the end or a /) is the app's files folder (own_root) and the rest.
// The data's folder (own_root) is the app's files folder on the volume the player chose (storage_android.c: internal storage or an SD card), and the pack's paths name
// internal storage's: a value in the app's own internal files folder is own_root's too when own_root is on another volume. A value in the app's own folder on an SD card
// stays (porter install --romfs-at sd: the RomFS there while the rest is in internal storage), unless that folder is gone and own_root has the file (dm_env_moved_path:
// the data moved from the card to internal storage).
// tests/test_env_path.py builds it on the computer.
#ifndef DM_ENV_PATH_H
#define DM_ENV_PATH_H
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

// the package of a path /<...>/Android/data/<package>/files[/...]: its start and length in *pkg, *pn, and where the rest after "files" begins (*rest); 0 when the path is
// not one (not absolute, no such part, an empty package)
static int dm_env_files_path(const char *p, const char **pkg, size_t *pn, const char **rest) {
    static const char mid[] = "/Android/data/";
    if (!p || p[0] != '/') return 0;
    const char *m = strstr(p, mid); if (!m) return 0;
    const char *k = m + sizeof mid - 1, *e = strchr(k, '/'); if (!e || e == k) return 0;
    if (strncmp(e, "/files", 6) || (e[6] && e[6] != '/')) return 0;
    *pkg = k; *pn = (size_t)(e - k); *rest = e + 6; return 1;
}

// whether the part of a path before /Android/data/ (p .. m) is internal storage, as Android or a pack spells it: /sdcard, /storage/emulated/<n>, /storage/self/primary,
// /mnt/sdcard, /data/media/<n>
static int dm_env_primary(const char *p, const char *m) {
    size_t n = (size_t)(m - p);
    static const char *const fixed[] = {"/sdcard", "/storage/self/primary", "/mnt/sdcard", NULL};
    for (int i = 0; fixed[i]; i++) if (n == strlen(fixed[i]) && !strncmp(p, fixed[i], n)) return 1;
    static const char *const num[] = {"/storage/emulated/", "/data/media/", NULL};
    for (int i = 0; num[i]; i++) { size_t k = strlen(num[i]); if (n > k && !strncmp(p, num[i], k)) { size_t j = k; while (j < n && p[j] >= '0' && p[j] <= '9') j++; if (j == n) return 1; } }
    return 0;
}
static const char *dm_env_mid(const char *p) { return strstr(p, "/Android/data/"); }

// 1 with out = own_root + the rest when value is a path into another package's files folder than own_root's, or into the app's own internal files folder while own_root
// is on another volume (own_root: the data's folder, /storage/emulated/0/Android/data/<package>/files or /storage/<card>/Android/data/<package>/files); 0 (out untouched)
// for any other value, the data's own folder's path among them, or one that does not fit in cap
static int dm_env_own_path(const char *value, const char *own_root, char *out, size_t cap) {
    const char *vp, *vr, *op, *orr; size_t vn, on;
    if (!dm_env_files_path(value, &vp, &vn, &vr) || !dm_env_files_path(own_root, &op, &on, &orr) || orr[0]) return 0;
    if (vn == on && !strncmp(vp, op, vn)) {   // the app's own package: rewritten only from internal storage to the card the data is on
        if (!dm_env_primary(value, dm_env_mid(value)) || dm_env_primary(own_root, dm_env_mid(own_root))) return 0;
    }
    int n = snprintf(out, cap, "%s%s", own_root, vr);
    return n > 0 && (size_t)n < cap;
}
// 1 with out = own_root + the rest when value is in the app's own files folder on another volume than own_root's, that path is not there and own_root's is (the data
// moved from a card back to internal storage, and the row named the card: porter install --romfs-at sd); 0 otherwise
static int dm_env_moved_path(const char *value, const char *own_root, char *out, size_t cap) {
    const char *vp, *vr, *op, *orr; size_t vn, on; struct stat st;
    if (!dm_env_files_path(value, &vp, &vn, &vr) || !dm_env_files_path(own_root, &op, &on, &orr) || orr[0]) return 0;
    if (vn != on || strncmp(vp, op, vn)) return 0;   // another package: dm_env_own_path's
    if ((size_t)(vr - value) == strlen(own_root) && !strncmp(value, own_root, strlen(own_root))) return 0;   // the data's own folder
    if (!stat(value, &st)) return 0;
    char t[1200]; int n = snprintf(t, sizeof t, "%s%s", own_root, vr);
    if (n <= 0 || (size_t)n >= sizeof t || (size_t)n >= cap || stat(t, &st)) return 0;
    memcpy(out, t, (size_t)n + 1); return 1;
}
#endif
