// Android port: the data root on internal storage or an SD card (storage.h holds the decisions and the file work, tests/test_storage.py runs them on the computer).
// The volumes are the app's files folders Android gives it on each one (Context.getExternalFilesDirs through JNI: framework classes only, an APK without classes.dex
// has them too): the first is internal storage, the others an SD card or a USB drive, with Environment.isExternalStorageRemovable and getExternalStorageState. None
// needs a permission, and Android deletes them with the app.
// The choice is DM_ST_FILE in the app's internal storage; porter reads DM_ST_POINTER in the internal files folder (porter/datapack.py device_files) to push a pack where
// the app reads it. A move runs on a thread of its own from the launcher's App settings (datasync_android.c): the data counted, the target's room checked, the copy
// (cancellable), its check, the choice written, the old copy deleted. The app then ends (the game's Vulkan side opened files under the old root at the shader preparation,
// so the next start is a new process): the next start finds the marker and deletes what the app wrote under the old root meanwhile.
#include "dm.h"
#include "storage_android.h"
#include <jni.h>
#include <pthread.h>
#include <time.h>

static struct {
    pthread_mutex_t mu;
    dm_vol v[DM_ST_NVOL]; int n, cur;                 // the volumes (v[0] internal storage), the data root's
    char internal[512], saved[512];                   // the app's internal storage; the choice kept ("" none)
    dm_storage_move mv; volatile int cancel; int to;  // the move (under mu), its cancel, its target volume
} T = {PTHREAD_MUTEX_INITIALIZER};

static int jx(JNIEnv *E) { if (!(*E)->ExceptionCheck(E)) return 0; (*E)->ExceptionClear(E); return 1; }
static int jstr(JNIEnv *E, jstring s, char *out, size_t cap) {
    const char *u = s ? (*E)->GetStringUTFChars(E, s, NULL) : NULL; if (!u) { jx(E); return 0; }
    int ok = strlen(u) < cap; if (ok) snprintf(out, cap, "%s", u);
    (*E)->ReleaseStringUTFChars(E, s, u); return ok;
}
// the app's files folders, v[0] internal storage (act->externalDataPath when the call fails); a volume that is there but not available is an empty slot (null)
static int list_volumes(ANativeActivity *act, dm_vol *v, int cap) {
    JNIEnv *E = act->env; int n = 0;
    memset(v, 0, sizeof *v * (size_t)cap);
    if ((*E)->PushLocalFrame(E, 64) == 0) {
        jclass cA = (*E)->GetObjectClass(E, act->clazz);
        jmethodID mDirs = cA ? (*E)->GetMethodID(E, cA, "getExternalFilesDirs", "(Ljava/lang/String;)[Ljava/io/File;") : NULL;
        jobjectArray a = mDirs && !jx(E) ? (*E)->CallObjectMethod(E, act->clazz, mDirs, NULL) : NULL;
        jclass cF = a && !jx(E) ? (*E)->FindClass(E, "java/io/File") : NULL, cEv = cF && !jx(E) ? (*E)->FindClass(E, "android/os/Environment") : NULL;
        jmethodID mPath = cEv && !jx(E) ? (*E)->GetMethodID(E, cF, "getAbsolutePath", "()Ljava/lang/String;") : NULL;
        jmethodID mRem = mPath && !jx(E) ? (*E)->GetStaticMethodID(E, cEv, "isExternalStorageRemovable", "(Ljava/io/File;)Z") : NULL;
        jmethodID mState = mRem && !jx(E) ? (*E)->GetStaticMethodID(E, cEv, "getExternalStorageState", "(Ljava/io/File;)Ljava/lang/String;") : NULL;
        int len = mState && !jx(E) ? (*E)->GetArrayLength(E, a) : 0;
        for (int i = 0; i < len && n < cap; i++) {
            jobject f = (*E)->GetObjectArrayElement(E, a, i); if (jx(E)) break;
            if (!f) { if (!i) break; continue; }   // (internal storage not available: the activity's own path below)
            dm_vol *o = &v[n];
            if (!jstr(E, (*E)->CallObjectMethod(E, f, mPath), o->path, sizeof o->path)) { if (!i) break; continue; }
            jboolean rem = (*E)->CallStaticBooleanMethod(E, cEv, mRem, f); o->removable = !jx(E) && rem;
            char st[32] = ""; jstring s = (*E)->CallStaticObjectMethod(E, cEv, mState, f); if (!jx(E)) jstr(E, s, st, sizeof st);
            o->mounted = !strcmp(st, "mounted") && dm_st_is_dir(o->path);
            n++;
        }
        jx(E); (*E)->PopLocalFrame(E, NULL);
    } else jx(E);
    if (!n || (act->externalDataPath && strcmp(v[0].path, act->externalDataPath))) {   // the call failed: internal storage as the activity names it, alone
        if (n) LOG("[RED] storage: Android's first files folder %s is not the activity's %s: only the activity's is used", v[0].path, act->externalDataPath);
        memset(v, 0, sizeof *v * (size_t)cap); n = 0;
        if (act->externalDataPath) { snprintf(v[0].path, sizeof v[0].path, "%s", act->externalDataPath); v[0].mounted = 1; n = 1; }
    }
    for (int i = 0; i < n; i++) { struct statvfs s; if (v[i].mounted && !statvfs(v[i].path, &s)) { v[i].free = (uint64_t)s.f_bavail * s.f_frsize; v[i].total = (uint64_t)s.f_blocks * s.f_frsize; } }
    return n;
}
static void pointer_write(void) {   // porter's pointer in the internal files folder: the root while it is not that folder
    char p[700]; snprintf(p, sizeof p, "%s/%s", T.v[0].path, DM_ST_POINTER);
    if (T.cur > 0) { char t[700]; snprintf(t, sizeof t, "root=%s\n", T.v[T.cur].path); int r = dm_st_write(p, t); if (r) LOG("[RED] storage: cannot write %s (errno %d)", p, -r); }
    else if (unlink(p) && errno != ENOENT) LOG("[RED] storage: cannot remove %s (errno %d)", p, errno);
}
static int choice_write(const char *root) {
    char p[700], t[700]; snprintf(p, sizeof p, "%s/%s", T.internal, DM_ST_FILE); snprintf(t, sizeof t, "root=%s\n", root);
    int r = dm_st_write(p, t); if (r) LOG("[RED] storage: cannot write %s (errno %d)", p, -r);
    else snprintf(T.saved, sizeof T.saved, "%s", root);
    return r;
}
static const char *kind(int i) { return i <= 0 ? "internal storage" : T.v[i].removable ? "the SD card" : "a shared volume"; }
// the volumes listed (again), the saved root among them when it is a folder the listing did not give (a failed call), the root chosen: 1 when the saved one is not in
static int choose(ANativeActivity *act, int log) {
    T.n = list_volumes(act, T.v, DM_ST_NVOL);
    if (T.saved[0] && T.n < DM_ST_NVOL && dm_st_is_dir(T.saved)) {
        int in = 0; for (int i = 0; i < T.n; i++) in |= !strcmp(T.v[i].path, T.saved);
        if (!in) { dm_vol *o = &T.v[T.n++]; snprintf(o->path, sizeof o->path, "%s", T.saved); o->removable = o->mounted = 1; o->free = dm_st_free(o->path); }
    }
    int has[DM_ST_NVOL] = {0}; for (int i = 0; i < T.n; i++) has[i] = T.v[i].mounted && dm_st_has_data(T.v[i].path);
    dm_st_pick k = dm_st_choose(T.v, T.n, T.saved, has, T.n && dm_st_exists(T.v[0].path, "nx2a-data.json"));
    if (log) for (int i = 0; i < T.n; i++)
        LOG("storage: volume %d %s (%s, %s%s), %.1f GB free of %.1f GB", i, T.v[i].path, i ? (T.v[i].removable ? "removable" : "not removable") : "internal storage",
            T.v[i].mounted ? "mounted" : "not mounted", has[i] ? ", has game data" : "", T.v[i].free / 1073741824.0, T.v[i].total / 1073741824.0);
    if (k.missing) { LOG("[RED] storage: the game data is on %s, which is not in the device now: the start asks to insert it or to use internal storage", T.saved); T.cur = 0; return 1; }
    T.cur = k.vol;
    if (k.adopt) LOG("[RED] storage: the game data was on %s, but an install put a whole pack into internal storage since: internal storage is used (%s is left as it is)", T.saved, T.saved);
    else if (k.save) LOG("storage: no choice yet: the game data goes on %s (%s)", kind(T.cur), has[T.cur] ? "it has data there" : "the most room");
    if (k.save) choice_write(T.v[T.cur].path);
    pointer_write();
    LOG("storage: the game data is on %s: %s", kind(T.cur), T.v[T.cur].path);
    return 0;
}
int dm_storage_start(ANativeActivity *act, char *root, size_t cap) {
    pthread_mutex_lock(&T.mu);
    snprintf(T.internal, sizeof T.internal, "%s", act->internalDataPath ? act->internalDataPath : "");
    char p[700]; snprintf(p, sizeof p, "%s/%s", T.internal, DM_ST_FILE); char *t = T.internal[0] ? dm_st_read(p, 4096) : NULL;
    T.saved[0] = 0; if (t && !dm_st_value(t, "root", T.saved, sizeof T.saved)) T.saved[0] = 0; free(t);
    if (T.internal[0] && dm_st_recover(T.internal, T.saved)) LOG("storage: a move of the game data that did not finish was %s", T.saved[0] ? "finished or undone" : "undone");
    int missing = choose(act, 1);
    snprintf(root, cap, "%s", T.v[T.cur].path);
    pthread_mutex_unlock(&T.mu);
    return missing;
}
int dm_storage_recheck(ANativeActivity *act, char *root, size_t cap) {
    pthread_mutex_lock(&T.mu); int missing = choose(act, 1); snprintf(root, cap, "%s", T.v[T.cur].path); pthread_mutex_unlock(&T.mu);
    return missing;
}
void dm_storage_use_internal(char *root, size_t cap) {
    pthread_mutex_lock(&T.mu); T.cur = 0; choice_write(T.v[0].path); pointer_write(); snprintf(root, cap, "%s", T.v[0].path); pthread_mutex_unlock(&T.mu);
    LOG("storage: the player chose internal storage for the game data (the card that held it is not in): it is downloaded or installed there again");
}
const char *dm_storage_home(void) { return T.v[0].path; }   // (set once in onCreate, before any other thread)
static int sd_index_locked(void) {   // the SD card's volume: the first mounted removable one, else any other mounted one
    for (int i = 1; i < T.n; i++) if (T.v[i].mounted && T.v[i].removable) return i;
    for (int i = 1; i < T.n; i++) if (T.v[i].mounted) return i;
    return -1;
}
void dm_storage_info_get(dm_storage_info *o) {
    memset(o, 0, sizeof *o);
    pthread_mutex_lock(&T.mu);
    int sd = T.cur > 0 ? T.cur : sd_index_locked();
    o->on_sd = T.cur > 0; o->has_sd = sd > 0 && dm_st_is_dir(T.v[sd].path);
    if (T.n) o->free_int = dm_st_free(T.v[0].path);
    if (o->has_sd) o->free_sd = dm_st_free(T.v[sd].path);
    pthread_mutex_unlock(&T.mu);
}

// ---- the move ----
static void mv_note(void *u, uint64_t done, uint64_t total) { (void)u; pthread_mutex_lock(&T.mu); T.mv.done = done; T.mv.total = total; pthread_mutex_unlock(&T.mu); }
static void mv_end(int state, int err, const char *why) {
    pthread_mutex_lock(&T.mu); T.mv.state = state; T.mv.err = err; snprintf(T.mv.why, sizeof T.mv.why, "%s", why ? why : ""); pthread_mutex_unlock(&T.mu);
}
static void *move_thread(void *a) {
    (void)a; pthread_setname_np(pthread_self(), "datamove");
    pthread_mutex_lock(&T.mu); char from[512], to[512]; int ti = T.to; snprintf(from, sizeof from, "%s", T.v[T.cur].path); snprintf(to, sizeof to, "%s", T.v[ti].path); pthread_mutex_unlock(&T.mu);
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    uint64_t bytes = 0; long files = 0; dm_st_size(from, 1, &bytes, &files);
    uint64_t need = bytes + (uint64_t)files * DM_ST_FILE_SLACK + DM_ST_MARGIN, room = dm_st_free(to);
    pthread_mutex_lock(&T.mu); T.mv.sizing = 0; T.mv.total = bytes; T.mv.need = need; T.mv.room = room; pthread_mutex_unlock(&T.mu);
    LOG("storage: moving the game data from %s to %s: %ld files, %.2f GB (%.2f GB free there)", from, to, files, bytes / 1073741824.0, room / 1073741824.0);
    if (need > room) { LOG("[RED] storage: not moved: %s has %.2f GB free, the data needs %.2f GB", to, room / 1073741824.0, need / 1073741824.0); mv_end(DM_MV_FAIL, -ENOSPC, to); return NULL; }
    char mp[700], mt[1200]; snprintf(mp, sizeof mp, "%s/%s", T.internal, DM_ST_MOVE); snprintf(mt, sizeof mt, "from=%s\nto=%s\n", from, to);
    int r = dm_st_write(mp, mt);
    if (r) { LOG("[RED] storage: not moved: cannot write %s (errno %d)", mp, -r); mv_end(DM_MV_FAIL, r, mp); return NULL; }
    dm_mv m; memset(&m, 0, sizeof m); m.total = bytes; m.cancel = &T.cancel; m.note = mv_note;
    r = dm_st_move_copy(from, to, &m);
    if (!r) {   // the commit: the choice names the new root
        pthread_mutex_lock(&T.mu); r = choice_write(to);
        if (!r) { T.cur = ti; pointer_write(); }
        pthread_mutex_unlock(&T.mu);
        if (r) { dm_st_remove_moving(to, from); snprintf(m.err, sizeof m.err, "%s/%s", T.internal, DM_ST_FILE); }
    }
    struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1); double s = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    if (r) {
        unlink(mp);
        if (r == -ECANCELED) { LOG("storage: the move was cancelled after %.1f s: the copy is removed, the data stays on %s", s, from); mv_end(DM_MV_CANCELLED, r, NULL); }
        else { LOG("[RED] storage: the move failed (errno %d at %s) after %.1f s: the copy is removed, the data stays on %s", -r, m.err, s, from); mv_end(DM_MV_FAIL, r, m.err); }
        return NULL;
    }
    dm_st_remove_moving(from, from);   // the old copy (the marker stays: the next start deletes what the app writes there before it ends)
    LOG("storage: the game data moved to %s in %.1f s (%.1f MB/s): the app ends, its next start reads it there", to, s, s > 0 ? bytes / 1048576.0 / s : 0);
    mv_end(DM_MV_DONE, 0, to);
    return NULL;
}
int dm_storage_move_begin(int to_sd) {
    pthread_mutex_lock(&T.mu);
    if (T.mv.state == DM_MV_RUN) { pthread_mutex_unlock(&T.mu); return -EBUSY; }
    int ti = to_sd ? sd_index_locked() : 0;
    if (ti < 0 || ti == T.cur || !dm_st_is_dir(T.v[ti].path) || !T.internal[0]) { pthread_mutex_unlock(&T.mu); return -ENODEV; }
    memset(&T.mv, 0, sizeof T.mv); T.mv.state = DM_MV_RUN; T.mv.to_sd = to_sd; T.mv.sizing = 1; T.cancel = 0; T.to = ti;
    pthread_mutex_unlock(&T.mu);
    pthread_t t;
    if (pthread_create(&t, NULL, move_thread, NULL)) { mv_end(DM_MV_FAIL, -EAGAIN, "thread"); return -1; }
    pthread_detach(t);
    return 0;
}
void dm_storage_move_cancel(void) { T.cancel = 1; LOG("storage: the player cancelled the move"); }
void dm_storage_move_get(dm_storage_move *o) { pthread_mutex_lock(&T.mu); *o = T.mv; pthread_mutex_unlock(&T.mu); }
void dm_storage_move_clear(void) { pthread_mutex_lock(&T.mu); if (T.mv.state != DM_MV_RUN && T.mv.state != DM_MV_DONE) memset(&T.mv, 0, sizeof T.mv); pthread_mutex_unlock(&T.mu); }
