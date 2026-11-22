// env_path.h: an env.txt value that is a path into another app's files folder, read as the app's own (front_android.c env_file).
// A data pack's env.txt names some of its own files by absolute path (DM_SHARED_FONT, DM_AOC_DIR, DM_ROMFS_OVERLAY: /sdcard/Android/data/<package>/files/...), with the
// package it was ported with. An APK renamed by porter apk-update --package reads that pack as it is on the server (the old app reads it too): its files are in its own
// folder, so a value whose path starts with /<anything>/Android/data/<another package>/files (then the end or a /) is the app's files folder (own_root) and the rest.
// tests/test_env_path.py builds it on the computer.
#ifndef DM_ENV_PATH_H
#define DM_ENV_PATH_H
#include <stdio.h>
#include <string.h>

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

// 1 with out = own_root + the rest when value is a path into another package's files folder than own_root's (own_root: the app's files folder,
// /storage/emulated/0/Android/data/<package>/files); 0 (out untouched) for any other value, the app's own folder's path among them, or one that does not fit in cap
static int dm_env_own_path(const char *value, const char *own_root, char *out, size_t cap) {
    const char *vp, *vr, *op, *orr; size_t vn, on;
    if (!dm_env_files_path(value, &vp, &vn, &vr) || !dm_env_files_path(own_root, &op, &on, &orr) || orr[0]) return 0;
    if (vn == on && !strncmp(vp, op, vn)) return 0;   // the app's own
    int n = snprintf(out, cap, "%s%s", own_root, vr);
    return n > 0 && (size_t)n < cap;
}
#endif
