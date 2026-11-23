// Android port: the save's cloud sync, as Steam Cloud does it for a game: the save is taken from the server before the game starts when the server's is newer, and goes up
// when the app leaves the screen or ends (and, as a safety net, once it stayed changed 10 min while the game runs). The server (rooms: /api/saves/g/<package>/{meta,data,session}, a Bearer token) keeps one save a game,
// opaque bytes: the deterministic tar of savesync.h, whose SHA-256 is the save's identity. savesync.h holds the parts that need no Android (the archive, sync.txt, the state,
// the meta, the start's decision; dm/host/tests/t_savesync.c tests them); this file is the transport, the threads, the start screen's content and the corner mark.
//
// The device link writes sync.txt (savesync.h ss_link_*): a start without it asks rooms' link/start once (3 s at most, online, not a trial build, not this device only) with
// the device's key (sha256 of "nx2a-savecloud:" + ANDROID_ID, the same for every game this signing key ports): another game on the device linked it already, so the
// token comes back at once, sync.txt is written and the check goes on; else the start goes on without the sync (no code is shown at the start). The launcher's Cloud
// save page links by hand (link_thread: a code and its QR code, polled every 3 s; dm_cloud_link_*) and unlinks this app (sync.txt removed, dm_cloud_unlink).
// A token the server refuses (401/403 to meta, data or session; a hand-pushed sync.txt's or the link's alike) moves sync.txt to sync.txt.revoked and asks link/start
// once a run (token_refused): linked, the new token is used at once and the request goes again; else the run goes on as not linked. Never a question in the game.
// The server's name for the device is the one shown and compared: its answers carry you: {id, name}, the requests that record a name (PUT data, POST session) send
// you=1 so the server writes its own, and sync.txt's device= follows it (the names before kept as prev_device=: you_seen_locked).
// Off unless the files folder has sync.txt (url=, token=, device=: savesync.h ss_cfg): without it nothing here runs and the app is as it was. The token is never logged
// (its first 4 characters only). A trial build (an APK without a data server, a debuggable one, env.txt's DM_SAVESYNC=off) and a game whose sync the player switched off (the
// launcher's Cloud save page: this device only) run without the network at all; such a run is marked offline_play in the state (saves/sync-state.txt), and the next start with the sync asks before that
// run's save goes up.
//
// Offline first: a device without a network (ConnectivityManager: none at all) starts the game at once with its save, no question. The early check (dm_cloud_precheck,
// from onCreate, beside the data's step) links the device (link/start, no sync.txt yet), reads sync.txt, archives this device's save and asks the server's meta with
// 3 s in all, so the launcher knows the cloud's state from the start; no answer in that time is offline (no question: the cloud never holds the start). It asks the
// player nothing and puts nothing in place: the start (dm_cloud_begin, from game_start before the host mounts the save, after the save manager's requests, the data
// and the shaders) takes its meta, archives the save again and decides, on a thread of its own with its screen (datasync_android.c dm_cloud_screen_begin): nothing
// shows when it ends within 300 ms. A check the player asked for (the launcher's, a Retry) that gets no answer offers Play anyway (taken by itself after 10 s without
// input) and Retry. B at any moment plays without the sync this run. When
// the server's save is newer and this device's did not change since the last sync, it is downloaded (a progress bar, 60 s at most), checked against its SHA-256 and swapped
// in through the save manager's import (saves_android.c dm_saves_import_now: the current save is exported first, <stamp>-before-import). When both changed, the player
// chooses between two cards (nothing is overwritten by itself; the side not chosen stays: this device's in the import's backup folder, the server's previous save in the
// server's history). Another device first (savesync.h ss_other_device, from the session): one with a save the cloud has not got (dirty, in the last 30 days) is warned
// about, Start anyway or Cancel; one running the game now (a heartbeat in the last 3 min) is a note on the screen for 1.5 s; one whose heartbeats stopped (killed) is
// nothing. Then the session is marked open on the server (dirty 0: the start just synced).
//
// Uploads (the "savesync" thread): the commit count (hle_fs.c dm_fs_commit_count) is polled every 1.5 s. A commit sends nothing while the game runs (savesync.h ss_tick):
// it makes the save owed (pending), and the session's heartbeat says dirty at once. The save goes up when the app leaves the screen (onPause: sleep, HOME, the app's
// end, under a 15 s partial wake lock), at the menu's Exit game (dm_cloud_flush), and by the safety net once it stayed owed SS_SAFETY_UPLOAD_SECS (10 min; env.txt
// DM_SAVESYNC_SAFETY_SECS) while the game runs, then again that long after its next change. An upload archives the committed tree (never <DM_SAVE>.work; a commit while it
// is read reads it again) and, when its hash is not the last synced one, PUTs it with base = the last synced seq and files = its file count. 409: the server moved
// (another device): a conflict, kept for the next start's cards, not retried, one corner note; nothing is ever asked while the game runs. No network: retried after 1, 2,
// 5, 10 min, and at once when the device's network comes back (ConnectivityManager: no request is made while the device has none) or the app is back in front, once the
// safety net made it due. A run that started offline opens its session once the network is back. onPause closes the session with dirty = whether a save is still owed
// (pending or a conflict). While the game is on screen the upload thread sends a heartbeat every 60 s (alive, dirty the same), one at once when dirty changes and one
// after an upload went up; onResume opens it again. A heartbeat that does not go through is dropped (offline). The corner marks, icons only (dm_cloud_mark): every
// upload's spinner and, when it went up, its check (uploads less than 3 s apart: one spinner, one check); a save only owed shows nothing (automatic: the normal state);
// waiting and a conflict a change of state only, none of a kind within 5 min (savesync.h ss_up_begin, ss_up_end, ss_mark_now). Every network failure is a log line:
// the game never waits for the network once it runs.
//
// The mode (savesync.h SS_MODE_*, the game's own, in its state): automatic as above; manual sends only what the player asks for (the launcher's Upload now,
// a card of the start), a burst of commits is only hashed (owed or not: the struck-through cloud stays in the corner while a save is owed), and the next start asks
// about it (ManualPending.dc.html) unless the cloud moved too (the conflict's cards); this device only asks and sends nothing (no network at all), and leaving it makes
// the next check a first connection. The launcher (datasync_android.c) reads this file's state (dm_cloud_info_get) and asks it for Upload now, Back to the cloud save
// (the cloud's save in, this device's into the backup slot saves/device-backup) and a check again, each on a thread of its own before the game starts.
#include "dm.h"
#include "savesync.h"
#include "savecloud.h"
#include "saves_android.h"
#include "startscreen.h"
#include "pixel_font.h"
#include <android/asset_manager.h>
#include <android/keycodes.h>
#include <jni.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/system_properties.h>
// the internal files folder (front_android.c DM_HOME): sync.txt and the save manager's folder stay there whatever volume holds the game data (storage_android.c);
// DM_ROOT for a host without it
static const char *home_or_null(void) { const char *h = getenv("DM_HOME"); return h && h[0] ? h : getenv("DM_ROOT"); }
static const char *home_dir(void) { const char *h = home_or_null(); return h ? h : "."; }

uint64_t dm_fs_commit_count(void);       // hle_fs.c
void dm_perf_background_thread(const char *who);   // perf_android.c: nice 19, the slowest cores, out of the ADPF session
void dm_perf_bg_mark(int delta);         // perf_android.c: the adaptive frame rate does not judge the long frames of an upload (+1 its start, -1 its end)
const uint8_t *dm_overlay_font(void);    // overlay_theme.c

static int64_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static int has_glyph(uint32_t cp) { const pf_index *g = pf_find(dm_overlay_font(), cp); return g && (g->cp_adv >> 8) == cp; }   // not the U+FFFD a font without it draws
static int korean(void) { return has_glyph(0xAC00); }   // the APK's font has Hangul (assets/font/ui.bin): the screens in Korean, else in English

// ---- the state (C.mu guards all of it) ----
static struct {
    pthread_mutex_t mu; pthread_cond_t cv;
    int mode, trial, began, skip_req;    // savesync.h SS_RUN_*; a trial build; dm_cloud_begin read sync.txt; the launcher's Play without the sync before the check
    ss_cfg cfg; char pkg[256], build[64], gver[64], state_path[700], base[900];   // base: <url>/api/saves/g/<package>
    ss_state st;
    int skipped;                         // this run plays without the sync (B at the start)
    int uploading, offline, fails, kick, pause_ev, resume_ev, up_started; unsigned pause_done;   // pause_done: the leaves (pause_flush) done
    int sess_open;                       // the server took this run's session open (or a heartbeat): a run that started offline opens it once the network is back
    ss_notes notes;                      // the corner marks (savesync.h ss_up_begin, ss_up_end, ss_mark_now)
    int64_t next_try;                    // when an owed upload is tried again (monotonic ms)
    unsigned ver;                        // moves on with each change of the state (changed_locked)
    JavaVM *vm; jobject act;             // the application's Context (a global reference: app_context), what the threads ask the system with
    ss_meta meta; int meta_ok;           // the server's meta as this run's check (or a launcher's) last read it
    int lknown; char lsha[65]; uint64_t lsize; int64_t lfiles, lt;   // this device's save as its archive, before the game (the launcher's page)
    int busy, prog; char result[96]; int result_bad;   // the launcher's action (DM_CLOUD_ACT_*) and its download's progress; its outcome
    int attached, trial_base, reached, auto_tried;     // dm_cloud_attach ran; no data server or debuggable (trial without env.txt's word); dm_cloud_begin ran; its link/start asked
    char key[65], model[96], label[160];               // the device link's: the device's key (never logged whole), its model, the app's label
    unsigned cfg_gen;                                  // moves on each time C.cfg changes (a request's hres.gen: which token it was sent with)
    ss_reauth reauth; int reauth_busy;                 // a refused token's link/start (savesync.h ss_reauth_*: once a run); one thread at a time asks it
    int srv_read; char srv_asset[700];                 // assets/nx2a/savecloud.txt read (server_asset_load); its text, "" without one (the porter had no savecloud.txt)
} C = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, .mode = SS_RUN_NONE};
static void st_save_locked(void) { if (C.state_path[0] && ss_state_save(C.state_path, &C.st)) LOG("[RED] cloud: cannot write %s (errno %d)", C.state_path, errno); }
static void changed_locked(void) { C.ver++; }
_Static_assert(SS_MARK_BUSY == DM_CLOUD_MARK_BUSY && SS_MARK_SAVED == DM_CLOUD_MARK_SAVED && SS_MARK_WAITING == DM_CLOUD_MARK_OFFLINE && SS_MARK_CONFLICT == DM_CLOUD_MARK_CONFLICT
               && SS_MARK_SAVED_MS == DM_CLOUD_MARK_SAVED_MS && SS_MARK_WAITING_MS == DM_CLOUD_MARK_OFFLINE_MS && SS_MARK_CONFLICT_MS == DM_CLOUD_MARK_CONFLICT_MS, "the corner marks");

// ---- HTTP: java.net.HttpURLConnection through JNI, as datasync_android.c (the system's DNS, proxy, TLS trust store; no dex of the app's own) ----
static struct { int ok; jclass cUrl, cConn, cIn, cOut; jmethodID mUrl, mOpen, mConnTo, mReadTo, mProp, mCaches, mMethod, mDoOut, mFixed, mCode, mHeader, mLen, mIn, mErr, mOut,
                mDisc, mRead, mClose, mWrite, mOClose; } J;
static pthread_mutex_t J_mu = PTHREAD_MUTEX_INITIALIZER;
static int jx(JNIEnv *E, char *why, size_t cap) {   // an exception: cleared, its toString() in why (never with the token: it is only in a request header)
    if (!(*E)->ExceptionCheck(E)) return 0;
    jthrowable t = (*E)->ExceptionOccurred(E); (*E)->ExceptionClear(E);
    if (why && cap) { why[0] = 0;
        jclass c = t ? (*E)->FindClass(E, "java/lang/Object") : NULL; jmethodID m = c ? (*E)->GetMethodID(E, c, "toString", "()Ljava/lang/String;") : NULL;
        jstring s = m ? (*E)->CallObjectMethod(E, t, m) : NULL; if ((*E)->ExceptionCheck(E)) { (*E)->ExceptionClear(E); s = NULL; }
        const char *u = s ? (*E)->GetStringUTFChars(E, s, NULL) : NULL; if (u) { snprintf(why, cap, "%s", u); (*E)->ReleaseStringUTFChars(E, s, u); } }
    if (t) (*E)->DeleteLocalRef(E, t);
    return 1;
}
static int jinit(JNIEnv *E) {
    pthread_mutex_lock(&J_mu);
    if (!J.ok) {
        char why[200] = "";
        jclass u = (*E)->FindClass(E, "java/net/URL"), c = u ? (*E)->FindClass(E, "java/net/HttpURLConnection") : NULL, i = c ? (*E)->FindClass(E, "java/io/InputStream") : NULL,
               o = i ? (*E)->FindClass(E, "java/io/OutputStream") : NULL;
        if (jx(E, why, sizeof why) || !o) LOG("[RED] cloud: no java.net classes (%s)", why);
        else {
            J.cUrl = (*E)->NewGlobalRef(E, u); J.cConn = (*E)->NewGlobalRef(E, c); J.cIn = (*E)->NewGlobalRef(E, i); J.cOut = (*E)->NewGlobalRef(E, o);
            J.mUrl = (*E)->GetMethodID(E, u, "<init>", "(Ljava/lang/String;)V"); J.mOpen = (*E)->GetMethodID(E, u, "openConnection", "()Ljava/net/URLConnection;");
            J.mConnTo = (*E)->GetMethodID(E, c, "setConnectTimeout", "(I)V"); J.mReadTo = (*E)->GetMethodID(E, c, "setReadTimeout", "(I)V");
            J.mProp = (*E)->GetMethodID(E, c, "setRequestProperty", "(Ljava/lang/String;Ljava/lang/String;)V"); J.mCaches = (*E)->GetMethodID(E, c, "setUseCaches", "(Z)V");
            J.mMethod = (*E)->GetMethodID(E, c, "setRequestMethod", "(Ljava/lang/String;)V"); J.mDoOut = (*E)->GetMethodID(E, c, "setDoOutput", "(Z)V");
            J.mFixed = (*E)->GetMethodID(E, c, "setFixedLengthStreamingMode", "(J)V"); J.mCode = (*E)->GetMethodID(E, c, "getResponseCode", "()I");
            J.mHeader = (*E)->GetMethodID(E, c, "getHeaderField", "(Ljava/lang/String;)Ljava/lang/String;"); J.mLen = (*E)->GetMethodID(E, c, "getContentLengthLong", "()J");
            J.mIn = (*E)->GetMethodID(E, c, "getInputStream", "()Ljava/io/InputStream;"); J.mErr = (*E)->GetMethodID(E, c, "getErrorStream", "()Ljava/io/InputStream;");
            J.mOut = (*E)->GetMethodID(E, c, "getOutputStream", "()Ljava/io/OutputStream;"); J.mDisc = (*E)->GetMethodID(E, c, "disconnect", "()V");
            J.mRead = (*E)->GetMethodID(E, i, "read", "([B)I"); J.mClose = (*E)->GetMethodID(E, i, "close", "()V");
            J.mWrite = (*E)->GetMethodID(E, o, "write", "([BII)V"); J.mOClose = (*E)->GetMethodID(E, o, "close", "()V");
            jx(E, NULL, 0);   // (a method an old Android lacks is checked where it is used: getContentLengthLong)
            if (!J.mUrl || !J.mOpen || !J.mConnTo || !J.mReadTo || !J.mProp || !J.mCaches || !J.mMethod || !J.mDoOut || !J.mCode || !J.mHeader || !J.mIn || !J.mOut || !J.mDisc || !J.mRead || !J.mClose || !J.mWrite || !J.mOClose || !J.mFixed) LOG("[RED] cloud: java.net methods missing (%s)", why);
            else J.ok = 1;
        }
    }
    int ok = J.ok; pthread_mutex_unlock(&J_mu); return ok;
}
static JNIEnv *jattach(void) { JNIEnv *E = NULL; if ((*C.vm)->AttachCurrentThread(C.vm, &E, NULL) != JNI_OK) return NULL; return jinit(E) ? E : NULL; }
typedef struct { int code; ss_buf body; char seq[32], sha[80], why[200]; unsigned gen; } hres;   // gen: C.cfg_gen when its token was taken
typedef void (*hprog)(uint64_t done, int64_t total);
static void jheader(JNIEnv *E, jobject C_, const char *k, char *out, size_t cap) {
    out[0] = 0; jstring n = (*E)->NewStringUTF(E, k); jstring v = n ? (*E)->CallObjectMethod(E, C_, J.mHeader, n) : NULL;
    if (jx(E, NULL, 0) || !v) return;
    const char *s = (*E)->GetStringUTFChars(E, v, NULL); if (!s) { jx(E, NULL, 0); return; }
    snprintf(out, cap, "%s", s); (*E)->ReleaseStringUTFChars(E, v, s);
}
static int jprop(JNIEnv *E, jobject c, const char *k, const char *v) { jstring a = (*E)->NewStringUTF(E, k), b = a ? (*E)->NewStringUTF(E, v) : NULL; if (b) (*E)->CallVoidMethod(E, c, J.mProp, a, b); return jx(E, NULL, 0) || !b ? -1 : 0; }
// One request: method GET, PUT (body, its hash in X-Save-Sha256) or POST (no body). 0 when the server answered (r->code, its body up to cap bytes in r->body, X-Save-Seq and
// X-Save-Sha256), -1 when it did not (r->why). deadline: monotonic ms the whole request ends by (each blocking read waits at most what is left of it)
// ctype: the body's Content-Type (NULL: application/octet-stream); auth: the token's Authorization header (the device link's requests have none)
static int http_req(JNIEnv *E, const char *method, const char *url, const uint8_t *body, size_t n, const char *body_sha, const char *ctype, int auth, int conn_ms, int64_t deadline,
                    size_t cap, hres *r, hprog prog) {
    memset(r, 0, sizeof *r); int rc = -1, auth_on = auth; jobject U = NULL, Cn = NULL, in = NULL, out = NULL; jbyteArray buf = NULL; char auth_h[600];
    if ((*E)->PushLocalFrame(E, 64)) { jx(E, NULL, 0); snprintf(r->why, sizeof r->why, "no JNI frame"); return -1; }
    jstring su = (*E)->NewStringUTF(E, url); if (jx(E, r->why, sizeof r->why) || !su) goto out;
    U = (*E)->NewObject(E, J.cUrl, J.mUrl, su); if (jx(E, r->why, sizeof r->why) || !U) goto out;
    Cn = (*E)->CallObjectMethod(E, U, J.mOpen); if (jx(E, r->why, sizeof r->why) || !Cn || !(*E)->IsInstanceOf(E, Cn, J.cConn)) goto out;
    int64_t left = deadline - now_ms(); if (left < 200) { snprintf(r->why, sizeof r->why, "out of time"); goto out; }
    (*E)->CallVoidMethod(E, Cn, J.mConnTo, (jint)(conn_ms < left ? conn_ms : left)); (*E)->CallVoidMethod(E, Cn, J.mReadTo, (jint)(left < 10000 ? left : 10000));
    (*E)->CallVoidMethod(E, Cn, J.mCaches, JNI_FALSE); if (jx(E, r->why, sizeof r->why)) goto out;
    (*E)->CallVoidMethod(E, Cn, J.mMethod, (*E)->NewStringUTF(E, method)); if (jx(E, r->why, sizeof r->why)) goto out;
    int bad = 0;
    if (auth_on) {   // (C.cfg may change on another thread: a refused token's new one)
        pthread_mutex_lock(&C.mu); r->gen = C.cfg_gen; int tk = C.cfg.token[0] != 0; if (tk) snprintf(auth_h, sizeof auth_h, "Bearer %s", C.cfg.token); pthread_mutex_unlock(&C.mu);
        if (tk) bad = jprop(E, Cn, "Authorization", auth_h);
        memset(auth_h, 0, sizeof auth_h);
    }
    if (bad || jprop(E, Cn, "Accept-Encoding", "identity")) { snprintf(r->why, sizeof r->why, "a request header was refused"); goto out; }
    buf = (*E)->NewByteArray(E, 256 << 10); if (jx(E, r->why, sizeof r->why) || !buf) goto out;
    if (strcmp(method, "GET")) {   // PUT and POST: a body of a known length (POST's is empty)
        if (body_sha && jprop(E, Cn, "X-Save-Sha256", body_sha)) goto out;
        if (body && jprop(E, Cn, "Content-Type", ctype ? ctype : "application/octet-stream")) goto out;
        (*E)->CallVoidMethod(E, Cn, J.mDoOut, JNI_TRUE); (*E)->CallVoidMethod(E, Cn, J.mFixed, (jlong)n); if (jx(E, r->why, sizeof r->why)) goto out;
        out = (*E)->CallObjectMethod(E, Cn, J.mOut); if (jx(E, r->why, sizeof r->why) || !out) goto out;
        for (size_t o = 0; o < n;) {
            if (now_ms() > deadline) { snprintf(r->why, sizeof r->why, "out of time while sending"); goto out; }
            size_t k = n - o > (256u << 10) ? (256u << 10) : n - o;
            (*E)->SetByteArrayRegion(E, buf, 0, (jsize)k, (const jbyte *)(body + o)); (*E)->CallVoidMethod(E, out, J.mWrite, buf, 0, (jint)k);
            if (jx(E, r->why, sizeof r->why)) goto out;
            o += k;
        }
        (*E)->CallVoidMethod(E, out, J.mOClose); if (jx(E, r->why, sizeof r->why)) goto out;
        out = NULL;
    }
    int code = (*E)->CallIntMethod(E, Cn, J.mCode); if (jx(E, r->why, sizeof r->why)) goto out;
    r->code = code; rc = 0;
    jheader(E, Cn, "X-Save-Seq", r->seq, sizeof r->seq); jheader(E, Cn, "X-Save-Sha256", r->sha, sizeof r->sha);
    jlong total = J.mLen ? (*E)->CallLongMethod(E, Cn, J.mLen) : -1; if (jx(E, NULL, 0)) total = -1;   // (getContentLengthLong: Android 7 on)
    in = code < 400 || J.mErr ? (*E)->CallObjectMethod(E, Cn, code < 400 ? J.mIn : J.mErr) : NULL; if (jx(E, NULL, 0)) in = NULL;   // (no error body: an answer all the same)
    if (in) {
        uint8_t *chunk = malloc(256 << 10); if (!chunk) { snprintf(r->why, sizeof r->why, "out of memory"); rc = -1; goto out; }
        for (;;) {
            if (now_ms() > deadline) { snprintf(r->why, sizeof r->why, "out of time while receiving"); rc = -1; break; }
            jint k = (*E)->CallIntMethod(E, in, J.mRead, buf); if (jx(E, r->why, sizeof r->why)) { rc = -1; break; }
            if (k < 0) break;
            if (!k) continue;
            if (r->body.n + (size_t)k > cap) { snprintf(r->why, sizeof r->why, "the answer is longer than %zu bytes", cap); rc = -1; break; }
            (*E)->GetByteArrayRegion(E, buf, 0, k, (jbyte *)chunk);
            if (ss_buf_put(&r->body, chunk, (size_t)k)) { snprintf(r->why, sizeof r->why, "out of memory"); rc = -1; break; }
            if (prog) prog(r->body.n, total);
        }
        free(chunk);
    }
out:
    if (out) { (*E)->CallVoidMethod(E, out, J.mOClose); jx(E, NULL, 0); }
    if (in) { (*E)->CallVoidMethod(E, in, J.mClose); jx(E, NULL, 0); }
    if (Cn) { (*E)->CallVoidMethod(E, Cn, J.mDisc); jx(E, NULL, 0); }
    (*E)->PopLocalFrame(E, NULL);
    if (rc) { if (!r->why[0]) snprintf(r->why, sizeof r->why, "failed"); ss_buf_free(&r->body); }
    else if (ss_buf_put(&r->body, "", 1) == 0) r->body.n--;   // NUL-terminated (for the JSON reader's sake), not counted
    return rc;
}
static int http_do(JNIEnv *E, const char *method, const char *url, const uint8_t *body, size_t n, const char *body_sha, int conn_ms, int64_t deadline, size_t cap, hres *r, hprog prog) {
    return http_req(E, method, url, body, n, body_sha, NULL, 1, conn_ms, deadline, cap, r, prog);
}

// ---- this device's save, archived (with a commit between the start and the end of the read: read again) ----
static int snap(ss_buf *t, char sha[65], int64_t *newest, char *err, size_t ecap) {   // 0 with the archive, 1 no save, -errno
    int r = -EAGAIN;
    for (int k = 0; k < 4 && r == -EAGAIN; k++) {
        uint64_t c0 = dm_fs_commit_count();
        r = ss_tar_tree(dm_saves_save_path(), dm_saves_device_path(), t, newest, err, ecap);
        if (r == 0 && dm_fs_commit_count() != c0) { ss_buf_free(t); r = -EAGAIN; }
        if (r == -EAGAIN) usleep(200 * 1000);
    }
    if (r == -EAGAIN) snprintf(err, ecap, "the game kept saving while its save was read");
    if (!r) ss_sha256(t->p, t->n, sha); else sha[0] = 0;
    return r;
}
static void url_of(char *o, size_t n, const char *what) { pthread_mutex_lock(&C.mu); snprintf(o, n, "%s/%s", C.base, what); pthread_mutex_unlock(&C.mu); }   // (C.mu not held)
// The server's name for this device (you.name in its answers to GET meta, PUT data, POST session; the requests that record a name send you=1, so the server writes its
// own name, not device=): C.cfg.device follows it and sync.txt is written again with it (0600, through a rename), the name before kept as prev_device= so the sessions
// and uploads this app made under it stay this device's (savesync.h ss_name_mine). Under C.mu
static void you_seen_locked(const char *you) {
    char n[64]; snprintf(n, sizeof n, "%s", you ? you : ""); ss_dev_clean(n);
    if (!n[0] || !C.began || !strcmp(n, C.cfg.device)) return;
    char old[64]; snprintf(old, sizeof old, "%s", C.cfg.device);
    ss_cfg_rename(&C.cfg, n);   // (this run's name now, also when sync.txt cannot be written)
    const char *root = home_or_null(); char p[700], out[2048]; size_t len = 0; ss_cfg f; int wrote = 0;
    snprintf(p, sizeof p, "%s/sync.txt", home_dir());
    char *txt = root ? ds_read(p, &len, 1 << 16) : NULL;
    if (txt && ss_cfg_text(txt, &f) && !strcmp(f.token, C.cfg.token) && !ss_cfg_renamed(txt, n, out, sizeof out)) {   // (a sync.txt with another token is not this run's: left as it is)
        if (ss_write_private(p, out, strlen(out))) LOG("[RED] cloud: sync.txt cannot be written with the server's name (errno %d)", errno); else wrote = 1;
    }
    if (txt) { memset(txt, 0, len); free(txt); }
    memset(out, 0, sizeof out); memset(&f, 0, sizeof f);
    LOG("cloud: the server names this device \"%s\" (was \"%s\")%s", n, old, wrote ? ": sync.txt follows (the old name kept as prev_device=)" : ": sync.txt as it was");
    changed_locked();
}
static void you_seen(const char *you) { pthread_mutex_lock(&C.mu); you_seen_locked(you); pthread_mutex_unlock(&C.mu); }
static void shown_dev_locked(char *o, size_t n, const char *dev) { snprintf(o, n, "%s", ss_name_mine(&C.cfg, dev) ? C.cfg.device : dev && dev[0] ? dev : "-"); }   // a name of this device's: its name now
static int token_refused(JNIEnv *E, unsigned gen);
static void link_none(void);
enum { SESS_OPEN, SESS_ALIVE, SESS_CLOSED };
static int owed_now(void) { pthread_mutex_lock(&C.mu); int d = C.st.pending || C.st.conflict; pthread_mutex_unlock(&C.mu); return d; }   // a save the cloud has not got
// The session: state and dirty (a save still owed), 0 when the server took it. A heartbeat (alive) logs only when its outcome or its dirty changed
static int session_post(JNIEnv *E, int state, int dirty, int64_t deadline) {
    static const char *const NAME[] = {"open", "alive", "closed"};
    static int last_alive = -1;   // the last heartbeat's outcome (0 ok, 1 not), dirty in bit 1 (the upload thread's only)
    hres r; int got = -1;
    for (int again = 0; again < 2; again++) {   // a refused token: once more with the new one (token_refused)
        char u[1400], dev[200];
        pthread_mutex_lock(&C.mu); ss_qenc(dev, sizeof dev, C.cfg.device); snprintf(u, sizeof u, "%s/session?state=%s&dirty=%d&device=%s&you=1", C.base, NAME[state], dirty ? 1 : 0, dev); pthread_mutex_unlock(&C.mu);
        got = http_do(E, "POST", u, NULL, 0, NULL, 3000, deadline, 64 << 10, &r, NULL);
        if (got || !ss_refused(r.code) || again) break;
        ss_buf_free(&r.body); unsigned g = r.gen;
        if (!token_refused(E, g)) { LOG("cloud: session %s: the token was refused and this device is not linked now", NAME[state]); return -1; }
    }
    int ok = !got && r.code == 200;
    if (ok) { char yn[64]; if (ss_you_name((const char *)r.body.p, r.body.n, yn, sizeof yn)) you_seen(yn); }
    int quiet = 0;
    if (state == SESS_ALIVE) { int o = (ok ? 0 : 1) | (dirty ? 2 : 0); quiet = o == last_alive; last_alive = o; }
    else last_alive = -1;
    if (!quiet) { if (got) LOG("cloud: session %s (dirty %d): no answer (%s)", NAME[state], dirty ? 1 : 0, r.why);
                  else LOG("cloud: session %s (dirty %d): %s (HTTP %d)", NAME[state], dirty ? 1 : 0, ok ? "ok" : "refused", r.code); }
    ss_buf_free(&r.body);
    return ok ? 0 : -1;
}

// ---- the start screen's state: the view (datasync_android.c draws it), the player's choice ----
enum { WV_WAIT, WV_ASK };   // a step that only waits (its button and B skip the sync), or a question (A chooses, B skips the sync)
static struct {
    dm_cloud_view v; int kind, done, quit, applying, choice, auto_ms; int64_t last_input;
    void (*done_cb)(int quit); int up_after;
    int active, recheck;   // a check runs (its view and input are the screen's); recheck: one the launcher asked for (no done_cb: the launcher goes on)
    int autolink;          // the start's link/start runs (no sync.txt yet), or the start waits for the early check: B waits for it (a few s at most)
} W;
// the early check (dm_cloud_precheck): started; its thread runs; have: its outcome for the start's check (net: SS_NET_OK, SS_NET_NOTFOUND with m, or SS_NET_FAIL: no
// network or no answer in PC_META_MS), taken once, while sync.txt is the one it was for (gen: C.cfg_gen then) and for PC_FRESH_MS after it came (t: then; a first
// download of GBs takes longer: the start's check asks again, as without the early one, so another device's upload meanwhile is seen)
static struct { int started, running, have, net; ss_meta m; unsigned gen; int64_t t; } PC;   // under C.mu
#define PC_META_MS 3000
#define PC_FRESH_MS (3 * 60 * 1000)
void dm_cloud_view_get(dm_cloud_view *v) { pthread_mutex_lock(&C.mu); *v = W.v; if (!W.active) { v->ask = 0; v->urgent = 0; } pthread_mutex_unlock(&C.mu); }
static void up_start(void);
enum { UP_NOTHING, UP_OK, UP_FAIL, UP_CONFLICT, UP_ERR };
static int upload(JNIEnv *E, int64_t deadline);
static void finish_locked(int quit, int skip) {   // the start ends: the screen goes, done() on the main thread
    if (W.done) return;
    W.done = 1; W.active = 0; W.quit = quit; W.up_after = !quit && !skip;
    if (skip) { C.skipped = 1; ss_mark_offline_play(&C.st); st_save_locked(); changed_locked();
                LOG("cloud: this run plays without the sync (the next start asks before its save goes up)"); }
}
// The start's check ended (C.mu not held): its step, the screen's phase (a check the launcher asked for has none), and when the player chose to play without the sync
// (asked: B, a card's Keep it), the launcher starts the game at once
static void ended_now(int quit, int skip, int asked) {
    pthread_mutex_lock(&C.mu); int off = C.offline; pthread_mutex_unlock(&C.mu);
    dm_start_step(STV_CLOUD, skip ? STV_SKIP : off ? STV_SKIP : STV_DONE, skip ? (korean() ? "건너뜀" : "Skipped") : off ? (korean() ? "오프라인" : "Offline") : (korean() ? "완료" : "Done"));
    dm_cloud_screen_end();
    if (asked && skip && !quit) dm_launcher_start_now(1);
    dm_cloud_screen_poke();
}
static void skipped_end(void) { ended_now(0, 1, 1); }   // B: the start ends without the sync
static void finish(int quit, int skip, int asked) {
    pthread_mutex_lock(&C.mu); int was = W.done; finish_locked(quit, skip); pthread_mutex_unlock(&C.mu);
    if (!was) ended_now(quit, skip, asked); else dm_cloud_screen_end();
}
static void screen_done(void) {   // the main thread, once the screen let the window go (the upload thread starts with the game: dm_cloud_game_start)
    void (*d)(int) = W.done_cb; W.done_cb = NULL; if (d) d(W.quit);
}
void dm_cloud_game_start(void) { pthread_mutex_lock(&C.mu); int go = W.up_after; pthread_mutex_unlock(&C.mu); if (go) up_start(); }
// The view, under C.mu. A step that only waits is the cloud's row in the start's steps (its note; B's line under them is its one button); a question is a screen of its
// own: an icon, a title, one or two lines, buttons (sel: the first) or the cards, the hint lines at the bottom
static void blank_locked(int kind) {
    dm_cloud_view *v = &W.v; unsigned ver = v->ver + 1; memset(v, 0, sizeof *v); v->ver = ver; v->progress = -1; v->count_permille = -1;
    W.kind = kind; W.choice = -1; W.last_input = now_ms();
}
static void wait_locked(const char *note_ko, const char *note_en) {
    blank_locked(WV_WAIT); int ko = korean(); dm_cloud_view *v = &W.v;
    snprintf(v->note, sizeof v->note, "%s", ko ? note_ko : note_en);
    v->nbtn = 1; snprintf(v->btn[0], sizeof v->btn[0], "%s", ko ? "동기화 없이 플레이" : "Play without the sync");
}
static void wait_view(const char *note_ko, const char *note_en) { pthread_mutex_lock(&C.mu); wait_locked(note_ko, note_en); pthread_mutex_unlock(&C.mu); dm_cloud_screen_poke(); }
static void ask_locked(int icon, const char *title, const char *l0, const char *l1, int nbtn, const char *b0, const char *b1, const char *foot, const char *foot2) {
    blank_locked(WV_ASK); dm_cloud_view *v = &W.v; v->urgent = 1; v->ask = 1; v->icon = icon;
    snprintf(v->title, sizeof v->title, "%s", title);
    snprintf(v->line[0], sizeof v->line[0], "%s", l0 ? l0 : ""); snprintf(v->line[1], sizeof v->line[1], "%s", l1 ? l1 : "");
    v->nbtn = nbtn; if (nbtn > 0) snprintf(v->btn[0], sizeof v->btn[0], "%s", b0); if (nbtn > 1) snprintf(v->btn[1], sizeof v->btn[1], "%s", b1);
    snprintf(v->foot, sizeof v->foot, "%s", foot ? foot : ""); snprintf(v->foot2, sizeof v->foot2, "%s", foot2 ? foot2 : "");
}
static void ask_view(int icon, const char *title, const char *l0, const char *l1, int nbtn, const char *b0, const char *b1, const char *foot, const char *foot2) {
    pthread_mutex_lock(&C.mu); ask_locked(icon, title, l0, l1, nbtn, b0, b1, foot, foot2); pthread_mutex_unlock(&C.mu); dm_cloud_screen_poke();
}
static int ask(int auto_ms, const char *count_ko, const char *count_en) {   // the player's choice (the button or card), auto_ms: Play anyway (0) after that long without input; -1: the start ended
    pthread_mutex_lock(&C.mu); int c = -1, last = -1, lastp = -1;
    while (!W.done && W.choice < 0) {
        int64_t idle = now_ms() - W.last_input;
        if (auto_ms && idle >= auto_ms) { W.choice = 0; LOG("cloud: no input for %d s: Play anyway", auto_ms / 1000); break; }
        if (auto_ms) { int s = (int)((auto_ms - idle + 999) / 1000), p = (int)(idle * 1000 / auto_ms);   // the countdown's text and bar (the time gone of it)
            if (s != last || p / 25 != lastp) { last = s; lastp = p / 25; snprintf(W.v.count, sizeof W.v.count, korean() ? count_ko : count_en, s); W.v.count_permille = p; W.v.ver++;
                pthread_mutex_unlock(&C.mu); dm_cloud_screen_poke(); pthread_mutex_lock(&C.mu); continue; } }
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += 250 * 1000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&C.cv, &C.mu, &ts);
    }
    if (!W.done) c = W.choice;
    pthread_mutex_unlock(&C.mu); return c;
}
static int ended(void) { pthread_mutex_lock(&C.mu); int d = W.done; pthread_mutex_unlock(&C.mu); return d; }
static void dl_prog(uint64_t done, int64_t total) {
    static int64_t last; int64_t t = now_ms(); if (t - last < 100) return; last = t;
    pthread_mutex_lock(&C.mu); if (total > 0) { W.v.progress = (int)(done * 1000 / (uint64_t)total); C.prog = W.v.progress; } W.v.ver++; pthread_mutex_unlock(&C.mu); dm_cloud_screen_poke();
}
static void size_line(char *o, size_t n, uint64_t size, int64_t files, int ko) {   // "1.2 MB · 파일 9개"; the size only when the count is not known
    char sz[32]; ss_fmt_size(sz, sizeof sz, size);
    if (files < 0) snprintf(o, n, "%s", sz); else if (ko) snprintf(o, n, "%s · 파일 %lld개", sz, (long long)files); else snprintf(o, n, "%s · %lld file%s", sz, (long long)files, files == 1 ? "" : "s");
}
// the two cards: this device's (0) and the server's (1), the newer one chosen first. local_size, local_files: this device's archive (its tar, as it would go up)
static void cards_view(const ss_meta *m, int64_t local_t, uint64_t local_size, int64_t local_files) {
    int ko = korean(); int64_t now = (int64_t)time(NULL); char md[64];
    pthread_mutex_lock(&C.mu); shown_dev_locked(md, sizeof md, m->device);
    ask_locked(DM_CLOUD_ICON_NONE, ko ? "어느 세이브로 할까요?" : "Which save do you want to keep?", ko ? "이 기기와 클라우드에 서로 다른 진행이 있어요" : "This device and the cloud have different progress", NULL, 0, NULL, NULL,
               ko ? "고르지 않은 쪽도 백업해 둬요" : "The one you do not choose is kept as a backup", ko ? "← → 고르기 · A 확정 · B 동기화 없이 플레이" : "← → choose · A confirm · B play without the sync");
    dm_cloud_view *v = &W.v; v->ncards = 2;
    snprintf(v->card[0].head, sizeof v->card[0].head, ko ? "이 기기 · %s" : "This device · %s", C.cfg.device);
    snprintf(v->card[1].head, sizeof v->card[1].head, ko ? "클라우드 · %s에서 올림" : "Cloud · from %s", md);
    ss_fmt_date(v->card[0].date, sizeof v->card[0].date, local_t, ko); ss_fmt_ago(v->card[0].ago, sizeof v->card[0].ago, local_t, now, ko);
    ss_fmt_date(v->card[1].date, sizeof v->card[1].date, m->time, ko); ss_fmt_ago(v->card[1].ago, sizeof v->card[1].ago, m->time, now, ko);
    size_line(v->card[0].size, sizeof v->card[0].size, local_size, local_files, ko);
    if (m->size) size_line(v->card[1].size, sizeof v->card[1].size, m->size, m->files, ko);   // (a server that keeps no file count: the size only)
    snprintf(v->badge, sizeof v->badge, "%s", ko ? "더 최근" : "More recent");
    int newer = m->time > local_t ? 1 : 0; v->card[newer].newer = 1; v->sel = newer;
    pthread_mutex_unlock(&C.mu); dm_cloud_screen_poke();
}
static void when_size(char *o, size_t n, int64_t t, uint64_t size, int ko) {   // "오늘 오전 2:10 · 7.1 MB"
    char w[64], z[32]; ss_fmt_when(w, sizeof w, t, (int64_t)time(NULL), ko); ss_fmt_size(z, sizeof z, size);
    if (size) snprintf(o, n, "%s · %s", w, z); else snprintf(o, n, "%s", w);
}
// AfterTest.dc.html: a run without the sync (the launcher's, B on a card) changed this device's save and the cloud did not move: back to the cloud's (this device's to
// the backup slot), this device's up (the cloud's previous one stays in the server's history), or kept (this run without the sync too)
static void after_test_view(const ss_meta *m, int64_t local_t, uint64_t local_size) {
    int ko = korean(); char cv[96];
    pthread_mutex_lock(&C.mu);
    ask_locked(DM_CLOUD_ICON_NONE, ko ? "지난번에 동기화 없이 한 플레이가 있어요" : "You played without the sync last time", ko ? "테스트로 하셨다면 클라우드 세이브로 되돌리세요" : "If it was a test, go back to the cloud save", NULL,
               2, ko ? "클라우드 세이브로 되돌리기" : "Back to the cloud save", ko ? "이 기기 것 올리기" : "Upload this device's",
               ko ? "되돌린 테스트 세이브는 이 기기 백업 1칸에 남아요" : "The test save stays in this device's backup slot", ko ? "↑↓ 고르기 · A 선택" : "↑↓ choose · A select");
    dm_cloud_view *v = &W.v; v->style = DM_CLOUD_STYLE_LIST; v->nbtn = 3; snprintf(v->btn[2], sizeof v->btn[2], "%s", ko ? "그대로 두기" : "Keep it");
    snprintf(v->bnote[1], sizeof v->bnote[1], "%s", ko ? "클라우드 세이브는 백업해 둬요" : "The cloud's is kept as a backup");
    snprintf(v->bnote[2], sizeof v->bnote[2], "%s", ko ? "이번에도 올리지 않고 플레이" : "Play without uploading again");
    v->nrow = 2; snprintf(v->row_k[0], sizeof v->row_k[0], "%s", ko ? "이 기기 (테스트 플레이)" : "This device (test play)"); when_size(v->row_v[0], sizeof v->row_v[0], local_t, local_size, ko);
    snprintf(v->row_k[1], sizeof v->row_k[1], "%s", ko ? "클라우드" : "Cloud"); when_size(cv, sizeof cv, m->time, m->size, ko);
    char md[64]; shown_dev_locked(md, sizeof md, m->device); snprintf(v->row_v[1], sizeof v->row_v[1], "%s · %s", md, cv);
    pthread_mutex_unlock(&C.mu); dm_cloud_screen_poke();
}
// ManualPending.dc.html: manual mode, this device's save changed since it last went up and the cloud did not move: A uploads it now, B keeps it (owed, the corner's mark)
static void manual_view(const ss_meta *m, int64_t local_t, uint64_t local_size) {
    int ko = korean(); char w[64];
    pthread_mutex_lock(&C.mu);
    ask_locked(DM_CLOUD_ICON_NONE, ko ? "이 기기에 안 올린 진행이 있어요" : "This device has progress not uploaded", ko ? "수동 모드 · 올릴지 정해 주세요" : "Manual mode · choose whether it goes up", NULL,
               2, ko ? "A · 올리기" : "A · Upload", ko ? "B · 그대로 두기" : "B · Keep it", ko ? "클라우드 세이브 설정에서 자동으로 바꿀 수 있어요" : "The cloud save settings can make it automatic", NULL);
    dm_cloud_view *v = &W.v; v->style = DM_CLOUD_STYLE_PAIR; v->nrow = 2;
    snprintf(v->row_k[0], sizeof v->row_k[0], "%s", ko ? "이 기기" : "This device"); when_size(v->row_v[0], sizeof v->row_v[0], local_t, local_size, ko);
    snprintf(v->row_k[1], sizeof v->row_k[1], "%s", ko ? "클라우드" : "Cloud"); ss_fmt_when(w, sizeof w, m->time, (int64_t)time(NULL), ko);
    if (!m->seq) snprintf(v->row_v[1], sizeof v->row_v[1], "%s", ko ? "아직 없음" : "None yet");
    else if (ss_name_mine(&C.cfg, m->device)) snprintf(v->row_v[1], sizeof v->row_v[1], ko ? "이 기기에서 올림 · %s" : "From this device · %s", w);
    else snprintf(v->row_v[1], sizeof v->row_v[1], ko ? "%s에서 올림 · %s" : "From %s · %s", m->device[0] ? m->device : "-", w);
    pthread_mutex_unlock(&C.mu); dm_cloud_screen_poke();
}
#define BACKUP_SLOT "device-backup"   // saves/<this>: Back to the cloud save's backup of this device's save, one place overwritten each time
// The server's save: downloaded, checked, unpacked into saves/<stamp>-cloud and imported (saves_android.c: the current save backed up first, into saves/<slot> when slot
// is given). 0, -1 (why), -2 the start ended. act: the launcher's Back to the cloud save (no start to end, no button on its view)
static int download_apply(JNIEnv *E, const ss_meta *m, char *why, size_t wcap, const char *slot, int act) {
    char u[1400], sha[65], err[600], name[64], dir[800], dird[820], part[820], partd[840];
    pthread_mutex_lock(&C.mu); wait_locked("받는 중…", "Downloading…"); W.v.progress = 0; C.prog = 0; if (act) W.v.nbtn = 0; pthread_mutex_unlock(&C.mu); dm_cloud_screen_poke();
    url_of(u, sizeof u, "data"); hres r;
    if (http_do(E, "GET", u, NULL, 0, NULL, 3000, now_ms() + 60000, SS_MAX_BODY, &r, dl_prog)) { snprintf(why, wcap, "%s", r.why); return !act && ended() ? -2 : -1; }
    if (r.code != 200) { snprintf(why, wcap, "HTTP %d", r.code); ss_buf_free(&r.body); return -1; }
    ss_sha256(r.body.p, r.body.n, sha); for (char *q = r.sha; *q; q++) if (*q >= 'A' && *q <= 'F') *q += 32;
    if ((r.sha[0] && strcmp(r.sha, sha)) || (!r.sha[0] && strcmp(sha, m->sha))) { snprintf(why, wcap, "the download does not match its SHA-256 (damaged on the way)"); ss_buf_free(&r.body); return -1; }
    uint64_t seq = r.seq[0] ? strtoull(r.seq, NULL, 10) : m->seq; if (!seq) seq = m->seq;
    pthread_mutex_lock(&C.mu); int d = !act && W.done; if (!d) { W.applying = 1; C.prog = -1; wait_locked("적용하는 중…", "Putting it in place…"); W.v.nbtn = 0; W.v.urgent = 1; }   // (B cannot stop it now: no button)
    pthread_mutex_unlock(&C.mu); dm_cloud_screen_poke();
    if (d) { ss_buf_free(&r.body); return -2; }   // (the player skipped: nothing is changed)
    time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm); strftime(name, sizeof name, "%Y%m%d-%H%M%S-cloud", &tm);
    snprintf(dir, sizeof dir, "%s/%s", dm_saves_dir(), name); snprintf(dird, sizeof dird, "%s.device", dir);
    snprintf(part, sizeof part, "%s/.%s.part", dm_saves_dir(), name); snprintf(partd, sizeof partd, "%s.device", part);
    ss_rm_tree(part); ss_rm_tree(partd); ss_rm_tree(dir); ss_rm_tree(dird);
    int rc = -1; struct stat st;
    if (ss_untar(r.body.p, r.body.n, part, partd, err, sizeof err)) snprintf(why, wcap, "%s", err);
    else if (!stat(partd, &st) && rename(partd, dird)) snprintf(why, wcap, "%s cannot be renamed (errno %d)", partd, errno);
    else if (rename(part, dir)) snprintf(why, wcap, "%s cannot be renamed (errno %d)", part, errno);
    else if (dm_saves_import_now(name, slot)) snprintf(why, wcap, "the import failed (saves/result.txt says why)");
    else rc = 0;
    ss_buf_free(&r.body); ss_rm_tree(part); ss_rm_tree(partd); ss_rm_tree(dir); ss_rm_tree(dird);   // (the save before it stays: saves/<stamp>-before-import, or the slot)
    if (rc) { pthread_mutex_lock(&C.mu); W.applying = 0; pthread_mutex_unlock(&C.mu); return -1; }
    ss_buf now_t; char now_sha[65] = ""; int64_t nw = 0; uint64_t nsize = 0; int64_t nfiles = 0;
    if (!snap(&now_t, now_sha, &nw, err, sizeof err)) { nsize = now_t.n; nfiles = ss_tar_files(now_t.p, now_t.n); ss_buf_free(&now_t); }
    if (strcmp(now_sha, sha)) LOG("[RED] cloud: the save imported is archived to %.12s, not the server's %.12s (a device save kept from before?): it goes up as it is now", now_sha, sha);
    pthread_mutex_lock(&C.mu);
    ss_after_restore(&C.st, seq, now_sha, sha); st_save_locked(); W.applying = 0;
    C.lknown = 1; snprintf(C.lsha, sizeof C.lsha, "%s", now_sha); C.lsize = nsize; C.lfiles = nfiles; C.lt = nw; changed_locked();
    pthread_mutex_unlock(&C.mu);
    LOG("cloud: the server's save (seq %llu, %zu bytes, from %s) is the save now%s%s", (unsigned long long)seq, (size_t)m->size, m->device[0] ? m->device : "-",
        slot ? "; this device's is in saves/" : "", slot ? slot : "");
    return 0;
}
// the meta's answer as savesync.h SS_NET_*: m read from a 200 (or a 404's body: the session, {"error":"none","session":…}, a game another device runs before its first
// upload; seq 0)
static int net_of(int got, const hres *r, ss_meta *m) {
    if (got) return SS_NET_FAIL;
    if (r->code == 200) return ss_meta_parse((const char *)r->body.p, r->body.n, m) ? SS_NET_FAIL : SS_NET_OK;
    if (r->code == 404) { ss_meta_parse((const char *)r->body.p, r->body.n, m); m->seq = 0; return SS_NET_NOTFOUND; }
    return r->code == 401 || r->code == 403 ? SS_NET_UNAUTH : SS_NET_FAIL;
}
// The start's check, on the start thread (or one the launcher asked for: recheck): this device's save against the server's meta (the early check's, else asked now),
// the questions, the download, the session. Every way out ends the check (finish): the start goes on, or the launcher does
static void start_check(JNIEnv *E, int recheck) {
    int ko = korean();
    ss_buf t; char local[65], err[600], why[300]; int64_t local_t = 0, local_files = -1; uint64_t local_size = 0;
    int lr = snap(&t, local, &local_t, err, sizeof err); if (!lr) { local_size = t.n; local_files = ss_tar_files(t.p, t.n); ss_buf_free(&t); }
    if (lr < 0) { LOG("[RED] cloud: this device's save cannot be archived (%s): this run plays without the sync", err); finish(0, 1, 0); return; }
    const char *loc = lr ? NULL : local;
    pthread_mutex_lock(&C.mu); C.lknown = 1; snprintf(C.lsha, sizeof C.lsha, "%s", loc ? loc : ""); C.lsize = loc ? local_size : 0; C.lfiles = loc ? local_files : 0; C.lt = local_t; pthread_mutex_unlock(&C.mu);
    LOG("cloud: this device's save: %s%s", loc ? loc : "none", recheck ? " (a check the launcher asked for)" : "");
    int pre = 0, pnet = SS_NET_FAIL; ss_meta pm; memset(&pm, 0, sizeof pm);
    if (!recheck) { pthread_mutex_lock(&C.mu); pre = PC.have && PC.gen == C.cfg_gen && now_ms() - PC.t < PC_FRESH_MS; if (pre) { pnet = PC.net; pm = PC.m; } PC.have = 0; pthread_mutex_unlock(&C.mu); }   // (once)
    int dev_net = pre ? -1 : dm_net_state(E, C.act), unreached = pre && pnet == SS_NET_FAIL;
    if (ss_start_offline(dev_net, unreached)) {   // no network on the device at all (a console without Wi-Fi), or the cloud did not answer the early check: no
        // question, no request that can only time out; the game starts with this device's save at once ("Offline" in the steps), and the upload thread opens the
        // session and sends what is owed once the network is back
        ss_meta m; memset(&m, 0, sizeof m);
        pthread_mutex_lock(&C.mu); int p; ss_decide(SS_NET_FAIL, &m, &C.st, loc, &p); if (p) C.st.pending = 1; C.offline = 1; C.meta_ok = 0; st_save_locked(); changed_locked(); pthread_mutex_unlock(&C.mu);
        LOG("cloud: %s: the game starts with this device's save%s; the sync goes on once it is back", dev_net == 0 ? "the device has no network" : "the cloud did not answer the early check",
            p ? " (owed: it goes up then)" : "");
        finish(0, 0, 0); return;
    }
    for (int first = 1;; first = 0) {
        wait_view("맞추는 중…", "Syncing…");
        int asked = recheck || !first;   // (a check the player asked for, or one again after a choice or a Retry)
        hres r; memset(&r, 0, sizeof r); ss_meta m; memset(&m, 0, sizeof m); m.files = -1; int net;
        if (first && pre) { m = pm; net = pnet; LOG("cloud: the early check's meta (seq %llu): not asked again", (unsigned long long)m.seq); }
        else {
            char u[1400]; url_of(u, sizeof u, "meta");
            int got = http_do(E, "GET", u, NULL, 0, NULL, 3000, now_ms() + (asked ? 5000 : PC_META_MS), 256 << 10, &r, NULL);
            if (ended()) { ss_buf_free(&r.body); break; }
            net = net_of(got, &r, &m);
            if (got) LOG("cloud: no answer from the server (%s): the game starts with this device's save%s", r.why, ss_fail_asks(asked) ? " unless the player retries" : "");
            else if (net == SS_NET_FAIL) LOG("[RED] cloud: the server answered HTTP %d%s", r.code, r.code == 200 ? " with a meta that cannot be read" : "");
            ss_buf_free(&r.body);
        }
        if (net == SS_NET_UNAUTH) {   // the token was refused: sync.txt aside, link/start once (token_refused); linked, the check again with the new token
            wait_view("기기 확인 중…", "Looking for the link…");
            if (token_refused(E, r.gen)) { if (ended()) break; continue; }
            pthread_mutex_lock(&C.mu); int p; ss_decide(net, &m, &C.st, loc, &p); if (p) C.st.pending = 1; C.st.auth = 0; if (C.state_path[0]) st_save_locked(); changed_locked(); pthread_mutex_unlock(&C.mu);
            LOG("cloud: the game starts with this device's save, without the sync (not linked: the launcher's Cloud save page links it, the next start asks again)");
            link_none();   // (the step "Not linked", no question; the start goes on)
            break;
        }
        if (net == SS_NET_FAIL) {
            pthread_mutex_lock(&C.mu); C.st.auth = 0; int p; ss_decide(net, &m, &C.st, loc, &p); if (p) C.st.pending = 1; C.offline = 1; C.meta_ok = 0; st_save_locked(); changed_locked(); pthread_mutex_unlock(&C.mu);
            if (!ss_fail_asks(asked)) { LOG("cloud: offline: the game starts with this device's save%s; the sync goes on once the server answers", p ? " (owed: it goes up then)" : ""); finish(0, 0, 0); break; }
            ask_view(DM_CLOUD_ICON_OFFLINE, ko ? "클라우드에 연결할 수 없어요" : "The cloud cannot be reached", ko ? "이 기기 세이브로 시작해요. 연결되면 나중에 올려요." : "The game starts with this device's save; it goes up once there is a connection.", NULL,
                     2, ko ? "그래도 플레이" : "Play anyway", ko ? "다시 시도" : "Retry", NULL, NULL);
            int c = ask(10000, "%d초 뒤 자동으로 시작해요", "Starting by itself in %d s");
            if (c == 1) continue;
            if (c >= 0) finish(0, 0, 0);
            break;
        }
        pthread_mutex_lock(&C.mu); C.st.auth = 0; C.offline = 0; C.meta = m; C.meta_ok = 1; C.st.checked = (int64_t)time(NULL);   // (the launcher's Cloud row, also offline later)
        you_seen_locked(m.you);   // (before another device is looked for: a session this device left under its old name is its own)
        snprintf(C.st.m_device, sizeof C.st.m_device, "%s", m.seq ? m.device : ""); C.st.m_time = m.seq ? m.time : 0; changed_locked();
        int other = ss_other_device(&m, &C.cfg, (int64_t)time(NULL)); pthread_mutex_unlock(&C.mu);
        if (other == SS_OTHER_UNSYNCED) {   // that device saved and the save did not go up: what the cloud has may be behind it
            char l0[200], when[64]; ss_fmt_date(when, sizeof when, m.s_updated, ko);
            snprintf(l0, sizeof l0, ko ? "%s에서 %s에 저장한 진행이 아직 클라우드에 없어요." : "The progress saved on %s at %s is not in the cloud yet.", m.s_device, when);
            LOG("cloud: %s has a save not yet uploaded (its last signal %s): the player is warned", m.s_device, when);
            ask_view(DM_CLOUD_ICON_WARN, ko ? "다른 기기에 아직 안 올라온 진행이 있어요" : "Another device has progress not uploaded yet", l0,
                     ko ? "그 기기를 인터넷에 연결해 켜면 올라가요." : "It goes up once that device is on and connected.",
                     2, ko ? "그래도 시작" : "Start anyway", recheck ? (ko ? "취소" : "Cancel") : (ko ? "취소 (앱 종료)" : "Cancel (close the app)"),
                     ko ? "그래도 시작하면 나중에 두 세이브 중 하나를 고르게 돼요 · B 동기화 없이 플레이" : "Start anyway, and later you choose one of the two saves · B play without the sync", NULL);
            int c = ask(0, NULL, NULL);
            if (c < 0) break;
            if (c == 1) { LOG("cloud: the player cancelled the %s (%s has a save not yet uploaded)", recheck ? "check" : "start", m.s_device); finish(!recheck, 0, 0); break; }
        } else if (other == SS_OTHER_RUNNING) {   // running there now, its saves up: a note under the steps, no choice
            pthread_mutex_lock(&C.mu); wait_locked("맞추는 중…", "Syncing…");
            snprintf(W.v.hint, sizeof W.v.hint, ko ? "%s에서 지금 실행 중이에요 · 그 기기의 진행은 끌 때 올라가요" : "Running on %s right now · its progress goes up when it closes", m.s_device);
            pthread_mutex_unlock(&C.mu); dm_cloud_screen_poke();
            LOG("cloud: the game runs on %s now (a heartbeat %lld s ago): a note only", m.s_device, (long long)((int64_t)time(NULL) - m.s_updated));
            int64_t until = now_ms() + 1500;
            pthread_mutex_lock(&C.mu);
            while (!W.done && now_ms() < until) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += 100 * 1000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
                pthread_cond_timedwait(&C.cv, &C.mu, &ts); }
            pthread_mutex_unlock(&C.mu);
            if (ended()) break;
        }
        pthread_mutex_lock(&C.mu); int pend = 0, go = ss_decide(net, &m, &C.st, loc, &pend); pthread_mutex_unlock(&C.mu);
        static const char *const GO[] = {"up to date", "this device's save goes up", "no save on either", "the server's save is newer: download", "conflict", "offline", "token refused",
                                         "played without the sync: ask", "manual, a save not sent: ask"};
        LOG("cloud: server seq %llu (%.12s, from %s), last synced seq %llu%s: %s", (unsigned long long)m.seq, m.seq ? m.sha : "-", m.device[0] ? m.device : "-", (unsigned long long)C.st.seq,
            C.st.relink ? " (back from this device only: a first connection)" : "", GO[go]);
        int retry = 0, up_now = 0; const char *slot = NULL;
        if (go == SS_GO_CONFLICT) {
            cards_view(&m, local_t, local_size, local_files);
            int c = ask(0, NULL, NULL);
            if (c < 0) break;
            if (c == 0) {   // this device's: the game starts with it and it goes up at once, over the server's (whose previous save stays in its history)
                LOG("cloud: the player chose this device's save over the server's seq %llu", (unsigned long long)m.seq);
                pthread_mutex_lock(&C.mu); if (W.done) { pthread_mutex_unlock(&C.mu); break; }
                C.st.force = 1; C.st.fbase = m.seq; C.st.pending = 1; C.st.conflict = C.st.offline_play = C.st.relink = 0; C.st.have = 1; C.kick = 1; st_save_locked(); changed_locked(); pthread_mutex_unlock(&C.mu);
            } else go = SS_GO_DOWNLOAD;
        } else if (go == SS_GO_ASK_OFFLINE) {   // AfterTest.dc.html
            after_test_view(&m, local_t, local_size);
            int c = ask(0, NULL, NULL);
            if (c < 0) break;
            if (c == 2) { LOG("cloud: the player keeps the save played without the sync: this run without the sync too"); finish(0, 1, 1); break; }
            if (c == 0) { LOG("cloud: back to the cloud's save (this device's to saves/" BACKUP_SLOT ")"); go = SS_GO_DOWNLOAD; slot = BACKUP_SLOT; }
            else {
                LOG("cloud: the player uploads the save played without the sync (the server's seq %llu stays in its history)", (unsigned long long)m.seq);
                pthread_mutex_lock(&C.mu); if (W.done) { pthread_mutex_unlock(&C.mu); break; }
                C.st.force = 1; C.st.fbase = m.seq; C.st.pending = 1; C.st.conflict = C.st.offline_play = C.st.relink = 0; C.st.have = 1; st_save_locked(); changed_locked(); pthread_mutex_unlock(&C.mu);
                up_now = 1;
            }
        } else if (go == SS_GO_ASK_MANUAL) {   // ManualPending.dc.html
            manual_view(&m, local_t, local_size);
            int c = ask(0, NULL, NULL);
            if (c < 0) break;
            pthread_mutex_lock(&C.mu); if (W.done) { pthread_mutex_unlock(&C.mu); break; }
            C.st.pending = 1; C.st.conflict = 0; C.st.have = 1; st_save_locked(); changed_locked(); pthread_mutex_unlock(&C.mu);
            if (c == 0) up_now = 1; else LOG("cloud: manual: the player keeps the save not sent (owed: Upload now sends it)");
        }
        if (go == SS_GO_DOWNLOAD) {
            int d = download_apply(E, &m, why, sizeof why, slot, 0);
            if (d == -2) break;
            if (d) {
                LOG("[RED] cloud: the server's save could not be taken: %s", why);
                ask_view(DM_CLOUD_ICON_WARN, ko ? "클라우드 세이브를 받지 못했어요" : "The cloud save could not be downloaded", ko ? "이 기기 세이브로 시작할 수 있어요." : "The game can start with this device's save.", why,
                         2, ko ? "그래도 플레이" : "Play anyway", ko ? "다시 시도" : "Retry", NULL, NULL);
                int c = ask(10000, "%d초 뒤 자동으로 시작해요", "Starting by itself in %d s");
                if (c == 1) retry = 1;
                else { if (c >= 0) finish(0, 0, 0); break; }
            }
        } else if (go != SS_GO_CONFLICT && go != SS_GO_ASK_OFFLINE && go != SS_GO_ASK_MANUAL) {
            pthread_mutex_lock(&C.mu); if (W.done) { pthread_mutex_unlock(&C.mu); break; }
            if (go == SS_GO_UPTODATE) { C.st.seq = m.seq; snprintf(C.st.sha, sizeof C.st.sha, "%s", loc ? loc : ""); C.st.pending = 0; C.st.conflict = C.st.force = 0; }
            else if (go == SS_GO_PENDING) { if (net == SS_NET_NOTFOUND) { C.st.seq = 0; C.st.force = 0; } C.st.pending = 1; C.st.conflict = 0; C.kick = 1; }
            else if (go == SS_GO_NOTHING) { C.st.seq = 0; C.st.sha[0] = 0; C.st.pending = C.st.conflict = C.st.force = 0; }
            C.st.offline_play = C.st.relink = 0; C.st.have = 1; st_save_locked(); changed_locked(); pthread_mutex_unlock(&C.mu);
        }
        if (up_now) {   // the player's choice goes up now, before the game; a 409 (the cloud moved meanwhile) asks again, with the conflict's cards
            wait_view("올리는 중…", "Uploading…"); pthread_mutex_lock(&C.mu); W.v.nbtn = 0; pthread_mutex_unlock(&C.mu);
            int up = upload(E, now_ms() + 30000);
            if (up == UP_CONFLICT) { LOG("cloud: the cloud moved meanwhile: the player chooses again"); continue; }
            if (up != UP_OK) LOG("cloud: the player's upload did not go through: it stays owed");
        }
        if (retry) continue;
        if (ended()) break;
        if (!session_post(E, SESS_OPEN, 0, now_ms() + 3000)) { pthread_mutex_lock(&C.mu); C.sess_open = 1; pthread_mutex_unlock(&C.mu); }   // (just synced: clean. A failure: the upload thread opens it)
        finish(0, 0, 0);
        break;
    }
}
static int autolink(JNIEnv *E);
enum { PLAN_WAIT = -1, PLAN_NONE, PLAN_LINK, PLAN_CHECK };   // the start's cloud: wait for the early check first; nothing to do; link/start, then the check; the check
static int begin_plan(JNIEnv *E, jobject ctx);
static void start_none(void) {   // the start's cloud ended with nothing to do (its step says why): the screen's phase ends, the start goes on
    pthread_mutex_lock(&C.mu); W.done = 1; W.active = 0; W.autolink = 0; W.up_after = 0; pthread_mutex_unlock(&C.mu);
    dm_cloud_screen_end(); dm_cloud_screen_poke();
}
static void *start_thread(void *a) {
    int plan = (int)(intptr_t)a; pthread_setname_np(pthread_self(), "cloudstart");
    JNIEnv *E = jattach();
    if (plan == PLAN_WAIT) {   // the early check still ran when the start came (a quick start: no data to get, no shaders): its outcome first
        pthread_mutex_lock(&C.mu); while (PC.running) pthread_cond_wait(&C.cv, &C.mu); pthread_mutex_unlock(&C.mu);
        plan = begin_plan(E, C.act);
        if (plan == PLAN_NONE) { start_none(); if (E) (*C.vm)->DetachCurrentThread(C.vm); return NULL; }
        pthread_mutex_lock(&C.mu); W.autolink = plan == PLAN_LINK; if (plan == PLAN_CHECK) wait_locked("맞추는 중…", "Syncing…"); else W.v.nbtn = 0; pthread_mutex_unlock(&C.mu);
        dm_cloud_screen_poke();
    }
    int al = plan == PLAN_LINK;
    if (!E) { LOG("[RED] cloud: no Java for the sync: this run plays without it"); if (al) link_none(); else finish(0, 1, 0); return NULL; }
    if (al) { int r = autolink(E); if (r <= 0) { if (!r) link_none(); (*C.vm)->DetachCurrentThread(C.vm); return NULL; } }
    start_check(E, 0);
    (*C.vm)->DetachCurrentThread(C.vm);
    return NULL;
}

// The start screen's input (front_android.c on_input_early, the main thread): the d-pad or a stick's hat moves between the buttons or cards, A (or Enter) chooses, B (or Back)
// plays without the sync; a touch chooses what it lifts on. The volume keys stay the system's
static void input_choose(int i) {
    pthread_mutex_lock(&C.mu);
    if (W.done || W.applying || W.autolink) { pthread_mutex_unlock(&C.mu); return; }
    int n = W.v.ncards ? W.v.ncards : W.v.nbtn;
    if (i < 0 || i >= n) { pthread_mutex_unlock(&C.mu); return; }
    W.v.sel = i; W.v.ver++; W.last_input = now_ms();
    if (W.kind == WV_WAIT) { finish_locked(0, 1); pthread_mutex_unlock(&C.mu); skipped_end(); return; }   // (the waiting step's only button: play without the sync)
    W.choice = i; pthread_cond_broadcast(&C.cv); pthread_mutex_unlock(&C.mu); dm_cloud_screen_poke();
}
static void input_move(int d) {
    pthread_mutex_lock(&C.mu); int n = W.v.ncards ? W.v.ncards : W.v.nbtn;
    if (!W.done && n > 1) { W.v.sel = (W.v.sel + d + n) % n; W.v.ver++; }
    W.last_input = now_ms(); pthread_mutex_unlock(&C.mu); dm_cloud_screen_poke();
}
static void input_back(void) {
    pthread_mutex_lock(&C.mu); int ok = !W.done && !W.applying && !W.autolink; if (ok) finish_locked(0, 1); pthread_mutex_unlock(&C.mu);
    if (ok) skipped_end();
}
// The question's layout decides B: a list's is its last button (Keep it), a pair's its second (B · Keep it), else play without the sync; a pair's A is its first
int dm_cloud_input(const AInputEvent *e) {
    pthread_mutex_lock(&C.mu); int on = W.active && !W.done, style = W.v.style; pthread_mutex_unlock(&C.mu);
    if (!on) return 0;
    if (AInputEvent_getType(e) == AINPUT_EVENT_TYPE_KEY) {
        int k = AKeyEvent_getKeyCode(e), act = AKeyEvent_getAction(e);
        if (k == AKEYCODE_VOLUME_UP || k == AKEYCODE_VOLUME_DOWN || k == AKEYCODE_VOLUME_MUTE) return 0;
        if (act != AKEY_EVENT_ACTION_DOWN || AKeyEvent_getRepeatCount(e)) return 1;
        if (k == AKEYCODE_DPAD_LEFT || k == AKEYCODE_DPAD_UP) { if (style != DM_CLOUD_STYLE_PAIR) input_move(-1); }
        else if (k == AKEYCODE_DPAD_RIGHT || k == AKEYCODE_DPAD_DOWN) { if (style != DM_CLOUD_STYLE_PAIR) input_move(1); }
        else if (k == AKEYCODE_BUTTON_A || k == AKEYCODE_DPAD_CENTER || k == AKEYCODE_ENTER || k == AKEYCODE_NUMPAD_ENTER || k == AKEYCODE_BUTTON_START) {
            pthread_mutex_lock(&C.mu); int s = style == DM_CLOUD_STYLE_PAIR ? 0 : W.v.sel; pthread_mutex_unlock(&C.mu); input_choose(s); }
        else if (k == AKEYCODE_BUTTON_B || k == AKEYCODE_BACK || k == AKEYCODE_ESCAPE) {
            if (style == DM_CLOUD_STYLE_LIST) input_choose(2); else if (style == DM_CLOUD_STYLE_PAIR) input_choose(1); else input_back(); }
        return 1;
    }
    if (AInputEvent_getType(e) != AINPUT_EVENT_TYPE_MOTION) return 1;
    int src = AInputEvent_getSource(e), act = AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_MASK;
    if ((src & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK) {   // the hat (a d-pad as axes) and the left stick: one step per push
        static int held; float x = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_X, 0), y = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_Y, 0);
        float sx = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_X, 0); if (fabsf(x) < 0.5f && fabsf(y) < 0.5f && fabsf(sx) > 0.7f) x = sx;
        int d = x < -0.5f || y < -0.5f ? -1 : x > 0.5f || y > 0.5f ? 1 : 0;
        if (d && d != held && style != DM_CLOUD_STYLE_PAIR) input_move(d);
        held = d; return 1;
    }
    if (act == AMOTION_EVENT_ACTION_UP) {
        int kind = 0, i = dm_cloud_screen_hit(AMotionEvent_getX(e, 0), AMotionEvent_getY(e, 0), &kind);
        if (i >= 0) input_choose(i);
    }
    return 1;
}

// ---- the uploads ----
static jobject g_wl;   // the PowerManager.WakeLock (a global reference), made at the first pause
static void wake_lock(JNIEnv *E, int on, int ms) {   // a failure (no WAKE_LOCK permission in an old APK) leaves the upload to go without it
    if (!g_wl && on) {
        if ((*E)->PushLocalFrame(E, 16)) { jx(E, NULL, 0); return; }
        char why[200] = "";
        jclass cA = (*E)->GetObjectClass(E, C.act); jmethodID mS = cA ? (*E)->GetMethodID(E, cA, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;") : NULL;
        jobject pm = mS && !jx(E, why, sizeof why) ? (*E)->CallObjectMethod(E, C.act, mS, (*E)->NewStringUTF(E, "power")) : NULL;
        jclass cP = pm && !jx(E, why, sizeof why) ? (*E)->GetObjectClass(E, pm) : NULL;
        jmethodID mN = cP ? (*E)->GetMethodID(E, cP, "newWakeLock", "(ILjava/lang/String;)Landroid/os/PowerManager$WakeLock;") : NULL;
        jobject wl = mN && !jx(E, why, sizeof why) ? (*E)->CallObjectMethod(E, pm, mN, (jint)1 /* PARTIAL_WAKE_LOCK */, (*E)->NewStringUTF(E, "nx2a:savesync")) : NULL;
        if (!jx(E, why, sizeof why) && wl) {
            jclass cW = (*E)->GetObjectClass(E, wl); jmethodID mR = cW ? (*E)->GetMethodID(E, cW, "setReferenceCounted", "(Z)V") : NULL;
            if (mR) (*E)->CallVoidMethod(E, wl, mR, JNI_FALSE);
            if (!jx(E, why, sizeof why)) g_wl = (*E)->NewGlobalRef(E, wl);
        }
        if (!g_wl) LOG("[RED] cloud: no wake lock (%s): the upload goes without it", why);
        (*E)->PopLocalFrame(E, NULL);
    }
    if (!g_wl) return;
    jclass cW = (*E)->GetObjectClass(E, g_wl); jmethodID m = cW ? (on ? (*E)->GetMethodID(E, cW, "acquire", "(J)V") : (*E)->GetMethodID(E, cW, "release", "()V")) : NULL;
    if (m) { if (on) (*E)->CallVoidMethod(E, g_wl, m, (jlong)ms); else (*E)->CallVoidMethod(E, g_wl, m); }
    if (cW) (*E)->DeleteLocalRef(E, cW);
    char why[200] = ""; if (jx(E, why, sizeof why)) LOG("[RED] cloud: the wake lock's %s failed (%s)", on ? "acquire" : "release", why);
}
static int upload_now(JNIEnv *E, int64_t deadline);
static int upload(JNIEnv *E, int64_t deadline) { dm_perf_bg_mark(1); int r = upload_now(E, deadline); dm_perf_bg_mark(-1); return r; }
static int upload_now(JNIEnv *E, int64_t deadline) {
    static const int64_t BACKOFF[] = {60000, 120000, 300000, 600000};
    pthread_mutex_lock(&C.mu);
    int skip = C.mode != SS_RUN_SYNC || C.skipped || C.st.mode == SS_MODE_LOCAL || C.st.conflict || C.st.auth || C.st.offline_play, force = C.st.force; uint64_t base = force ? C.st.fbase : C.st.seq; char last[65];
    snprintf(last, sizeof last, "%s", C.st.sha); pthread_mutex_unlock(&C.mu);
    if (skip) return UP_NOTHING;
    ss_buf t; char sha[65], err[600]; int64_t nw;
    int r = snap(&t, sha, &nw, err, sizeof err);
    if (r == 1 || (!r && !force && !strcmp(sha, last))) {   // nothing to send
        if (!r) ss_buf_free(&t);
        pthread_mutex_lock(&C.mu); if (C.st.pending) { C.st.pending = 0; st_save_locked(); changed_locked(); } pthread_mutex_unlock(&C.mu); return UP_NOTHING;
    }
    if (r) { LOG("[RED] cloud: the save cannot be archived for the upload: %s", err); pthread_mutex_lock(&C.mu); C.st.pending = 1; C.next_try = now_ms() + BACKOFF[3]; changed_locked(); pthread_mutex_unlock(&C.mu); return UP_ERR; }
    int64_t files = ss_tar_files(t.p, t.n);   // (the server keeps it beside the size: another device's conflict card shows both)
    pthread_mutex_lock(&C.mu); C.st.pending = 1; C.uploading = 1; ss_up_begin(&C.notes, now_ms()); changed_locked(); pthread_mutex_unlock(&C.mu);   // the spinner
    int rc = UP_ERR, note = SS_NOTE_NONE, zeroed = 0, relinked = 0;
    for (;;) {   // again: once with base 0 (the server has no save any more), once with a new token (the old one refused: token_refused)
        char u[1700], dev[200], gv[100], bd[100], fc[32] = ""; ss_qenc(gv, sizeof gv, C.gver); ss_qenc(bd, sizeof bd, C.build);
        if (files >= 0) snprintf(fc, sizeof fc, "&files=%lld", (long long)files);
        pthread_mutex_lock(&C.mu); ss_qenc(dev, sizeof dev, C.cfg.device);
        snprintf(u, sizeof u, "%s/data?base=%llu&device=%s&gameVersion=%s&build=%s%s&you=1", C.base, (unsigned long long)base, dev, gv, bd, fc); pthread_mutex_unlock(&C.mu);
        hres h; int got = http_do(E, "PUT", u, t.p, t.n, sha, 5000, deadline, 256 << 10, &h, NULL);
        ss_meta m; memset(&m, 0, sizeof m);
        pthread_mutex_lock(&C.mu);
        if (got) {
            int i = C.fails < 3 ? C.fails : 3; C.fails++; C.next_try = now_ms() + BACKOFF[i];
            note = ss_after_put(&C.st, 0, 0, sha, NULL, 0);
            C.offline = 1; rc = UP_FAIL;
            LOG("cloud: the upload did not go through (%s): tried again in %lld min, or at once when the network comes back", h.why, (long long)(BACKOFF[i] / 60000));
        } else if (h.code == 200) {
            uint64_t seq = !ss_meta_parse((const char *)h.body.p, h.body.n, &m) ? m.seq : h.seq[0] ? strtoull(h.seq, NULL, 10) : 0;
            if (!seq) LOG("[RED] cloud: the upload's answer has no seq: the next start asks the server again");
            { char yn[64]; if (ss_you_name((const char *)h.body.p, h.body.n, yn, sizeof yn)) you_seen_locked(yn); }   // (the server recorded the upload under it)
            note = ss_after_put(&C.st, 200, seq, sha, NULL, (int64_t)time(NULL));
            C.fails = 0; C.offline = 0; rc = UP_OK;
            snprintf(C.st.m_device, sizeof C.st.m_device, "%s", C.cfg.device); C.st.m_time = C.st.last_up;   // (the launcher's Cloud row)
            C.lknown = 1; snprintf(C.lsha, sizeof C.lsha, "%s", sha); C.lsize = t.n; C.lfiles = files;
            if (C.meta_ok) { C.meta.seq = seq ? seq : C.meta.seq; snprintf(C.meta.sha, sizeof C.meta.sha, "%s", sha); snprintf(C.meta.device, sizeof C.meta.device, "%s", C.cfg.device);
                             C.meta.time = C.st.last_up; C.meta.size = t.n; C.meta.files = files; }
            LOG("cloud: the save went up: seq %llu, %zu bytes, %lld files, %.12s", (unsigned long long)seq, t.n, (long long)files, sha);
        } else if (h.code == 409) {
            int k = ss_conflict_parse((const char *)h.body.p, h.body.n, &m);
            if (k == 0 && base && !zeroed) {   // the server has no save (it was cleared): again with base 0
                LOG("cloud: the server has no save any more: sent again as the first one"); base = 0; C.st.seq = 0; C.st.force = 0; zeroed = 1;
                pthread_mutex_unlock(&C.mu); ss_buf_free(&h.body); continue;
            }
            note = ss_after_put(&C.st, 409, 0, sha, &m, 0); C.offline = 0; rc = UP_CONFLICT;   // never overwritten, never asked while the game runs: the next start's cards
            LOG("[RED] cloud: the server moved on (seq %llu from %s, this device's base %llu): a conflict, the next start asks which save to keep", (unsigned long long)m.seq, m.device[0] ? m.device : "-", (unsigned long long)base);
        } else if (ss_refused(h.code)) {   // sync.txt aside, link/start once (quietly, the game may run): linked, the same save again with the new token
            pthread_mutex_unlock(&C.mu); ss_buf_free(&h.body);
            int k = token_refused(E, h.gen);   // (a second refusal, the new token's: not linked now)
            pthread_mutex_lock(&C.mu);
            if (k && !relinked && C.mode == SS_RUN_SYNC) { relinked = 1; pthread_mutex_unlock(&C.mu); LOG("cloud: the upload goes again with the new token"); continue; }
            C.st.pending = 1; rc = UP_ERR;   // (still owed: it goes up once this device is linked again)
        }
        else { C.next_try = now_ms() + (h.code == 413 ? 24ll * 3600 * 1000 : BACKOFF[3]); rc = UP_ERR;
               LOG("[RED] cloud: the upload was refused: HTTP %d%s", h.code, h.code == 400 ? " (the hash did not match)" : h.code == 413 ? " (the save is too big for the server)" : ""); }
        C.uploading = 0; ss_up_end(&C.notes, note, now_ms()); st_save_locked(); changed_locked();   // its check (once no upload follows within 3 s), or its note
        pthread_mutex_unlock(&C.mu); ss_buf_free(&h.body);
        break;
    }
    pthread_mutex_lock(&C.mu); C.uploading = 0; pthread_mutex_unlock(&C.mu);
    ss_buf_free(&t);
    return rc;
}
// The app leaves the screen: what is owed goes up now, then the session closes with what is still owed; under a 15 s wake lock
static void check_owed(void);
static void pause_flush(JNIEnv *E, int *dirty, int syncs, const ss_tick_in *ti) {
    int self = ss_uploads(ti->mode, SS_EV_LEAVE);
    if (syncs && !self && *dirty) { *dirty = 0; check_owed(); }   // manual: what the game saved is only looked at (the session closes dirty when it is owed)
    // (automatic: the tick before made each commit owed, so ti->owed has it)
    int64_t dl = now_ms() + 15000;
    if ((!syncs && !ti->sess_open) || dm_net_state(E, C.act) == 0) { if (syncs) LOG("cloud: the app leaves the screen without a network: what is owed goes up once it is back"); return; }
    wake_lock(E, 1, 15000);
    if (ss_leave_uploads(ti)) { *dirty = 0; upload(E, dl - 1000); }
    int owed = owed_now();
    if (owed) LOG("cloud: a save is still owed (it could not go up): the session closes dirty, another device warns before it starts");
    session_post(E, SESS_CLOSED, owed, dl);   // (no answer: the session's last signal stands, and stops being "running" within 3 min)
    pthread_mutex_lock(&C.mu); C.sess_open = 0; pthread_mutex_unlock(&C.mu);
    wake_lock(E, 0, 0);
}
// manual: whether the save the game committed is the one the cloud has (owed or not), for the launcher's page and the corner's mark: archived and hashed, nothing sent
static void check_owed(void) {
    ss_buf t; char sha[65], err[600]; int64_t nw; dm_perf_bg_mark(1); int r = snap(&t, sha, &nw, err, sizeof err); dm_perf_bg_mark(-1); if (!r) ss_buf_free(&t);
    if (r < 0) { LOG("[RED] cloud: the save cannot be archived to compare (%s)", err); return; }
    pthread_mutex_lock(&C.mu);
    int owed = C.st.conflict || C.st.force || (r == 0 ? strcmp(sha, C.st.sha) != 0 : C.st.sha[0] != 0);
    if (owed != C.st.pending) { C.st.pending = owed; st_save_locked(); changed_locked(); LOG("cloud: manual: %s", owed ? "a save not sent (Upload now sends it)" : "the save is the cloud's again"); }
    pthread_mutex_unlock(&C.mu);
}
// The upload thread: the commit count polled every 1.5 s. Each tick asks savesync.h ss_tick what to do: automatic, a commit only makes the save owed (the heartbeat
// says dirty at once) and it goes up when the app leaves the screen, or once it stayed owed SS_SAFETY_UPLOAD_SECS; manual, a burst of commits is hashed (owed or not).
// With no network on the device nothing is tried; when it comes back the backoff is forgotten (a save the safety net made due goes up then) and a session that never
// opened (an offline start) opens, then the heartbeats every 60 s
static void *up_thread(void *a) {
    (void)a; pthread_setname_np(pthread_self(), "savesync");
    JNIEnv *E = jattach(); if (!E) { LOG("[RED] cloud: no Java for the uploads: the save does not go up this run"); return NULL; }
    // it lives as long as the game plays: below the game's threads, on the slowest cores (after the attach, which makes it a Java thread too)
    dm_perf_background_thread("cloud upload (savesync)");
    int safety_s = ss_safety_secs(getenv("DM_SAVESYNC_SAFETY_SECS"));
    // dirty_sent: the start opened the session clean (dirty 0), if it could; owed_since: when the save became owed with nothing gone up since (the safety net's clock)
    uint64_t lastc = dm_fs_commit_count(); int dirty = 0, paused = 0, was_net = -1, dirty_sent = 0; int64_t changed_t = 0, last_hb = now_ms(), owed_since = 0;
    LOG("cloud: uploads on (automatic: when the app leaves the screen or ends, and %d s after the save changed while the game runs; a commit itself sends nothing); a heartbeat"
        " every 60 s while on screen, and at once when a save becomes owed", safety_s);
    for (;;) {
        pthread_mutex_lock(&C.mu);
        if (!C.kick && !C.pause_ev && !C.resume_ev) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += 500 * 1000000; ts.tv_sec += 1; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&C.cv, &C.mu, &ts); }   // 1.5 s
        int kick = C.kick, pause = C.pause_ev, resume = C.resume_ev; C.kick = C.pause_ev = C.resume_ev = 0;
        int syncs = C.mode == SS_RUN_SYNC && !C.skipped && C.st.mode != SS_MODE_LOCAL && !C.st.auth && !C.st.offline_play;
        uint64_t c = dm_fs_commit_count(); int64_t t = now_ms();
        if (c != lastc) { lastc = c; dirty = 1; changed_t = t; }
        if (syncs && dirty && ss_uploads(C.st.mode, SS_EV_SAFETY)) {   // automatic: the commit is owed, not sent (it goes up when the app leaves the screen, or by the safety net)
            dirty = 0; if (!C.st.pending) { C.st.pending = 1; st_save_locked(); changed_locked(); }
        }
        if (!C.st.pending) owed_since = 0; else if (!owed_since) owed_since = t;
        ss_tick_in ti = {.syncs = syncs, .mode = C.st.mode, .sess_open = C.sess_open, .owed = C.st.pending, .conflict = C.st.conflict, .next_try = C.next_try,
                         .dirty_sent = dirty_sent, .owed_since = owed_since, .safety_ms = (int64_t)safety_s * 1000};
        pthread_mutex_unlock(&C.mu);
        if (pause) {   // the app leaves the screen (sleep, HOME, its end, the menu's Exit game): what is owed goes up now, then the session closes (pause_flush)
            paused = 1; pause_flush(E, &dirty, syncs, &ti);
            pthread_mutex_lock(&C.mu); C.pause_done++; pthread_cond_broadcast(&C.cv); pthread_mutex_unlock(&C.mu);   // (dm_cloud_flush waits for it)
            continue;
        }
        if (resume) paused = 0;
        if (paused) continue;
        ti.kick = kick; ti.committed = dirty && t - changed_t >= 1500; ti.now = t; ti.last_hb = last_hb;
        // the device's network is read only when this tick may send something or it was down (a save owed but not due yet, online, with its session open asks Java
        // nothing between its heartbeats; manual sends only what is asked for)
        int due = ss_safety_due(&ti), dirty_now = ti.owed || ti.conflict;
        int ask_net = syncs && (kick || due || dirty_now != dirty_sent || !ti.sess_open || resume || was_net == 0 || t - last_hb >= 60000);
        ti.net = ask_net ? dm_net_state(E, C.act) : syncs ? was_net : -1; ti.was_net = was_net; if (ask_net) was_net = ti.net;
        if (resume && ti.net != 0) ti.next_try = 0;   // back in front: the backoff forgotten (a save the safety net made due is tried now)
        int act = ss_tick(&ti);
        if (act & SS_DO_RETRY_NOW) { LOG("cloud: the network is back%s", due ? ": what is owed goes up now" : ti.owed ? " (what is owed goes up when the app leaves the screen, or by the safety net)" : "");
                                     pthread_mutex_lock(&C.mu); C.next_try = 0; C.fails = 0; pthread_mutex_unlock(&C.mu); }
        int up = UP_NOTHING;
        if (act & SS_DO_UPLOAD) { dirty = 0; if (due && !kick) LOG("cloud: the save stayed owed %d s while the game runs: the safety net sends it", safety_s); up = upload(E, now_ms() + 60000); }
        else if (act & SS_DO_CHECK) { dirty = 0; check_owed(); }
        if ((act & SS_DO_OPEN) || resume) {   // an offline start, or the app back in front: the session opens (dirty while a save is owed)
            int d = owed_now();
            if (syncs && ti.net != 0) { if (!session_post(E, SESS_OPEN, d, now_ms() + 5000)) { pthread_mutex_lock(&C.mu); C.sess_open = 1; pthread_mutex_unlock(&C.mu); } dirty_sent = d; }
            last_hb = now_ms();
        } else if ((act & SS_DO_ALIVE) || (up == UP_OK && ti.sess_open)) { int d = owed_now(); session_post(E, SESS_ALIVE, d, now_ms() + 5000); dirty_sent = d; last_hb = now_ms(); }
    }
    return NULL;
}
static void up_start(void) {
    pthread_mutex_lock(&C.mu); int go = C.mode == SS_RUN_SYNC && !C.skipped && !C.up_started; if (go) C.up_started = 1; pthread_mutex_unlock(&C.mu);
    if (!go) return;
    pthread_t t; if (pthread_create(&t, NULL, up_thread, NULL)) LOG("[RED] cloud: cannot start the upload thread"); else pthread_detach(t);
}
void dm_cloud_pause(void) { pthread_mutex_lock(&C.mu); if (C.up_started) { C.pause_ev = 1; pthread_cond_broadcast(&C.cv); } pthread_mutex_unlock(&C.mu); }
// The menu's Exit game (front_android.c menu_exit_now): what onPause starts, done now and waited for, at most ms (onDestroy's _exit would cut it off). 0: done (or
// no sync this run), 1: still going when ms ran out
int dm_cloud_flush(int ms) {
    pthread_mutex_lock(&C.mu);
    if (!C.up_started) { pthread_mutex_unlock(&C.mu); return 0; }
    unsigned gen = C.pause_done; C.pause_ev = 1; pthread_cond_broadcast(&C.cv);
    struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl); dl.tv_sec += ms / 1000; dl.tv_nsec += (long)(ms % 1000) * 1000000; if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; }
    int r = 0; while (C.pause_done == gen) if (pthread_cond_timedwait(&C.cv, &C.mu, &dl) == ETIMEDOUT) { r = 1; break; }
    pthread_mutex_unlock(&C.mu);
    LOG("cloud: the save before the app ends: %s", r ? "not done within the time (what is owed goes up at the next start)" : "done");
    return r;
}
void dm_cloud_resume(void) { pthread_mutex_lock(&C.mu); if (C.up_started) { C.resume_ev = 1; C.next_try = 0; pthread_cond_broadcast(&C.cv); } pthread_mutex_unlock(&C.mu); }

// ---- the corner mark ----
// The game's corner mark (gfx_vk_present.c ov_mark, at each present): the spinner from an upload's start (the upload thread's ss_up_begin) on while uploads follow
// each other, then the check, or the waiting or conflict note (savesync.h ss_mark_now). None while the app leaves the screen. NONE: nothing is drawn. *age: ms since
// the mark began
int dm_cloud_mark(int64_t *age) {
    pthread_mutex_lock(&C.mu); int64_t a = 0; int k = ss_mark_now(&C.notes, now_ms(), &a);
    if (k == SS_MARK_BUSY && C.pause_ev) k = DM_CLOUD_MARK_NONE;
    if (k == DM_CLOUD_MARK_NONE && C.up_started && !C.pause_ev && C.st.mode == SS_MODE_MANUAL && C.st.pending) { k = DM_CLOUD_MARK_OFFLINE; a = 250; }   // manual, a save not sent: the
    // struck-through cloud stays (an age past its pop and before its fade)
    pthread_mutex_unlock(&C.mu); *age = a; return k;
}

// ---- the start ----
static int jstr(JNIEnv *E, jobject o, jmethodID m, char *out, size_t cap) {   // a String method's answer into out
    jstring s = m ? (*E)->CallObjectMethod(E, o, m) : NULL; if (jx(E, NULL, 0) || !s) return -1;
    const char *u = (*E)->GetStringUTFChars(E, s, NULL); if (!u) return -1;
    snprintf(out, cap, "%s", u); (*E)->ReleaseStringUTFChars(E, s, u); return 0;
}
static void app_info(ANativeActivity *act, int *debuggable) {   // the package's name and versionName, and whether it is debuggable (ApplicationInfo.FLAG_DEBUGGABLE)
    JNIEnv *E = act->env; *debuggable = 0;
    if ((*E)->PushLocalFrame(E, 32)) { jx(E, NULL, 0); return; }
    jclass cA = (*E)->GetObjectClass(E, act->clazz);
    jstr(E, act->clazz, cA ? (*E)->GetMethodID(E, cA, "getPackageName", "()Ljava/lang/String;") : NULL, C.pkg, sizeof C.pkg);
    jmethodID mAi = cA ? (*E)->GetMethodID(E, cA, "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;") : NULL;
    jobject ai = mAi && !jx(E, NULL, 0) ? (*E)->CallObjectMethod(E, act->clazz, mAi) : NULL;
    jfieldID fF = ai && !jx(E, NULL, 0) ? (*E)->GetFieldID(E, (*E)->GetObjectClass(E, ai), "flags", "I") : NULL;
    if (fF && !jx(E, NULL, 0)) *debuggable = ((*E)->GetIntField(E, ai, fF) & 2) != 0;
    jmethodID mPm = cA ? (*E)->GetMethodID(E, cA, "getPackageManager", "()Landroid/content/pm/PackageManager;") : NULL;
    jobject pm = mPm && !jx(E, NULL, 0) ? (*E)->CallObjectMethod(E, act->clazz, mPm) : NULL;
    jmethodID mPi = pm && !jx(E, NULL, 0) ? (*E)->GetMethodID(E, (*E)->GetObjectClass(E, pm), "getPackageInfo", "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;") : NULL;
    jobject pi = mPi && !jx(E, NULL, 0) ? (*E)->CallObjectMethod(E, pm, mPi, (*E)->NewStringUTF(E, C.pkg), 0) : NULL;
    jfieldID fV = pi && !jx(E, NULL, 0) ? (*E)->GetFieldID(E, (*E)->GetObjectClass(E, pi), "versionName", "Ljava/lang/String;") : NULL;
    jstring vn = fV && !jx(E, NULL, 0) ? (*E)->GetObjectField(E, pi, fV) : NULL;
    const char *u = vn && !jx(E, NULL, 0) ? (*E)->GetStringUTFChars(E, vn, NULL) : NULL; if (u) { snprintf(C.build, sizeof C.build, "%s", u); (*E)->ReleaseStringUTFChars(E, vn, u); }
    jx(E, NULL, 0);
    jmethodID mL = ai && pm ? (*E)->GetMethodID(E, (*E)->GetObjectClass(E, ai), "loadLabel", "(Landroid/content/pm/PackageManager;)Ljava/lang/CharSequence;") : NULL;   // the app's label (the link's game)
    jobject cs = mL && !jx(E, NULL, 0) ? (*E)->CallObjectMethod(E, ai, mL, pm) : NULL;
    jmethodID mS = cs && !jx(E, NULL, 0) ? (*E)->GetMethodID(E, (*E)->GetObjectClass(E, cs), "toString", "()Ljava/lang/String;") : NULL;
    if (mS && !jx(E, NULL, 0)) jstr(E, cs, mS, C.label, sizeof C.label);
    jx(E, NULL, 0); (*E)->PopLocalFrame(E, NULL);
}
// Settings.Secure.getString(getContentResolver(), "android_id") through JNI (framework classes only: an APK without a dex of its own has them too); 0 with the ID in out
static int android_id(JNIEnv *E, jobject act, char *out, size_t cap) {
    out[0] = 0; if ((*E)->PushLocalFrame(E, 16)) { jx(E, NULL, 0); return -1; }
    jclass cA = (*E)->GetObjectClass(E, act); jmethodID mR = cA ? (*E)->GetMethodID(E, cA, "getContentResolver", "()Landroid/content/ContentResolver;") : NULL;
    jobject cr = mR && !jx(E, NULL, 0) ? (*E)->CallObjectMethod(E, act, mR) : NULL;
    jclass cS = cr && !jx(E, NULL, 0) ? (*E)->FindClass(E, "android/provider/Settings$Secure") : NULL;
    jmethodID mG = cS && !jx(E, NULL, 0) ? (*E)->GetStaticMethodID(E, cS, "getString", "(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;") : NULL;
    jstring k = mG && !jx(E, NULL, 0) ? (*E)->NewStringUTF(E, "android_id") : NULL;
    jstring v = k ? (*E)->CallStaticObjectMethod(E, cS, mG, cr, k) : NULL;
    const char *u = v && !jx(E, NULL, 0) ? (*E)->GetStringUTFChars(E, v, NULL) : NULL; if (u) { snprintf(out, cap, "%s", u); (*E)->ReleaseStringUTFChars(E, v, u); }
    jx(E, NULL, 0); (*E)->PopLocalFrame(E, NULL);
    return out[0] ? 0 : -1;
}
// the server (savesync.h ss_link_server): sync.txt's url= while it is read, else env.txt's DM_SAVESYNC_URL, else the APK's assets/nx2a/savecloud.txt. 0, -1 none ("")
static char g_env_url[600];   // (env.txt's DM_SAVESYNC_URL read at onCreate: env_early)
static int link_base_locked(char *o, size_t n) { const char *e = getenv("DM_SAVESYNC_URL"); return ss_link_server(o, n, C.began ? C.cfg.url : NULL, e && *e ? e : g_env_url, C.srv_asset); }
static int link_base(char *o, size_t n) { pthread_mutex_lock(&C.mu); int r = link_base_locked(o, n); pthread_mutex_unlock(&C.mu); return r; }
// assets/nx2a/savecloud.txt (url=: the cloud save server the porter's savecloud.txt named), read once: without it (and without env.txt's DM_SAVESYNC_URL or a sync.txt)
// the app has no cloud save server, and the cloud save is off (no link asked, the launcher says so). AAssetManager only: no Java, any thread
static void server_asset_load(ANativeActivity *act) {
    pthread_mutex_lock(&C.mu); int done = C.srv_read; pthread_mutex_unlock(&C.mu);
    if (done || !act || !act->assetManager) return;
    char t[700] = ""; AAsset *as = AAssetManager_open(act->assetManager, "nx2a/savecloud.txt", AASSET_MODE_BUFFER);
    if (as) { int k = AAsset_read(as, t, sizeof t - 1); t[k > 0 ? k : 0] = 0; AAsset_close(as); }
    pthread_mutex_lock(&C.mu); if (!C.srv_read) { snprintf(C.srv_asset, sizeof C.srv_asset, "%s", t); C.srv_read = 1; } pthread_mutex_unlock(&C.mu);
}
int dm_cloud_no_server(ANativeActivity *act) {
    server_asset_load(act); char b[600]; pthread_mutex_lock(&C.mu); int none = link_base_locked(b, sizeof b) != 0; pthread_mutex_unlock(&C.mu); return none;
}
static jobject app_context(ANativeActivity *act) {   // the application's Context, a global reference (none: the activity's own, kept)
    JNIEnv *E = act->env; jobject r = NULL;
    jclass c = (*E)->GetObjectClass(E, act->clazz); jmethodID m = c ? (*E)->GetMethodID(E, c, "getApplicationContext", "()Landroid/content/Context;") : NULL;
    if (m && !(*E)->ExceptionCheck(E)) r = (*E)->CallObjectMethod(E, act->clazz, m);
    if ((*E)->ExceptionCheck(E)) { (*E)->ExceptionClear(E); r = NULL; }
    jobject g = (*E)->NewGlobalRef(E, r ? r : act->clazz);
    if (r) (*E)->DeleteLocalRef(E, r);
    if (c) (*E)->DeleteLocalRef(E, c);
    return g;
}
void dm_cloud_attach(ANativeActivity *act) {
    if (C.attached) return;
    server_asset_load(act);
    C.vm = act->vm; C.act = app_context(act);   // (not the activity: the background download can keep the process past it, datasync_android.c dm_sync_detach)
    int dbg = 0; app_info(act, &dbg);
    AAsset *as = act->assetManager ? AAssetManager_open(act->assetManager, "nx2a/source.bin", AASSET_MODE_UNKNOWN) : NULL; if (as) AAsset_close(as);
    char id[128] = "", br[PROP_VALUE_MAX] = "", mf[PROP_VALUE_MAX] = "", md[PROP_VALUE_MAX] = "";
    int have_id = !android_id(act->env, act->clazz, id, sizeof id);
    __system_property_get("ro.product.brand", br); __system_property_get("ro.product.manufacturer", mf); __system_property_get("ro.product.model", md);
    pthread_mutex_lock(&C.mu);
    C.trial_base = !as || dbg; if (have_id) ss_link_key(id, C.key); else C.key[0] = 0;
    ss_link_model(C.model, sizeof C.model, br, mf, md); if (!C.label[0]) snprintf(C.label, sizeof C.label, "%s", C.pkg);
    C.attached = 1; pthread_mutex_unlock(&C.mu);
    memset(id, 0, sizeof id);
    if (!have_id) LOG("[RED] cloud: the device's ANDROID_ID cannot be read: no device link on this device");
    else LOG("cloud: device link key %.6s... (%s, %s)", C.key, C.model, C.trial_base ? "a trial build: no link" : "the link can run");
    if (dm_cloud_no_server(act)) LOG("cloud: no cloud save server (no assets/nx2a/savecloud.txt, no DM_SAVESYNC_URL): the cloud save is off unless a sync.txt names one");
}
// sync.txt read into C.cfg, with the state it goes with (C.mu held): 1 when the sync is on. The start's (dm_cloud_begin) or, after it, a link's (link_done)
static int cfg_load_locked(void) {
    const char *root = home_or_null(); char p[700]; size_t n = 0;
    if (!root) return 0;
    snprintf(p, sizeof p, "%s/sync.txt", home_dir()); char *txt = ds_read(p, &n, 1 << 16);
    if (!txt) return 0;   // no sync.txt: as before the sync existed (or a link to come)
    int on = ss_cfg_text(txt, &C.cfg); memset(txt, 0, n); free(txt); C.cfg_gen++;
    if (!on) { LOG("[RED] cloud: %s has no url= (http:// or https://) and token=: the sync is off", p); return 0; }
    snprintf(C.state_path, sizeof C.state_path, "%s/sync-state.txt", dm_saves_dir());
    char pk[600]; ss_qenc(pk, sizeof pk, C.pkg); snprintf(C.base, sizeof C.base, "%s/api/saves/g/%s", C.cfg.url, pk);
    ss_state_load(C.state_path, &C.st); C.began = 1;
    C.mode = ss_run_mode(1, C.trial, C.st.mode == SS_MODE_LOCAL);
    LOG("cloud: %s, device %s, token %.4s... (%s), package %s, build %s%s", C.cfg.url, C.cfg.device, C.cfg.token, C.st.have ? "state kept" : "first setup", C.pkg, C.build,
        C.mode == SS_RUN_SYNC ? "" : C.trial ? (C.trial_base ? ": a trial build (no data server, or debuggable): no sync this run" : ": DM_SAVESYNC=off: no sync this run")
        : ": this device only (the game's mode)");
    return 1;
}
// The start's cloud once the early check ended (C.mu not held; E, ctx: dm_net_state's, E may be NULL): PLAN_NONE when there is nothing to do (no server, not linked
// and no link to ask, this device only or a trial build, Play without the sync chosen before; the step says so), PLAN_LINK (no sync.txt and the early check asked no
// link/start: no network then), PLAN_CHECK
static int begin_plan(JNIEnv *E, jobject ctx) {
    pthread_mutex_lock(&C.mu);
    if (!C.began && !cfg_load_locked()) {   // (cfg_load_locked: a link by hand on the launcher's page since the early check wrote sync.txt)
        char sb[600]; if (link_base_locked(sb, sizeof sb)) { pthread_mutex_unlock(&C.mu); dm_start_step(STV_CLOUD, STV_SKIP, korean() ? "클라우드 서버 없음" : "No cloud server"); return PLAN_NONE; }   // (said once, at attach)
        ss_state st; char sp[700]; snprintf(sp, sizeof sp, "%s/sync-state.txt", dm_saves_dir()); int key = C.key[0] != 0, tried = C.auto_tried, trial = C.trial;
        pthread_mutex_unlock(&C.mu);
        ss_state_load(sp, &st);
        int net = key && !tried ? dm_net_state(E, ctx) : 0, go = key && !tried && ss_link_auto(0, trial, st.mode == SS_MODE_LOCAL, net);
        if (!go) { if (key && !tried) LOG("cloud: no sync.txt and %s: no link asked this start", trial ? "a trial build" : st.mode == SS_MODE_LOCAL ? "this game is this device only" : "no network");
                   dm_start_step(STV_CLOUD, STV_SKIP, korean() ? "기기 연결 필요" : "Not linked"); return PLAN_NONE; }
        pthread_mutex_lock(&C.mu); C.auto_tried = 1; pthread_mutex_unlock(&C.mu);
        return PLAN_LINK;
    }
    if (C.mode == SS_RUN_SUPPRESSED) {   // no network at all; the next start with the sync asks before what this run saves goes up
        ss_mark_offline_play(&C.st); st_save_locked();
        changed_locked(); pthread_mutex_unlock(&C.mu);
        dm_start_step(STV_CLOUD, STV_SKIP, korean() ? (C.trial ? "시험판" : "이 기기만") : (C.trial ? "Trial build" : "This device only")); return PLAN_NONE;
    }
    if (C.skip_req) {   // the launcher's Play without the sync, pressed before the check: none is made
        C.skipped = 1; ss_mark_offline_play(&C.st); st_save_locked(); changed_locked(); pthread_mutex_unlock(&C.mu);
        LOG("cloud: this run plays without the sync (the launcher's choice; the next start asks before its save goes up)");
        dm_start_step(STV_CLOUD, STV_SKIP, korean() ? "건너뜀" : "Skipped"); return PLAN_NONE;
    }
    pthread_mutex_unlock(&C.mu);
    return PLAN_CHECK;
}
int dm_cloud_begin(ANativeActivity *act, ANativeWindow *w, void (*done)(int quit)) {
    if (!getenv("DM_ROOT")) return 0;
    dm_cloud_attach(act);
    const char *env = getenv("DM_SAVESYNC"); int env_off = env && (!strcmp(env, "off") || !strcmp(env, "0"));
    const char *gv = getenv("DM_GAME_VERSION"); snprintf(C.gver, sizeof C.gver, "%s", gv ? gv : "");
    dm_cloud_precheck(act);   // (started at onCreate; an app that did not: now, and the start waits for it)
    pthread_mutex_lock(&C.mu);
    C.reached = 1; C.trial = C.trial_base || env_off;   // (env.txt is read now: the data may have brought it)
    if (C.began) C.mode = ss_run_mode(1, C.trial, C.st.mode == SS_MODE_LOCAL);
    int plan = PC.running ? PLAN_WAIT : PLAN_CHECK;
    pthread_mutex_unlock(&C.mu);
    if (plan != PLAN_WAIT) plan = begin_plan(act->env, act->clazz);
    if (plan == PLAN_NONE) return 0;
    pthread_mutex_lock(&C.mu);
    memset(&W, 0, sizeof W); W.choice = -1; W.done_cb = done; W.active = 1; W.autolink = plan != PLAN_CHECK;   // (a link, or the early check: B waits for it)
    if (plan == PLAN_CHECK) wait_locked("맞추는 중…", "Syncing…");
    else { wait_locked(plan == PLAN_LINK ? "기기 확인 중…" : "확인 중…", plan == PLAN_LINK ? "Looking for the link…" : "Checking…"); W.v.nbtn = 0; }
    pthread_mutex_unlock(&C.mu);
    if (!dm_cloud_screen_begin(act, w, screen_done)) {   // no screen to ask on: this run without the sync (never a start that waits on nothing)
        LOG("[RED] cloud: no start screen: this run plays without the sync");
        pthread_mutex_lock(&C.mu); W.done_cb = NULL; W.autolink = 0; if (C.began) finish_locked(0, 1); else { W.done = 1; W.active = 0; } pthread_mutex_unlock(&C.mu); return 0;
    }
    pthread_t t;
    if (pthread_create(&t, NULL, start_thread, (void *)(intptr_t)plan)) { LOG("[RED] cloud: cannot start the sync thread: this run plays without it"); if (plan == PLAN_CHECK) finish(0, 1, 0); else link_none(); }
    else pthread_detach(t);
    return 1;
}

// ---- the early check (onCreate, beside the data's step; see the top of this file): what the start's check needs of the network, done before it is needed. It asks
// nothing of the player and puts nothing in place (no download, no session): those wait for the start's check, after the data and the shaders ----
static int g_env_off;   // env.txt's DM_SAVESYNC_URL and DM_SAVESYNC=off (front_android.c puts env.txt in the environment only later)
static void env_early(void) {   // (the main thread, before the early check's thread starts: read only after)
    const char *root = getenv("DM_ROOT"); char p[700]; size_t n = 0; char *t = NULL, off[16] = "";
    if (root) { snprintf(p, sizeof p, "%s/env.txt", root); t = ds_read(p, &n, 1 << 16); }
    for (char *q = t; q && *q; ) {   // KEY=VALUE a line (# comments), as front_android.c env_file: the last one of a key wins
        char *e = q; while (*e && *e != '\n') e++; char c = *e; *e = 0;
        char *z = e; while (z > q && (z[-1] == '\r' || z[-1] == ' ')) *--z = 0;
        if (*q && *q != '#') { char *eq = strchr(q, '=');
            if (eq && eq - q == 11 && !strncmp(q, "DM_SAVESYNC", 11)) snprintf(off, sizeof off, "%s", eq + 1);
            else if (eq && eq - q == 15 && !strncmp(q, "DM_SAVESYNC_URL", 15)) snprintf(g_env_url, sizeof g_env_url, "%s", eq + 1); }
        *e = c; q = *e ? e + 1 : e;
    }
    free(t);
    const char *ev = getenv("DM_SAVESYNC"), *uv = getenv("DM_SAVESYNC_URL");
    if (!off[0] && ev) snprintf(off, sizeof off, "%s", ev);
    if (!g_env_url[0] && uv) snprintf(g_env_url, sizeof g_env_url, "%s", uv);
    g_env_off = !strcmp(off, "off") || !strcmp(off, "0");
}
static void local_now(void);
static int link_start(JNIEnv *E, int conn_ms, int64_t deadline, ss_link *l, char *why, size_t wcap); static int link_take(const ss_link *l);
enum { PRE_NONE, PRE_NOLINK, PRE_LOCAL, PRE_OFF, PRE_OK };   // the early check's outcome, for the start screen's checklist line (pre_thread)
static int g_pre_out;
static void pre_check(JNIEnv *E) {
    g_pre_out = PRE_NONE;
    pthread_mutex_lock(&C.mu);
    if (!C.began && !cfg_load_locked()) {   // no sync.txt: the start's own link (another game on this device linked it already: the token comes at once)
        char sb[600]; int none = link_base_locked(sb, sizeof sb) != 0, key = C.key[0] != 0, trial = C.trial;
        char sp[700]; snprintf(sp, sizeof sp, "%s/sync-state.txt", dm_saves_dir());
        pthread_mutex_unlock(&C.mu);
        if (none || !key) { g_pre_out = none ? PRE_NONE : PRE_NOLINK; return; }   // (no server: said at attach; no key: no link on this device)
        ss_state st; ss_state_load(sp, &st);
        int net = dm_net_state(E, C.act);
        if (!ss_link_auto(0, trial, st.mode == SS_MODE_LOCAL, net)) { g_pre_out = trial || st.mode == SS_MODE_LOCAL ? PRE_LOCAL : PRE_OFF; if (!trial && st.mode != SS_MODE_LOCAL) LOG("cloud: early check: no sync.txt and no network: no link asked now"); return; }
        pthread_mutex_lock(&C.mu); C.auto_tried = 1; pthread_mutex_unlock(&C.mu);
        ss_link l; char why[200] = ""; int64_t t0 = now_ms(); int k = link_start(E, 3000, t0 + 3000, &l, why, sizeof why);
        int took = k == SS_LK_LINKED && !link_take(&l); memset(&l, 0, sizeof l);
        if (!took) { LOG("cloud: early check: link/start: %s (%lld ms): not linked (the launcher's Cloud save page links it)", k == SS_LK_CODE ? "no device link yet" : k == SS_LK_LIMIT ? "asked too often (429)"
                         : k == SS_LK_LINKED ? "linked, but sync.txt could not be written" : why[0] ? why : "no answer", (long long)(now_ms() - t0)); g_pre_out = k == SS_LK_CODE || k == SS_LK_LINKED ? PRE_NOLINK : PRE_OFF; return; }
        LOG("cloud: early check: link/start: linked by the device's key (%lld ms)", (long long)(now_ms() - t0));
        pthread_mutex_lock(&C.mu);
        if (!C.began && !cfg_load_locked()) { pthread_mutex_unlock(&C.mu); return; }
    }
    int sync = C.mode == SS_RUN_SYNC; unsigned gen = C.cfg_gen;
    pthread_mutex_unlock(&C.mu);
    if (!sync) { g_pre_out = PRE_LOCAL; return; }   // this device only, a trial build: nothing asked of the server (the start's check marks the run)
    local_now();   // (the launcher's page; the start's check archives it again: the save manager's requests come between)
    int dev_net = dm_net_state(E, C.act), net = SS_NET_FAIL; ss_meta m; memset(&m, 0, sizeof m); m.files = -1; int64_t t0 = now_ms(); char why[200] = "";
    for (int again = 0; dev_net != 0 && again < 2; again++) {   // a refused token: once more with the new one (token_refused)
        char u[1400]; url_of(u, sizeof u, "meta"); hres r; memset(&m, 0, sizeof m); m.files = -1;
        int got = http_do(E, "GET", u, NULL, 0, NULL, PC_META_MS, now_ms() + PC_META_MS, 256 << 10, &r, NULL);
        net = net_of(got, &r, &m); if (got) snprintf(why, sizeof why, "%s", r.why); else if (net == SS_NET_FAIL) snprintf(why, sizeof why, "HTTP %d", r.code);
        ss_buf_free(&r.body);
        if (net != SS_NET_UNAUTH || !token_refused(E, r.gen)) break;
        pthread_mutex_lock(&C.mu); gen = C.cfg_gen; pthread_mutex_unlock(&C.mu);
    }
    pthread_mutex_lock(&C.mu);
    if (net == SS_NET_UNAUTH) {   // refused, and not linked now (token_refused: sync.txt aside): what changed is owed for when it is linked again; the start goes on without the sync
        int p; ss_decide(net, &m, &C.st, C.lknown && C.lsha[0] ? C.lsha : NULL, &p); if (p) C.st.pending = 1; if (C.state_path[0]) st_save_locked();
    } else if (C.cfg_gen == gen) {
        if (net == SS_NET_FAIL) { C.offline = 1; C.meta_ok = 0; }
        else { C.st.auth = 0; C.offline = 0; C.meta = m; C.meta_ok = 1; C.st.checked = (int64_t)time(NULL); you_seen_locked(m.you);
               snprintf(C.st.m_device, sizeof C.st.m_device, "%s", m.seq ? m.device : ""); C.st.m_time = m.seq ? m.time : 0; }
        PC.have = 1; PC.net = net; PC.m = m; PC.gen = C.cfg_gen; PC.t = now_ms();
    }
    changed_locked(); pthread_mutex_unlock(&C.mu);
    g_pre_out = net == SS_NET_UNAUTH ? PRE_NOLINK : net == SS_NET_FAIL ? PRE_OFF : PRE_OK;
    if (net == SS_NET_UNAUTH) LOG("cloud: early check: the token was refused and this device is not linked now");
    else if (net == SS_NET_FAIL) LOG("cloud: early check: %s: offline (the start goes on with this device's save)", dev_net == 0 ? "the device has no network" : why[0] ? why : "no answer");
    else LOG("cloud: early check: the server's meta in %lld ms (seq %llu)", (long long)(now_ms() - t0), (unsigned long long)m.seq);
}
static void *pre_thread(void *a) {
    (void)a; pthread_setname_np(pthread_self(), "cloudpre");
    int ko = korean(); dm_start_step(STV_CLOUD, STV_RUN, ko ? "확인 중…" : "Checking…");   // (the checklist shows the early check, not a wait: it runs beside the data)
    JNIEnv *E = jattach();
    if (!E) LOG("[RED] cloud: no Java for the early check: the start's check asks itself");
    else { pre_check(E); (*C.vm)->DetachCurrentThread(C.vm); }
    pthread_mutex_lock(&C.mu); PC.running = 0; changed_locked(); pthread_cond_broadcast(&C.cv); pthread_mutex_unlock(&C.mu);
    switch (g_pre_out) {   // the line until the start's check (after the data and the shaders) says what it found; that check writes the final word
    case PRE_OK: dm_start_step(STV_CLOUD, STV_DONE, ko ? "확인됨" : "Checked"); break;
    case PRE_NOLINK: dm_start_step(STV_CLOUD, STV_SKIP, ko ? "기기 연결 필요" : "Not linked"); break;
    case PRE_OFF: dm_start_step(STV_CLOUD, STV_SKIP, ko ? "오프라인" : "Offline"); break;
    case PRE_LOCAL: dm_start_step(STV_CLOUD, STV_WAIT, ko ? "대기" : "Waiting"); break;
    default: dm_start_step(STV_CLOUD, STV_WAIT, ko ? "대기" : "Waiting"); break;
    }
    dm_cloud_screen_poke();
    return NULL;
}
void dm_cloud_precheck(ANativeActivity *act) {
    if (!getenv("DM_ROOT")) return;
    dm_cloud_attach(act);
    pthread_mutex_lock(&C.mu); int was = PC.started; PC.started = 1; pthread_mutex_unlock(&C.mu);
    if (was) return;
    env_early(); dm_saves_paths(home_dir());   // (the save's paths: dm_saves_start sets the same ones later)
    pthread_mutex_lock(&C.mu); C.trial = C.trial_base || g_env_off; PC.running = 1; changed_locked(); pthread_mutex_unlock(&C.mu);
    pthread_t t;
    if (pthread_create(&t, NULL, pre_thread, NULL)) { LOG("[RED] cloud: cannot start the early check: the start's check asks itself"); pthread_mutex_lock(&C.mu); PC.running = 0; pthread_mutex_unlock(&C.mu); }
    else pthread_detach(t);
}

// ---- the device link: the start's (autolink: link/start once, its token only) and the launcher's (link_thread: a code, polled every 3 s, until it is linked, runs
// out or the player goes back). The key and the token are never logged whole ----
static struct { int state, gen, limit; char code[16], qr[400], host[160], device[64], poll[128]; int64_t until; } K;   // under C.mu; gen: the page's, a cancel moves it on
static int link_start(JNIEnv *E, int conn_ms, int64_t deadline, ss_link *l, char *why, size_t wcap) {   // SS_LK_*, -1 no answer (why)
    char base[600], u[700], body[1400];
    pthread_mutex_lock(&C.mu); int bad = ss_link_body(body, sizeof body, C.key, C.model, C.label, C.pkg); pthread_mutex_unlock(&C.mu);
    if (bad) { snprintf(why, wcap, "the request does not fit"); return -1; }
    if (link_base(base, sizeof base)) { snprintf(why, wcap, "no cloud save server"); return -1; }
    snprintf(u, sizeof u, "%s/api/saves/link/start", base);
    hres r; int got = http_req(E, "POST", u, (const uint8_t *)body, strlen(body), NULL, "application/json", 0, conn_ms, deadline, 64 << 10, &r, NULL);
    memset(body, 0, sizeof body);
    if (got) { snprintf(why, wcap, "%s", r.why); return -1; }
    int k = ss_link_read(r.code, (const char *)r.body.p, r.body.n, l); if (k == SS_LK_BAD) snprintf(why, wcap, "HTTP %d", r.code);
    ss_buf_free(&r.body); return k;
}
// a token: sync.txt written (0600), and once the start has reached the cloud, read as the start reads it. 0, -1 (not written)
static int link_take(const ss_link *l) {
    const char *root = home_or_null(); char p[700], base[600]; if (!root) return -1;
    snprintf(p, sizeof p, "%s/sync.txt", home_dir()); if (link_base(base, sizeof base)) return -1;
    pthread_mutex_lock(&C.mu); char dev[64]; snprintf(dev, sizeof dev, "%s", l->dev_name[0] ? l->dev_name : C.model); pthread_mutex_unlock(&C.mu);
    if (ss_link_write(p, base, l->token, dev)) { LOG("[RED] cloud: the link's sync.txt cannot be written (errno %d)", errno); return -1; }
    LOG("cloud: this device is linked (%s, device %s, token %.6s...): sync.txt written", base, dev, l->token);
    pthread_mutex_lock(&C.mu);
    if (C.reached && !C.began && cfg_load_locked() && C.mode == SS_RUN_SUPPRESSED) { ss_mark_offline_play(&C.st); st_save_locked(); }
    changed_locked(); pthread_mutex_unlock(&C.mu);
    return 0;
}
// A token the server refused (savesync.h ss_reauth_*): sync.txt to sync.txt.revoked, link/start once a run; linked, the new sync.txt is C.cfg and the request goes again;
// else not linked: this run on without the sync (sync-state.txt kept), as an app never linked. Never a question (the game may be running). gen: the refused request's
// hres.gen (another thread dealt with it since: its outcome). 1: send it again, 0: not linked now
static int token_refused(JNIEnv *E, unsigned gen) {
    char p[700]; snprintf(p, sizeof p, "%s/sync.txt", home_dir());
    pthread_mutex_lock(&C.mu);
    while (C.reauth_busy) pthread_cond_wait(&C.cv, &C.mu);
    if (C.cfg_gen != gen || !C.began) { int on = C.began && C.mode == SS_RUN_SYNC; pthread_mutex_unlock(&C.mu); return on; }
    int go = ss_reauth_begin(&C.reauth, C.key[0] != 0, C.trial); C.reauth_busy = 1; char t4[8]; snprintf(t4, sizeof t4, "%.4s", C.cfg.token);
    pthread_mutex_unlock(&C.mu);
    LOG("[RED] cloud: the server refused the token (%s...): sync.txt to sync.txt.revoked%s", t4, go == SS_RA_ASK ? ", link/start asked" : ": not linked (asked once this run already, or no key)");
    if (ss_revoke(p)) LOG("[RED] cloud: sync.txt cannot be moved aside (errno %d)", errno);
    int k = -1, taken = 0; int64_t t0 = now_ms();
    if (go == SS_RA_ASK) {
        ss_link l; char why[200] = ""; k = link_start(E, 3000, t0 + 5000, &l, why, sizeof why);
        if (k == SS_LK_LINKED && !link_take(&l)) {
            pthread_mutex_lock(&C.mu); char *txt; size_t n = 0; ss_cfg c;
            if ((txt = ds_read(p, &n, 1 << 16))) {
                if (ss_cfg_text(txt, &c)) { char pk[600]; ss_qenc(pk, sizeof pk, C.pkg); C.cfg = c; snprintf(C.base, sizeof C.base, "%s/api/saves/g/%s", C.cfg.url, pk); C.cfg_gen++; C.st.auth = 0; taken = 1; }
                memset(txt, 0, n); free(txt); memset(&c, 0, sizeof c);
            }
            pthread_mutex_unlock(&C.mu);
        }
        memset(&l, 0, sizeof l);
        LOG("cloud: link/start after the refusal (%lld ms): %s", (long long)(now_ms() - t0), taken ? "linked by the device's key: the new token is used now"
            : k == SS_LK_LINKED ? "linked, but the new sync.txt could not be written or read" : k == SS_LK_CODE ? "not linked (the launcher's Cloud save page links it)"
            : k == SS_LK_LIMIT ? "asked too often (429)" : why[0] ? why : "no answer");
    }
    int ra = ss_reauth_end(k, taken);
    pthread_mutex_lock(&C.mu);
    if (ra != SS_RA_RETRY) {   // not linked: as dm_cloud_unlink (the state stays, with what is owed)
        C.st.auth = 0; if (C.state_path[0]) st_save_locked();
        C.began = 0; C.mode = SS_RUN_NONE; memset(&C.cfg, 0, sizeof C.cfg); C.cfg_gen++; C.meta_ok = 0; C.offline = 0; C.sess_open = 0; changed_locked();
    }
    C.reauth_busy = 0; pthread_cond_broadcast(&C.cv); pthread_mutex_unlock(&C.mu);
    if (ra != SS_RA_RETRY) { LOG("cloud: this device is not linked now: no sync this run (the next start asks link/start again)"); dm_start_step(STV_CLOUD, STV_SKIP, korean() ? "기기 연결 필요" : "Not linked"); }
    dm_cloud_screen_poke();
    return ra == SS_RA_RETRY;
}
static int autolink(JNIEnv *E) {   // 1: linked, the check goes on; 0: not linked (the start goes on without the sync); -1: the start ended meanwhile
    ss_link l; char why[200] = ""; int64_t t0 = now_ms(); int k = link_start(E, 3000, t0 + 3000, &l, why, sizeof why);
    int took = k == SS_LK_LINKED && !link_take(&l); memset(&l, 0, sizeof l);
    pthread_mutex_lock(&C.mu); W.autolink = 0; int d = W.done, sync = C.mode == SS_RUN_SYNC;
    if (took && d && C.began) { C.skipped = 1; ss_mark_offline_play(&C.st); st_save_locked(); changed_locked(); }   // (the player chose to play without the sync meanwhile)
    pthread_mutex_unlock(&C.mu);
    if (took) LOG("cloud: link/start: linked by the device's key (%lld ms): %s", (long long)(now_ms() - t0), d ? "this run without the sync (the player's choice)" : "the check goes on");
    else LOG("cloud: link/start: %s (%lld ms): this start goes on without the sync", k == SS_LK_CODE ? "not linked yet (the launcher's Cloud save page links it)" : k == SS_LK_LIMIT ? "asked too often (429)"
             : k == SS_LK_LINKED ? "linked, but sync.txt could not be written" : why[0] ? why : "no answer", (long long)(now_ms() - t0));
    if (d) return -1;
    if (took && !sync) { pthread_mutex_lock(&C.mu); W.done = 1; W.active = 0; pthread_mutex_unlock(&C.mu); dm_start_step(STV_CLOUD, STV_SKIP, korean() ? "이 기기만" : "This device only"); dm_cloud_screen_end(); return -1; }
    return took ? 1 : 0;
}
static void link_none(void) {   // the start's link found none: the start goes on without the sync (nothing is marked: no sync.txt, no sync)
    pthread_mutex_lock(&C.mu); int was = W.done; W.done = 1; W.active = 0; W.autolink = 0; W.up_after = 0; pthread_mutex_unlock(&C.mu);
    if (!was) dm_start_step(STV_CLOUD, STV_SKIP, korean() ? "기기 연결 필요" : "Not linked");
    dm_cloud_screen_end(); dm_cloud_screen_poke();
}
static void link_state(int gen, int state, int limit) { pthread_mutex_lock(&C.mu); if (K.gen == gen) { K.state = state; K.limit = limit; } pthread_mutex_unlock(&C.mu); dm_cloud_screen_poke(); }
static void link_done(int gen, const ss_link *l) {
    if (link_take(l)) { link_state(gen, DM_LINK_FAIL, 0); return; }
    pthread_mutex_lock(&C.mu); if (K.gen == gen) { K.state = DM_LINK_DONE; snprintf(K.device, sizeof K.device, "%s", l->dev_name[0] ? l->dev_name : C.model); } pthread_mutex_unlock(&C.mu);
    dm_cloud_screen_poke();
}
static void *link_thread(void *a) {
    int gen = (int)(intptr_t)a; pthread_setname_np(pthread_self(), "cloudlink");
    JNIEnv *E = jattach(); if (!E) { LOG("[RED] cloud: no Java for the link"); link_state(gen, DM_LINK_FAIL, 0); return NULL; }
    ss_link l; char why[200] = "", base[600], u[900]; link_base(base, sizeof base);
    int k = link_start(E, 5000, now_ms() + 10000, &l, why, sizeof why);
    if (k == SS_LK_LINKED) link_done(gen, &l);
    else if (k != SS_LK_CODE) { LOG("cloud: link/start: %s: no code", k == SS_LK_LIMIT ? "asked too often (429)" : why[0] ? why : "no answer"); link_state(gen, DM_LINK_FAIL, k == SS_LK_LIMIT); }
    else {
        char q[600]; ss_qenc(q, sizeof q, l.poll); snprintf(u, sizeof u, "%s/api/saves/link/poll?poll=%s", base, q);
        pthread_mutex_lock(&C.mu);
        if (K.gen == gen) { K.state = DM_LINK_CODE; snprintf(K.code, sizeof K.code, "%s", l.code); K.until = now_ms() + l.expires * 1000;
            char lu[700]; ss_link_page_url(lu, sizeof lu, l.url, base);
            ss_link_qr(K.qr, sizeof K.qr, lu, l.code); ss_link_host(K.host, sizeof K.host, lu); }
        pthread_mutex_unlock(&C.mu); dm_cloud_screen_poke();
        LOG("cloud: link/start: a code for the player, %lld s; polled every 3 s", (long long)l.expires);
        int quiet = 0;
        for (;;) {
            pthread_mutex_lock(&C.mu);
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 3;
            while (K.gen == gen && K.state == DM_LINK_CODE) if (pthread_cond_timedwait(&C.cv, &C.mu, &ts)) break;
            int alive = K.gen == gen && K.state == DM_LINK_CODE; int64_t until = K.until;
            pthread_mutex_unlock(&C.mu);
            if (!alive) { LOG("cloud: the link's code was let go"); break; }
            if (now_ms() >= until) { LOG("cloud: the link's code ran out"); link_state(gen, DM_LINK_EXPIRED, 0); break; }
            hres r; if (http_req(E, "GET", u, NULL, 0, NULL, NULL, 0, 3000, now_ms() + 5000, 64 << 10, &r, NULL)) { if (!quiet++) LOG("cloud: link/poll: no answer (%s): polled on", r.why); continue; }
            ss_link p; int pk = ss_link_read(r.code, (const char *)r.body.p, r.body.n, &p); ss_buf_free(&r.body);
            if (pk == SS_LK_LINKED) { link_done(gen, &p); memset(&p, 0, sizeof p); break; }
            if (pk == SS_LK_EXPIRED) { LOG("cloud: link/poll: HTTP %d: the code is gone", r.code); link_state(gen, DM_LINK_EXPIRED, 0); break; }
            if (pk == SS_LK_WAITING && p.expires > 0) { pthread_mutex_lock(&C.mu); if (K.gen == gen) K.until = now_ms() + p.expires * 1000; pthread_mutex_unlock(&C.mu); }
            else if (pk != SS_LK_WAITING && !quiet++) LOG("cloud: link/poll: HTTP %d: polled on", r.code);
        }
    }
    memset(&l, 0, sizeof l);
    (*C.vm)->DetachCurrentThread(C.vm);
    return NULL;
}
void dm_cloud_link_get(dm_cloud_link *o) {
    memset(o, 0, sizeof *o); pthread_mutex_lock(&C.mu);
    o->state = K.state; o->limit = K.limit; snprintf(o->code, sizeof o->code, "%s", K.code); snprintf(o->qr, sizeof o->qr, "%s", K.qr); snprintf(o->host, sizeof o->host, "%s", K.host);
    if (!o->host[0]) { char b[600], lu[700]; if (!link_base_locked(b, sizeof b)) { ss_link_page_url(lu, sizeof lu, NULL, b); ss_link_host(o->host, sizeof o->host, lu); } }   // (linked at once: no code, no address of the server's)
    snprintf(o->device, sizeof o->device, "%s", K.device); int64_t l = K.until - now_ms(); o->left_s = l > 0 ? (int)((l + 999) / 1000) : 0;
    pthread_mutex_unlock(&C.mu);
}
int dm_cloud_link_begin(void) {
    pthread_mutex_lock(&C.mu);
    char sb[600]; int ok = C.attached && C.vm && C.key[0] && !C.trial_base && !C.began && !W.active && !PC.running && K.state != DM_LINK_BUSY && K.state != DM_LINK_CODE && !link_base_locked(sb, sizeof sb), gen = 0;
    if (ok) { gen = ++K.gen; K.state = DM_LINK_BUSY; K.limit = 0; K.code[0] = K.qr[0] = K.host[0] = K.device[0] = 0; K.until = 0; }
    pthread_mutex_unlock(&C.mu);
    if (!ok) return -1;
    LOG("cloud: the launcher links this device (link/start)");
    pthread_t t; if (pthread_create(&t, NULL, link_thread, (void *)(intptr_t)gen)) { link_state(gen, DM_LINK_FAIL, 0); return -1; }
    pthread_detach(t); dm_cloud_screen_poke();
    return 0;
}
void dm_cloud_link_cancel(void) { pthread_mutex_lock(&C.mu); K.gen++; K.state = DM_LINK_NONE; pthread_cond_broadcast(&C.cv); pthread_mutex_unlock(&C.mu); dm_cloud_screen_poke(); }
void dm_cloud_link_end(void) {
    pthread_mutex_lock(&C.mu); int was = K.state == DM_LINK_DONE, go = was && C.began && C.mode == SS_RUN_SYNC && !C.trial; K.gen++; K.state = DM_LINK_NONE; pthread_mutex_unlock(&C.mu);
    if (go) { LOG("cloud: linked: the check runs now"); dm_cloud_act(DM_CLOUD_ACT_CHECK); }
    dm_cloud_screen_poke();
}
int dm_cloud_unlink(void) {
    const char *root = home_or_null(); char p[700]; if (!root) return -1;
    snprintf(p, sizeof p, "%s/sync.txt", home_dir());
    pthread_mutex_lock(&C.mu);
    int ok = C.began && !C.trial && !C.busy && !W.active && !C.up_started && C.reached && !PC.running;
    if (ok && unlink(p) && errno != ENOENT) { LOG("[RED] cloud: sync.txt cannot be removed (errno %d)", errno); ok = 0; }
    if (ok) { C.began = 0; C.mode = SS_RUN_NONE; memset(&C.cfg, 0, sizeof C.cfg); C.cfg_gen++; C.meta_ok = 0; C.offline = 0; C.result[0] = 0; W.up_after = 0; changed_locked(); }
    pthread_mutex_unlock(&C.mu);
    if (!ok) return -1;
    LOG("cloud: the launcher unlinked this app (sync.txt removed; the device's other games stay linked)");
    dm_start_step(STV_CLOUD, STV_SKIP, korean() ? "기기 연결 필요" : "Not linked"); dm_cloud_screen_poke();
    return 0;
}

// ---- the launcher (datasync_android.c): its Cloud save page reads this state, and asks for a mode, Upload now, Back to the cloud save, a check again ----
_Static_assert(SS_LOCAL_UNKNOWN == DM_CLOUD_LOCAL_UNKNOWN && SS_LOCAL_SAME == DM_CLOUD_LOCAL_SAME && SS_LOCAL_OWED == DM_CLOUD_LOCAL_OWED && SS_LOCAL_ONLY == DM_CLOUD_LOCAL_ONLY
               && SS_LOCAL_PLAYED == DM_CLOUD_LOCAL_PLAYED && SS_LOCAL_CONFLICT == DM_CLOUD_LOCAL_CONFLICT, "the launcher's This device row");
void dm_cloud_info_get(dm_cloud_info *o) {
    memset(o, 0, sizeof *o); o->progress = -1; o->files = -1;
    pthread_mutex_lock(&C.mu);
    o->avail = C.began; o->trial = C.trial; o->mode = C.trial ? SS_MODE_LOCAL : C.st.mode;
    o->checking = W.active || PC.running; o->act = C.busy; o->progress = (C.busy || W.active) && C.prog >= 0 ? C.prog : -1;
    o->starting = PC.running || (W.active && !W.recheck);   // the start's check (the early one, or its own) has not ended
    o->ready = C.reached && !o->starting;                     // (the page's actions wait for the start's check: it may still put the cloud's save in place)
    o->pre_go = -1;
    if (!C.reached && PC.have && C.began && C.mode == SS_RUN_SYNC && C.lknown && PC.gen == C.cfg_gen) { int p; o->pre_go = ss_decide(PC.net, &PC.m, &C.st, C.lsha[0] ? C.lsha : NULL, &p); }
    o->online = C.meta_ok; o->offline = C.offline; o->skipped = C.skipped || C.skip_req; o->auth = C.st.auth;
    o->local = C.began ? ss_local_state(&C.st, C.lknown ? C.lsha : NULL) : SS_LOCAL_UNKNOWN;
    if (C.trial) o->local = SS_LOCAL_ONLY;
    const char *cd = C.meta_ok ? (C.meta.seq ? C.meta.device : "") : C.st.m_device; o->cloud_time = C.meta_ok ? (C.meta.seq ? C.meta.time : 0) : C.st.m_time;
    snprintf(o->cloud_dev, sizeof o->cloud_dev, "%s", ss_name_mine(&C.cfg, cd) ? C.cfg.device : cd);   // (uploaded under an older name of this device's: its name now)
    o->checked = C.st.checked; o->size = C.lknown ? C.lsize : 0; o->files = C.lknown ? C.lfiles : -1; snprintf(o->device, sizeof o->device, "%s", C.cfg.device);
    int free_ = C.began && !C.trial && C.st.mode != SS_MODE_LOCAL && C.meta_ok && !C.busy && !W.active && !C.st.auth && o->ready;
    o->can_upload = free_ && (o->local == SS_LOCAL_OWED || o->local == SS_LOCAL_PLAYED);
    o->can_restore = free_ && C.meta.seq != 0;
    snprintf(o->result, sizeof o->result, "%s", C.result); o->result_bad = C.result_bad;
    char sb[600]; o->noserver = !C.began && link_base_locked(sb, sizeof sb);
    o->linkable = C.attached && C.vm && C.key[0] && !C.trial_base && !C.began && !W.active && !PC.running && !o->noserver;
    o->can_unlink = C.began && !C.trial && !C.busy && !W.active && !C.up_started && o->ready;
    pthread_mutex_unlock(&C.mu);
}
static void local_now(void) {   // this device's save archived again, for the page (before the game: nothing commits)
    ss_buf t; char sha[65], err[600]; int64_t nw = 0; int r = snap(&t, sha, &nw, err, sizeof err);
    pthread_mutex_lock(&C.mu);
    if (r >= 0) { C.lknown = 1; snprintf(C.lsha, sizeof C.lsha, "%s", r ? "" : sha); C.lsize = r ? 0 : t.n; C.lfiles = r ? 0 : ss_tar_files(t.p, t.n); C.lt = nw; changed_locked(); }
    pthread_mutex_unlock(&C.mu);
    if (!r) ss_buf_free(&t);
}
void dm_cloud_local_refresh(void) { pthread_mutex_lock(&C.mu); int go = C.began && !C.lknown && !C.busy && !W.active && !PC.running; pthread_mutex_unlock(&C.mu); if (go) local_now(); }
static void act_result(const char *ko, const char *en, int bad) {
    pthread_mutex_lock(&C.mu); snprintf(C.result, sizeof C.result, "%s", korean() ? ko : en); C.result_bad = bad; changed_locked(); pthread_mutex_unlock(&C.mu); dm_cloud_screen_poke();
}
// a check the launcher asked for (back from this device only, a 409 to Upload now): the start's check again, its questions over the launcher; the launcher goes on after it
// (W.done_cb stays as it is: the start's own callback may not have run yet)
static void recheck(JNIEnv *E) {
    pthread_mutex_lock(&C.mu);
    W.done = W.quit = W.applying = 0; W.choice = -1; W.recheck = 1; W.active = 1; C.mode = SS_RUN_SYNC; C.offline = 0; C.skipped = 0; wait_locked("맞추는 중…", "Syncing…");
    pthread_mutex_unlock(&C.mu);
    dm_start_step(STV_CLOUD, STV_RUN, korean() ? "맞추는 중…" : "Syncing…"); dm_cloud_screen_poke();
    start_check(E, 1);
    pthread_mutex_lock(&C.mu); if (W.active) { W.active = 0; W.done = 1; } pthread_mutex_unlock(&C.mu);
}
static void *act_thread(void *a) {
    int what = (int)(intptr_t)a; pthread_setname_np(pthread_self(), "cloudact");
    JNIEnv *E = jattach();
    if (!E) act_result("자바를 쓸 수 없어 못 했어요", "No Java for it", 1);
    else {
        if (what == DM_CLOUD_ACT_UPLOAD) {   // Upload now: what this device has (a run without the sync's too: the player asks for it)
            pthread_mutex_lock(&C.mu); C.st.offline_play = 0; C.st.pending = 1; st_save_locked(); changed_locked(); pthread_mutex_unlock(&C.mu); dm_cloud_screen_poke();
            LOG("cloud: the launcher's Upload now");
            int r = upload(E, now_ms() + 60000);
            if (r == UP_OK) act_result("올렸어요", "Uploaded", 0);
            else if (r == UP_CONFLICT) { act_result("다른 기기의 세이브가 더 새로워요", "Another device's save is newer", 1); recheck(E); }
            else if (r == UP_FAIL) act_result("연결되지 않아 올리지 못했어요", "Not connected: not uploaded", 1);
            else if (r == UP_NOTHING) act_result("올릴 진행이 없어요", "Nothing to upload", 0);
            else act_result("올리지 못했어요", "It could not be uploaded", 1);
        } else if (what == DM_CLOUD_ACT_RESTORE) {   // Back to the cloud save: the server's meta now, its save in, this device's to the backup slot
            char u[1400], why[300] = ""; hres r; ss_meta m; memset(&m, 0, sizeof m); int got;
            for (int again = 0;; again++) {   // a refused token: once more with the new one (token_refused)
                url_of(u, sizeof u, "meta"); got = http_do(E, "GET", u, NULL, 0, NULL, 3000, now_ms() + 5000, 256 << 10, &r, NULL);
                if (got || !ss_refused(r.code) || again) break;
                ss_buf_free(&r.body); if (!token_refused(E, r.gen)) break;
            }
            int ok = !got && r.code == 200 && !ss_meta_parse((const char *)r.body.p, r.body.n, &m) && m.seq; ss_buf_free(&r.body);
            LOG("cloud: the launcher's Back to the cloud save: %s", ok ? "the server's save comes in" : got ? r.why : "the server has no save");
            if (!ok) act_result(got ? "클라우드에 연결할 수 없어요" : "클라우드에 세이브가 없어요", got ? "The cloud cannot be reached" : "The cloud has no save", 1);
            else if (download_apply(E, &m, why, sizeof why, BACKUP_SLOT, 1)) { LOG("[RED] cloud: Back to the cloud save failed: %s", why); act_result("되돌리지 못했어요", "It could not be done", 1); }
            else { pthread_mutex_lock(&C.mu); C.meta = m; C.meta_ok = 1; you_seen_locked(m.you); pthread_mutex_unlock(&C.mu); act_result("되돌렸어요 · 이 기기 세이브는 백업 1칸에", "Done · this device's save is in the backup slot", 0); }
        } else if (what == DM_CLOUD_ACT_CHECK) recheck(E);
        local_now();
        (*C.vm)->DetachCurrentThread(C.vm);
    }
    pthread_mutex_lock(&C.mu); C.busy = 0; C.prog = -1; changed_locked(); pthread_mutex_unlock(&C.mu); dm_cloud_screen_poke();
    return NULL;
}
int dm_cloud_act(int what) {   // (takes no lock of datasync_android.c's: the screen's thread may call it holding its own)
    pthread_mutex_lock(&C.mu);
    int ok = what > DM_CLOUD_ACT_NONE && what <= DM_CLOUD_ACT_CHECK && C.began && C.vm && !C.trial && !C.busy && !W.active && C.reached && !PC.running;
    if (ok) { C.busy = what; C.prog = -1; C.result[0] = 0; changed_locked(); }
    pthread_mutex_unlock(&C.mu);
    if (!ok) return -1;
    pthread_t t;
    if (pthread_create(&t, NULL, act_thread, (void *)(intptr_t)what)) { LOG("[RED] cloud: cannot start the launcher's action"); pthread_mutex_lock(&C.mu); C.busy = 0; pthread_mutex_unlock(&C.mu); return -1; }
    pthread_detach(t);
    return 0;
}
int dm_cloud_set_mode(int mode) {
    pthread_mutex_lock(&C.mu);
    if (!C.began || C.trial || C.busy || W.active || !C.reached || PC.running || mode < SS_MODE_AUTO || mode > SS_MODE_LOCAL || mode == C.st.mode) { pthread_mutex_unlock(&C.mu); return -1; }
    int was = C.st.mode; ss_set_mode(&C.st, mode); st_save_locked(); changed_locked(); C.result[0] = 0;
    int again = was == SS_MODE_LOCAL;   // back from this device only: a first connection's check, now
    if (mode == SS_MODE_LOCAL) { C.mode = SS_RUN_SUPPRESSED; C.meta_ok = 0; }
    pthread_mutex_unlock(&C.mu);
    static const char *const NAME[] = {"automatic", "manual", "this device only"};
    LOG("cloud: the launcher sets this game's mode to %s%s", NAME[mode], again ? " (a first connection's check now)" : "");
    if (mode == SS_MODE_LOCAL) dm_start_step(STV_CLOUD, STV_SKIP, korean() ? "이 기기만" : "This device only");
    if (again) dm_cloud_act(DM_CLOUD_ACT_CHECK);
    dm_cloud_screen_poke();
    return 0;
}
// Play without the sync, from the launcher: before the check, none is made (dm_cloud_begin); during it, it ends without the sync (as B); after it, nothing goes up this
// run and the next start asks before this run's save does. -1: the cloud's save is being put in place (it cannot stop now)
int dm_cloud_play_unsynced(void) {
    pthread_mutex_lock(&C.mu);
    if (!C.began) { C.skip_req = 1; pthread_mutex_unlock(&C.mu); return 0; }
    if (W.active) {
        if (W.applying) { pthread_mutex_unlock(&C.mu); return -1; }
        finish_locked(0, 1); pthread_mutex_unlock(&C.mu); skipped_end(); return 0;
    }
    int now = C.mode == SS_RUN_SYNC && !C.skipped;
    if (now) { C.skipped = 1; ss_mark_offline_play(&C.st); st_save_locked(); changed_locked(); }
    pthread_mutex_unlock(&C.mu);
    if (now) { LOG("cloud: this run plays without the sync (the launcher's choice after the check; the next start asks before its save goes up)"); dm_start_step(STV_CLOUD, STV_SKIP, korean() ? "건너뜀" : "Skipped"); }
    return 0;
}
