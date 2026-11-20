// Android port: the save's cloud sync, as Steam Cloud does it for a game: the save is taken from the server before the game starts when the server's is newer, and goes up
// soon after the game commits it and when the app leaves the screen. The server (rooms: /api/saves/g/<package>/{meta,data,session}, a Bearer token) keeps one save a game,
// opaque bytes: the deterministic tar of savesync.h, whose SHA-256 is the save's identity. savesync.h holds the parts that need no Android (the archive, sync.txt, the state,
// the meta, the start's decision; dm/host/tests/t_savesync.c tests them); this file is the transport, the threads, the start screen's content and the pause menu's row.
//
// Off unless the files folder has sync.txt (url=, token=, device=: savesync.h ss_cfg): without it nothing here runs and the app is as it was. The token is never logged
// (its first 4 characters only). A trial build (an APK without a data server, a debuggable one, env.txt's DM_SAVESYNC=off) and a game whose sync the player switched off in
// the pause menu run without the network at all; such a run is marked offline_play in the state (saves/sync-state.txt), and the next start with the sync asks before that
// run's save goes up.
//
// Offline first: a device without a network (ConnectivityManager: none at all) starts the game at once with its save, no question. The start (dm_cloud_begin, from game_start before the host mounts the save, after the save manager's
// requests) runs on a thread of its own with its screen (datasync_android.c dm_cloud_screen_begin): nothing shows when it ends within 300 ms; the meta is asked for with
// 3 s to connect and 5 s in all; no answer offers Play anyway (taken by itself after 10 s without input) and Retry. B at any moment plays without the sync this run. When
// the server's save is newer and this device's did not change since the last sync, it is downloaded (a progress bar, 60 s at most), checked against its SHA-256 and swapped
// in through the save manager's import (saves_android.c dm_saves_import_now: the current save is exported first, <stamp>-before-import). When both changed, the player
// chooses between two cards (nothing is overwritten by itself; the side not chosen stays: this device's in the import's backup folder, the server's previous save in the
// server's history). Another device first (savesync.h ss_other_device, from the session): one with a save the cloud has not got (dirty, in the last 30 days) is warned
// about, Start anyway or Cancel; one running the game now (a heartbeat in the last 3 min) is a note on the screen for 1.5 s; one whose heartbeats stopped (killed) is
// nothing. Then the session is marked open on the server (dirty 0: the start just synced).
//
// Uploads (the "savesync" thread): the commit count (hle_fs.c dm_fs_commit_count) is polled every 1.5 s; once a poll sees no new commit after a burst (about 3 s after the
// game saved) the committed tree is archived (never <DM_SAVE>.work; a commit while it is read reads it again) and, when its hash is not the last synced one, PUT with
// base = the last synced seq and files = its file count. 409: the server moved (another device): a conflict, kept for the next start's cards, not retried, one corner
// note; nothing is ever asked while the game runs. No network: retried after 1, 2, 5, 10 min, and at once when the device's network comes back (ConnectivityManager,
// read at each poll: no request is made while the device has none) or the app is back in front. A run that started offline opens its session once the network is back.
// onPause (sleep, HOME, the app's end) uploads what is owed at once under a 15 s partial wake lock, then closes the session with dirty = whether a save is still owed
// (pending or a conflict). While the game is on screen the upload thread sends a heartbeat every 60 s (alive, dirty the same), and one at once after an upload went up;
// onResume opens it again. A heartbeat that does not go through is dropped (offline). The corner marks, icons only (dm_cloud_mark): every upload's spinner and,
// when it went up, its check (uploads less than 3 s apart: one spinner, one check); waiting and a conflict a change of state only, none of a kind within 5 min
// (savesync.h ss_up_begin, ss_up_end, ss_mark_now). Every network failure is a log line: the game never waits for the network once it runs.
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

uint64_t dm_fs_commit_count(void);       // hle_fs.c
const uint8_t *dm_overlay_font(void);    // overlay_theme.c
void dm_overlay_redraw(void);            // the backend (front_android.c has a weak one): the held game's last frame presented again
int dm_menu_state(void);                 // front_android.c: -1 when the pause menu is closed

static int64_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static int has_glyph(uint32_t cp) { const pf_index *g = pf_find(dm_overlay_font(), cp); return g && (g->cp_adv >> 8) == cp; }   // not the U+FFFD a font without it draws
static int korean(void) { return has_glyph(0xAC00); }   // the APK's font has Hangul (assets/font/ui.bin): the screens in Korean, else in English

// ---- the state (C.mu guards all of it) ----
static struct {
    pthread_mutex_t mu; pthread_cond_t cv;
    int mode, trial;                     // savesync.h SS_RUN_*; a trial build
    ss_cfg cfg; char pkg[256], build[64], gver[64], state_path[700], base[900];   // base: <url>/api/saves/g/<package>
    ss_state st;
    int skipped;                         // this run plays without the sync (B at the start)
    int uploading, offline, fails, kick, pause_ev, resume_ev, up_started; unsigned pause_done;   // pause_done: the leaves (pause_flush) done
    int sess_open;                       // the server took this run's session open (or a heartbeat): a run that started offline opens it once the network is back
    ss_notes notes;                      // the corner marks (savesync.h ss_up_begin, ss_up_end, ss_mark_now)
    int64_t next_try;                    // when an owed upload is tried again (monotonic ms)
    unsigned ver;                        // the menu row's version
    JavaVM *vm; jobject act;             // the NativeActivity (a global reference, ANativeActivity.clazz)
} C = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, .mode = SS_RUN_NONE};
static void st_save_locked(void) { if (C.state_path[0] && ss_state_save(C.state_path, &C.st)) LOG("[RED] cloud: cannot write %s (errno %d)", C.state_path, errno); }
static void changed_locked(void) { C.ver++; }
static void menu_redraw(void) { if (dm_menu_state() >= 0) dm_overlay_redraw(); }   // (the panel compares dm_cloud_menu_ver)
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
typedef struct { int code; ss_buf body; char seq[32], sha[80], why[200]; } hres;
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
static int http_do(JNIEnv *E, const char *method, const char *url, const uint8_t *body, size_t n, const char *body_sha, int conn_ms, int64_t deadline, size_t cap, hres *r, hprog prog) {
    memset(r, 0, sizeof *r); int rc = -1; jobject U = NULL, Cn = NULL, in = NULL, out = NULL; jbyteArray buf = NULL; char auth[600];
    if ((*E)->PushLocalFrame(E, 64)) { jx(E, NULL, 0); snprintf(r->why, sizeof r->why, "no JNI frame"); return -1; }
    jstring su = (*E)->NewStringUTF(E, url); if (jx(E, r->why, sizeof r->why) || !su) goto out;
    U = (*E)->NewObject(E, J.cUrl, J.mUrl, su); if (jx(E, r->why, sizeof r->why) || !U) goto out;
    Cn = (*E)->CallObjectMethod(E, U, J.mOpen); if (jx(E, r->why, sizeof r->why) || !Cn || !(*E)->IsInstanceOf(E, Cn, J.cConn)) goto out;
    int64_t left = deadline - now_ms(); if (left < 200) { snprintf(r->why, sizeof r->why, "out of time"); goto out; }
    (*E)->CallVoidMethod(E, Cn, J.mConnTo, (jint)(conn_ms < left ? conn_ms : left)); (*E)->CallVoidMethod(E, Cn, J.mReadTo, (jint)(left < 10000 ? left : 10000));
    (*E)->CallVoidMethod(E, Cn, J.mCaches, JNI_FALSE); if (jx(E, r->why, sizeof r->why)) goto out;
    (*E)->CallVoidMethod(E, Cn, J.mMethod, (*E)->NewStringUTF(E, method)); if (jx(E, r->why, sizeof r->why)) goto out;
    snprintf(auth, sizeof auth, "Bearer %s", C.cfg.token);
    int bad = jprop(E, Cn, "Authorization", auth); memset(auth, 0, sizeof auth);
    if (bad || jprop(E, Cn, "Accept-Encoding", "identity")) { snprintf(r->why, sizeof r->why, "a request header was refused"); goto out; }
    buf = (*E)->NewByteArray(E, 256 << 10); if (jx(E, r->why, sizeof r->why) || !buf) goto out;
    if (strcmp(method, "GET")) {   // PUT and POST: a body of a known length (POST's is empty)
        if (body_sha && jprop(E, Cn, "X-Save-Sha256", body_sha)) goto out;
        if (body && jprop(E, Cn, "Content-Type", "application/octet-stream")) goto out;
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
static void url_of(char *o, size_t n, const char *what) { snprintf(o, n, "%s/%s", C.base, what); }
enum { SESS_OPEN, SESS_ALIVE, SESS_CLOSED };
static int owed_now(void) { pthread_mutex_lock(&C.mu); int d = C.st.pending || C.st.conflict; pthread_mutex_unlock(&C.mu); return d; }   // a save the cloud has not got
// The session: state and dirty (a save still owed), 0 when the server took it. A heartbeat (alive) logs only when its outcome or its dirty changed
static int session_post(JNIEnv *E, int state, int dirty, int64_t deadline) {
    static const char *const NAME[] = {"open", "alive", "closed"};
    static int last_alive = -1;   // the last heartbeat's outcome (0 ok, 1 not), dirty in bit 1 (the upload thread's only)
    char u[1400], dev[200]; ss_qenc(dev, sizeof dev, C.cfg.device);
    snprintf(u, sizeof u, "%s/session?state=%s&dirty=%d&device=%s", C.base, NAME[state], dirty ? 1 : 0, dev);
    hres r; int got = http_do(E, "POST", u, NULL, 0, NULL, 3000, deadline, 64 << 10, &r, NULL); int ok = !got && r.code == 200;
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
} W;
void dm_cloud_view_get(dm_cloud_view *v) { pthread_mutex_lock(&C.mu); *v = W.v; pthread_mutex_unlock(&C.mu); }
static void up_start(void);
static void finish_locked(int quit, int skip) {   // the start ends: the screen goes, done() on the main thread
    if (W.done) return;
    W.done = 1; W.quit = quit; W.up_after = !quit && !skip;
    if (skip) { C.skipped = 1; ss_mark_offline_play(&C.st); st_save_locked(); changed_locked();
                LOG("cloud: this run plays without the sync (the next start asks before its save goes up)"); }
}
static void skipped_end(void) { dm_start_step(STV_CLOUD, STV_SKIP, korean() ? "건너뜀" : "Skipped"); dm_cloud_screen_end(); }   // B: the start ends without the sync
static void finish(int quit, int skip) {
    pthread_mutex_lock(&C.mu); int was = W.done; finish_locked(quit, skip); int off = C.offline; pthread_mutex_unlock(&C.mu);
    if (!was) dm_start_step(STV_CLOUD, skip ? STV_SKIP : off ? STV_SKIP : STV_DONE, skip ? (korean() ? "건너뜀" : "Skipped") : off ? (korean() ? "오프라인" : "Offline") : (korean() ? "완료" : "Done"));
    dm_cloud_screen_end();
}
static void screen_done(void) {   // the main thread, once the screen let the window go
    if (W.up_after) up_start();
    void (*d)(int) = W.done_cb; W.done_cb = NULL; if (d) d(W.quit);
}
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
    pthread_mutex_lock(&C.mu); if (total > 0) W.v.progress = (int)(done * 1000 / (uint64_t)total); W.v.ver++; pthread_mutex_unlock(&C.mu); dm_cloud_screen_poke();
}
static void size_line(char *o, size_t n, uint64_t size, int64_t files, int ko) {   // "1.2 MB · 파일 9개"; the size only when the count is not known
    char sz[32]; ss_fmt_size(sz, sizeof sz, size);
    if (files < 0) snprintf(o, n, "%s", sz); else if (ko) snprintf(o, n, "%s · 파일 %lld개", sz, (long long)files); else snprintf(o, n, "%s · %lld file%s", sz, (long long)files, files == 1 ? "" : "s");
}
// the two cards: this device's (0) and the server's (1), the newer one chosen first. local_size, local_files: this device's archive (its tar, as it would go up)
static void cards_view(const ss_meta *m, int64_t local_t, uint64_t local_size, int64_t local_files, int offline_play) {
    int ko = korean(); int64_t now = (int64_t)time(NULL); const char *md = m->device[0] ? m->device : "-";
    pthread_mutex_lock(&C.mu);
    ask_locked(DM_CLOUD_ICON_NONE, offline_play ? (ko ? "동기화 없이 플레이한 기록이 있어요" : "This device played without the sync") : (ko ? "어느 세이브로 할까요?" : "Which save do you want to keep?"),
               offline_play ? (ko ? "이 기기에서 동기화 없이 플레이한 진행을 어떻게 할까요?" : "What should happen to the progress played without the sync?")
                            : (ko ? "이 기기와 클라우드에 서로 다른 진행이 있어요" : "This device and the cloud have different progress"), NULL, 0, NULL, NULL,
               ko ? "고르지 않은 쪽도 백업해 둬요" : "The one you do not choose is kept as a backup", ko ? "← → 고르기 · A 확정 · B 동기화 없이 플레이" : "← → choose · A confirm · B play without the sync");
    dm_cloud_view *v = &W.v; v->ncards = 2;
    if (offline_play) { snprintf(v->card[0].head, sizeof v->card[0].head, ko ? "이 기록 올리기 · %s" : "Upload this play · %s", C.cfg.device);
                        snprintf(v->card[1].head, sizeof v->card[1].head, ko ? "클라우드로 되돌리기 · %s" : "Back to the cloud save · %s", md); }
    else { snprintf(v->card[0].head, sizeof v->card[0].head, ko ? "이 기기 · %s" : "This device · %s", C.cfg.device);
           snprintf(v->card[1].head, sizeof v->card[1].head, ko ? "클라우드 · %s에서 올림" : "Cloud · from %s", md); }
    ss_fmt_date(v->card[0].date, sizeof v->card[0].date, local_t, ko); ss_fmt_ago(v->card[0].ago, sizeof v->card[0].ago, local_t, now, ko);
    ss_fmt_date(v->card[1].date, sizeof v->card[1].date, m->time, ko); ss_fmt_ago(v->card[1].ago, sizeof v->card[1].ago, m->time, now, ko);
    size_line(v->card[0].size, sizeof v->card[0].size, local_size, local_files, ko);
    if (m->size) size_line(v->card[1].size, sizeof v->card[1].size, m->size, m->files, ko);   // (a server that keeps no file count: the size only)
    snprintf(v->badge, sizeof v->badge, "%s", ko ? "더 최근" : "More recent");
    int newer = m->time > local_t ? 1 : 0; v->card[newer].newer = 1; v->sel = newer;
    pthread_mutex_unlock(&C.mu); dm_cloud_screen_poke();
}
// The server's save: downloaded, checked, unpacked into saves/<stamp>-cloud and imported (saves_android.c: the current save backed up first). 0, -1 (why), -2 the start ended
static int download_apply(JNIEnv *E, const ss_meta *m, char *why, size_t wcap) {
    int ko = korean(); char u[1400], sha[65], err[600], name[64], dir[800], dird[820], part[820], partd[840];
    pthread_mutex_lock(&C.mu); wait_locked("받는 중…", "Downloading…"); W.v.progress = 0; pthread_mutex_unlock(&C.mu); dm_cloud_screen_poke();
    url_of(u, sizeof u, "data"); hres r;
    if (http_do(E, "GET", u, NULL, 0, NULL, 3000, now_ms() + 60000, SS_MAX_BODY, &r, dl_prog)) { snprintf(why, wcap, "%s", r.why); return ended() ? -2 : -1; }
    if (r.code != 200) { snprintf(why, wcap, "HTTP %d", r.code); ss_buf_free(&r.body); return -1; }
    ss_sha256(r.body.p, r.body.n, sha); for (char *q = r.sha; *q; q++) if (*q >= 'A' && *q <= 'F') *q += 32;
    if ((r.sha[0] && strcmp(r.sha, sha)) || (!r.sha[0] && strcmp(sha, m->sha))) { snprintf(why, wcap, "the download does not match its SHA-256 (damaged on the way)"); ss_buf_free(&r.body); return -1; }
    uint64_t seq = r.seq[0] ? strtoull(r.seq, NULL, 10) : m->seq; if (!seq) seq = m->seq;
    pthread_mutex_lock(&C.mu); int d = W.done; if (!d) { W.applying = 1; wait_locked("적용하는 중…", "Putting it in place…"); W.v.nbtn = 0; W.v.urgent = 1; }   // (B cannot stop it now: no button)
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
    else if (dm_saves_import_now(name)) snprintf(why, wcap, "the import failed (saves/result.txt says why)");
    else rc = 0;
    ss_buf_free(&r.body); ss_rm_tree(part); ss_rm_tree(partd); ss_rm_tree(dir); ss_rm_tree(dird);   // (the save before it stays: saves/<stamp>-before-import)
    if (rc) { pthread_mutex_lock(&C.mu); W.applying = 0; pthread_mutex_unlock(&C.mu); return -1; }
    ss_buf now_t; char now_sha[65] = ""; int64_t nw;
    if (!snap(&now_t, now_sha, &nw, err, sizeof err)) ss_buf_free(&now_t);
    if (strcmp(now_sha, sha)) LOG("[RED] cloud: the save imported is archived to %.12s, not the server's %.12s (a device save kept from before?): it goes up as it is now", now_sha, sha);
    pthread_mutex_lock(&C.mu);
    C.st.seq = seq; snprintf(C.st.sha, sizeof C.st.sha, "%s", now_sha); C.st.pending = strcmp(now_sha, sha) != 0; C.st.conflict = C.st.force = C.st.offline_play = 0; C.st.have = 1;
    st_save_locked(); W.applying = 0; changed_locked();
    pthread_mutex_unlock(&C.mu);
    LOG("cloud: the server's save (seq %llu, %zu bytes, from %s) is the save now", (unsigned long long)seq, (size_t)m->size, m->device[0] ? m->device : "-");
    return 0;
}
static void *start_thread(void *a) {
    (void)a; pthread_setname_np(pthread_self(), "cloudstart");
    JNIEnv *E = jattach(); int ko = korean();
    if (!E) { LOG("[RED] cloud: no Java for the sync: this run plays without it"); finish(0, 1); return NULL; }
    ss_buf t; char local[65], err[600], why[300]; int64_t local_t = 0, local_files = -1; uint64_t local_size = 0;
    int lr = snap(&t, local, &local_t, err, sizeof err); if (!lr) { local_size = t.n; local_files = ss_tar_files(t.p, t.n); ss_buf_free(&t); }
    if (lr < 0) { LOG("[RED] cloud: this device's save cannot be archived (%s): this run plays without the sync", err); finish(0, 1); goto out; }
    const char *loc = lr ? NULL : local;
    LOG("cloud: this device's save: %s", loc ? loc : "none");
    int dev_net = dm_net_state(E, C.act), data_unreached = dm_data_unreached();
    if (ss_start_offline(dev_net, data_unreached)) {   // no network on the device at all (a console without Wi-Fi), or the data's server was not reached just now: no
        // question, no request that can only time out; the game starts with this device's save at once ("Offline" in the steps), and the upload thread opens the
        // session and sends what is owed once the network is back
        ss_meta m; memset(&m, 0, sizeof m);
        pthread_mutex_lock(&C.mu); int p; ss_decide(SS_NET_FAIL, &m, &C.st, loc, &p); if (p) C.st.pending = 1; C.offline = 1; st_save_locked(); changed_locked(); pthread_mutex_unlock(&C.mu);
        LOG("cloud: %s: the game starts with this device's save%s; the sync goes on once it is back", dev_net == 0 ? "the device has no network" : "the data's server was not reached",
            p ? " (owed: it goes up then)" : "");
        finish(0, 0); goto out;
    }
    for (;;) {
        wait_view("맞추는 중…", "Syncing…");
        char u[1400]; url_of(u, sizeof u, "meta"); hres r; ss_meta m; memset(&m, 0, sizeof m); m.files = -1;
        int got = http_do(E, "GET", u, NULL, 0, NULL, 3000, now_ms() + 5000, 256 << 10, &r, NULL);
        if (ended()) { ss_buf_free(&r.body); break; }
        int net = got ? SS_NET_FAIL : r.code == 200 ? (ss_meta_parse((const char *)r.body.p, r.body.n, &m) ? SS_NET_FAIL : SS_NET_OK) : r.code == 404 ? (ss_meta_parse((const char *)r.body.p, r.body.n, &m), m.seq = 0, SS_NET_NOTFOUND)
                : r.code == 401 || r.code == 403 ? SS_NET_UNAUTH : SS_NET_FAIL;
        if (got) LOG("cloud: no answer from the server (%s): the game starts with this device's save unless the player retries", r.why);
        else if (net == SS_NET_FAIL) LOG("[RED] cloud: the server answered HTTP %d%s", r.code, r.code == 200 ? " with a meta that cannot be read" : "");
        // (a 404 still carries the session, {"error":"none","session":…}: a game another device runs before its first upload is seen too)
        ss_buf_free(&r.body);
        if (net == SS_NET_FAIL || net == SS_NET_UNAUTH) {
            pthread_mutex_lock(&C.mu); C.st.auth = net == SS_NET_UNAUTH; int p; ss_decide(net, &m, &C.st, loc, &p); if (p) C.st.pending = 1; C.offline = net == SS_NET_FAIL; st_save_locked(); changed_locked(); pthread_mutex_unlock(&C.mu);
            if (net == SS_NET_UNAUTH) {
                LOG("[RED] cloud: the server refused the token (sync.txt's token=, %.4s...)", C.cfg.token);
                ask_view(DM_CLOUD_ICON_WARN, ko ? "기기 연결이 끊겼어요 (토큰)" : "This device is no longer linked (token)", ko ? "sync.txt의 토큰을 확인해 주세요. 이 기기 세이브로 시작해요." : "Check the token in sync.txt. The game starts with this device's save.", NULL,
                         1, ko ? "그래도 플레이" : "Play anyway", NULL, NULL, NULL);
                if (ask(10000, "%d초 뒤 자동으로 시작해요", "Starting by itself in %d s") >= 0) finish(0, 0);
                break;
            }
            ask_view(DM_CLOUD_ICON_OFFLINE, ko ? "클라우드에 연결할 수 없어요" : "The cloud cannot be reached", ko ? "이 기기 세이브로 시작해요. 연결되면 나중에 올려요." : "The game starts with this device's save; it goes up once there is a connection.", NULL,
                     2, ko ? "그래도 플레이" : "Play anyway", ko ? "다시 시도" : "Retry", NULL, NULL);
            int c = ask(10000, "%d초 뒤 자동으로 시작해요", "Starting by itself in %d s");
            if (c == 1) continue;
            if (c >= 0) finish(0, 0);
            break;
        }
        pthread_mutex_lock(&C.mu); C.st.auth = 0; C.offline = 0; pthread_mutex_unlock(&C.mu);
        int other = net == SS_NET_OK || net == SS_NET_NOTFOUND ? ss_other_device(&m, C.cfg.device, (int64_t)time(NULL)) : SS_OTHER_NONE;
        if (other == SS_OTHER_UNSYNCED) {   // that device saved and the save did not go up: what the cloud has may be behind it
            char l0[200], when[64]; ss_fmt_date(when, sizeof when, m.s_updated, ko);
            snprintf(l0, sizeof l0, ko ? "%s에서 %s에 저장한 진행이 아직 클라우드에 없어요." : "The progress saved on %s at %s is not in the cloud yet.", m.s_device, when);
            LOG("cloud: %s has a save not yet uploaded (its last signal %s): the player is warned", m.s_device, when);
            ask_view(DM_CLOUD_ICON_WARN, ko ? "다른 기기에 아직 안 올라온 진행이 있어요" : "Another device has progress not uploaded yet", l0,
                     ko ? "그 기기를 인터넷에 연결해 켜면 올라가요." : "It goes up once that device is on and connected.",
                     2, ko ? "그래도 시작" : "Start anyway", ko ? "취소 (앱 종료)" : "Cancel (close the app)",
                     ko ? "그래도 시작하면 나중에 두 세이브 중 하나를 고르게 돼요 · B 동기화 없이 플레이" : "Start anyway, and later you choose one of the two saves · B play without the sync", NULL);
            int c = ask(0, NULL, NULL);
            if (c < 0) break;
            if (c == 1) { LOG("cloud: the player cancelled the start (%s has a save not yet uploaded)", m.s_device); finish(1, 0); break; }
        } else if (other == SS_OTHER_RUNNING) {   // running there now, its saves up: a note under the steps, no choice
            pthread_mutex_lock(&C.mu); wait_locked("맞추는 중…", "Syncing…");
            snprintf(W.v.hint, sizeof W.v.hint, ko ? "%s에서 지금 실행 중이에요 · 그 기기의 진행은 저장할 때마다 올라가요" : "Running on %s right now · its progress goes up each time it saves", m.s_device);
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
        static const char *const GO[] = {"up to date", "this device's save goes up", "no save on either", "the server's save is newer: download", "conflict", "offline", "token refused", "played without the sync: ask"};
        LOG("cloud: server seq %llu (%.12s, from %s), last synced seq %llu: %s", (unsigned long long)m.seq, m.seq ? m.sha : "-", m.device[0] ? m.device : "-", (unsigned long long)C.st.seq, GO[go]);
        int retry = 0;
        if (go == SS_GO_CONFLICT || go == SS_GO_ASK_OFFLINE) {
            cards_view(&m, local_t, local_size, local_files, go == SS_GO_ASK_OFFLINE);
            int c = ask(0, NULL, NULL);
            if (c < 0) break;
            if (c == 0) {   // this device's: the game starts with it and it goes up at once, over the server's (whose previous save stays in its history)
                LOG("cloud: the player chose this device's save over the server's seq %llu", (unsigned long long)m.seq);
                pthread_mutex_lock(&C.mu); if (W.done) { pthread_mutex_unlock(&C.mu); break; }
                C.st.force = 1; C.st.fbase = m.seq; C.st.pending = 1; C.st.conflict = C.st.offline_play = 0; C.st.have = 1; C.kick = 1; st_save_locked(); changed_locked(); pthread_mutex_unlock(&C.mu);
            } else go = SS_GO_DOWNLOAD;
        }
        if (go == SS_GO_DOWNLOAD) {
            int d = download_apply(E, &m, why, sizeof why);
            if (d == -2) break;
            if (d) {
                LOG("[RED] cloud: the server's save could not be taken: %s", why);
                ask_view(DM_CLOUD_ICON_WARN, ko ? "클라우드 세이브를 받지 못했어요" : "The cloud save could not be downloaded", ko ? "이 기기 세이브로 시작할 수 있어요." : "The game can start with this device's save.", why,
                         2, ko ? "그래도 플레이" : "Play anyway", ko ? "다시 시도" : "Retry", NULL, NULL);
                int c = ask(10000, "%d초 뒤 자동으로 시작해요", "Starting by itself in %d s");
                if (c == 1) retry = 1;
                else { if (c >= 0) finish(0, 0); break; }
            }
        } else if (go != SS_GO_CONFLICT && go != SS_GO_ASK_OFFLINE) {
            pthread_mutex_lock(&C.mu); if (W.done) { pthread_mutex_unlock(&C.mu); break; }
            if (go == SS_GO_UPTODATE) { C.st.seq = m.seq; snprintf(C.st.sha, sizeof C.st.sha, "%s", loc ? loc : ""); C.st.pending = 0; C.st.conflict = C.st.force = 0; }
            else if (go == SS_GO_PENDING) { if (net == SS_NET_NOTFOUND) { C.st.seq = 0; C.st.force = 0; } C.st.pending = 1; C.st.conflict = 0; C.kick = 1; }
            else if (go == SS_GO_NOTHING) { C.st.seq = 0; C.st.sha[0] = 0; C.st.pending = C.st.conflict = C.st.force = 0; }
            C.st.offline_play = 0; C.st.have = 1; st_save_locked(); changed_locked(); pthread_mutex_unlock(&C.mu);
        }
        if (retry) continue;
        if (ended()) break;
        if (!session_post(E, SESS_OPEN, 0, now_ms() + 3000)) { pthread_mutex_lock(&C.mu); C.sess_open = 1; pthread_mutex_unlock(&C.mu); }   // (just synced: clean. A failure: the upload thread opens it)
        finish(0, 0);
        break;
    }
out:
    (*C.vm)->DetachCurrentThread(C.vm);
    return NULL;
}

// The start screen's input (front_android.c on_input_early, the main thread): the d-pad or a stick's hat moves between the buttons or cards, A (or Enter) chooses, B (or Back)
// plays without the sync; a touch chooses what it lifts on. The volume keys stay the system's
static void input_choose(int i) {
    pthread_mutex_lock(&C.mu);
    if (W.done || W.applying) { pthread_mutex_unlock(&C.mu); return; }
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
    pthread_mutex_lock(&C.mu); int ok = !W.done && !W.applying; if (ok) finish_locked(0, 1); pthread_mutex_unlock(&C.mu);
    if (ok) skipped_end();
}
int dm_cloud_input(const AInputEvent *e) {
    pthread_mutex_lock(&C.mu); int on = W.done_cb != NULL && !W.done; pthread_mutex_unlock(&C.mu);
    if (!on) return 0;
    if (AInputEvent_getType(e) == AINPUT_EVENT_TYPE_KEY) {
        int k = AKeyEvent_getKeyCode(e), act = AKeyEvent_getAction(e);
        if (k == AKEYCODE_VOLUME_UP || k == AKEYCODE_VOLUME_DOWN || k == AKEYCODE_VOLUME_MUTE) return 0;
        if (act != AKEY_EVENT_ACTION_DOWN || AKeyEvent_getRepeatCount(e)) return 1;
        if (k == AKEYCODE_DPAD_LEFT || k == AKEYCODE_DPAD_UP) input_move(-1);
        else if (k == AKEYCODE_DPAD_RIGHT || k == AKEYCODE_DPAD_DOWN) input_move(1);
        else if (k == AKEYCODE_BUTTON_A || k == AKEYCODE_DPAD_CENTER || k == AKEYCODE_ENTER || k == AKEYCODE_NUMPAD_ENTER || k == AKEYCODE_BUTTON_START) {
            pthread_mutex_lock(&C.mu); int s = W.v.sel; pthread_mutex_unlock(&C.mu); input_choose(s); }
        else if (k == AKEYCODE_BUTTON_B || k == AKEYCODE_BACK || k == AKEYCODE_ESCAPE) input_back();
        return 1;
    }
    if (AInputEvent_getType(e) != AINPUT_EVENT_TYPE_MOTION) return 1;
    int src = AInputEvent_getSource(e), act = AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_MASK;
    if ((src & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK) {   // the hat (a d-pad as axes) and the left stick: one step per push
        static int held; float x = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_X, 0), y = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_Y, 0);
        float sx = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_X, 0); if (fabsf(x) < 0.5f && fabsf(y) < 0.5f && fabsf(sx) > 0.7f) x = sx;
        int d = x < -0.5f || y < -0.5f ? -1 : x > 0.5f || y > 0.5f ? 1 : 0;
        if (d && d != held) input_move(d);
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
enum { UP_NOTHING, UP_OK, UP_FAIL, UP_CONFLICT, UP_ERR };
static int upload(JNIEnv *E, int64_t deadline) {
    static const int64_t BACKOFF[] = {60000, 120000, 300000, 600000};
    pthread_mutex_lock(&C.mu);
    int skip = C.mode != SS_RUN_SYNC || C.skipped || C.st.off || C.st.conflict || C.st.auth || C.st.offline_play, force = C.st.force; uint64_t base = force ? C.st.fbase : C.st.seq; char last[65];
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
    pthread_mutex_lock(&C.mu); C.st.pending = 1; C.uploading = 1; ss_up_begin(&C.notes, now_ms()); changed_locked(); pthread_mutex_unlock(&C.mu); menu_redraw();   // the spinner
    int rc = UP_ERR, note = SS_NOTE_NONE;
    for (int round = 0; round < 2; round++) {
        char u[1700], dev[200], gv[100], bd[100], fc[32] = ""; ss_qenc(dev, sizeof dev, C.cfg.device); ss_qenc(gv, sizeof gv, C.gver); ss_qenc(bd, sizeof bd, C.build);
        if (files >= 0) snprintf(fc, sizeof fc, "&files=%lld", (long long)files);
        snprintf(u, sizeof u, "%s/data?base=%llu&device=%s&gameVersion=%s&build=%s%s", C.base, (unsigned long long)base, dev, gv, bd, fc);
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
            note = ss_after_put(&C.st, 200, seq, sha, NULL, (int64_t)time(NULL));
            C.fails = 0; C.offline = 0; rc = UP_OK;
            LOG("cloud: the save went up: seq %llu, %zu bytes, %lld files, %.12s", (unsigned long long)seq, t.n, (long long)files, sha);
        } else if (h.code == 409) {
            int k = ss_conflict_parse((const char *)h.body.p, h.body.n, &m);
            if (k == 0 && base && round == 0) {   // the server has no save (it was cleared): again with base 0
                LOG("cloud: the server has no save any more: sent again as the first one"); base = 0; C.st.seq = 0; C.st.force = 0;
                pthread_mutex_unlock(&C.mu); ss_buf_free(&h.body); continue;
            }
            note = ss_after_put(&C.st, 409, 0, sha, &m, 0); C.offline = 0; rc = UP_CONFLICT;   // never overwritten, never asked while the game runs: the next start's cards
            LOG("[RED] cloud: the server moved on (seq %llu from %s, this device's base %llu): a conflict, the next start asks which save to keep", (unsigned long long)m.seq, m.device[0] ? m.device : "-", (unsigned long long)base);
        } else if (h.code == 401 || h.code == 403) { ss_after_put(&C.st, h.code, 0, sha, NULL, 0); rc = UP_ERR; LOG("[RED] cloud: the server refused the token (sync.txt's token=, %.4s...)", C.cfg.token); }
        else { C.next_try = now_ms() + (h.code == 413 ? 24ll * 3600 * 1000 : BACKOFF[3]); rc = UP_ERR;
               LOG("[RED] cloud: the upload was refused: HTTP %d%s", h.code, h.code == 400 ? " (the hash did not match)" : h.code == 413 ? " (the save is too big for the server)" : ""); }
        C.uploading = 0; ss_up_end(&C.notes, note, now_ms()); st_save_locked(); changed_locked();   // its check (once no upload follows within 3 s), or its note
        pthread_mutex_unlock(&C.mu); ss_buf_free(&h.body);
        break;
    }
    pthread_mutex_lock(&C.mu); C.uploading = 0; pthread_mutex_unlock(&C.mu);
    ss_buf_free(&t); menu_redraw();
    return rc;
}
// The app leaves the screen: what is owed goes up now, then the session closes with what is still owed; under a 15 s wake lock
static void pause_flush(JNIEnv *E, int *dirty, int syncs, const ss_tick_in *ti) {
    int64_t dl = now_ms() + 15000;
    if ((!syncs && !ti->sess_open) || dm_net_state(E, C.act) == 0) { if (syncs) LOG("cloud: the app leaves the screen without a network: what is owed goes up once it is back"); return; }
    wake_lock(E, 1, 15000);
    if (*dirty || ti->owed) { *dirty = 0; upload(E, dl - 1000); }
    int owed = owed_now();
    if (owed) LOG("cloud: a save is still owed (it could not go up): the session closes dirty, another device warns before it starts");
    session_post(E, SESS_CLOSED, owed, dl);   // (no answer: the session's last signal stands, and stops being "running" within 3 min)
    pthread_mutex_lock(&C.mu); C.sess_open = 0; pthread_mutex_unlock(&C.mu);
    wake_lock(E, 0, 0);
}
// The upload thread: the commit count polled every 1.5 s; a burst of commits goes up once a poll saw none (about 3 s after the game saved). Each tick asks savesync.h
// ss_tick what to do: with no network on the device nothing is tried; when it comes back an owed save goes up at once (its backoff forgotten) and a session that never
// opened (an offline start) opens, then the heartbeats every 60 s
static void *up_thread(void *a) {
    (void)a; pthread_setname_np(pthread_self(), "savesync");
    JNIEnv *E = jattach(); if (!E) { LOG("[RED] cloud: no Java for the uploads: the save does not go up this run"); return NULL; }
    uint64_t lastc = dm_fs_commit_count(); int dirty = 0, paused = 0, was_net = -1; int64_t changed_t = 0, last_hb = now_ms();   // (the start just opened the session, if it could)
    LOG("cloud: uploads on (about 3 s after the game commits its save, at once when the network comes back, and when the app leaves the screen); a heartbeat every 60 s while on screen");
    for (;;) {
        pthread_mutex_lock(&C.mu);
        if (!C.kick && !C.pause_ev && !C.resume_ev) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += 500 * 1000000; ts.tv_sec += 1; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&C.cv, &C.mu, &ts); }   // 1.5 s
        int kick = C.kick, pause = C.pause_ev, resume = C.resume_ev; C.kick = C.pause_ev = C.resume_ev = 0;
        int syncs = C.mode == SS_RUN_SYNC && !C.skipped && !C.st.off && !C.st.auth && !C.st.offline_play;
        ss_tick_in ti = {.syncs = syncs, .sess_open = C.sess_open, .owed = C.st.pending, .conflict = C.st.conflict, .next_try = C.next_try};
        pthread_mutex_unlock(&C.mu);
        uint64_t c = dm_fs_commit_count(); int64_t t = now_ms();
        if (c != lastc) { lastc = c; dirty = 1; changed_t = t; }
        if (pause) {   // the app leaves the screen (sleep, HOME, its end, the menu's Exit game): what is owed goes up now, then the session closes (pause_flush)
            paused = 1; pause_flush(E, &dirty, syncs, &ti);
            pthread_mutex_lock(&C.mu); C.pause_done++; pthread_cond_broadcast(&C.cv); pthread_mutex_unlock(&C.mu);   // (dm_cloud_flush waits for it)
            continue;
        }
        if (resume) paused = 0;
        if (paused) continue;
        ti.committed = kick || (dirty && t - changed_t >= 1500); ti.now = t; ti.last_hb = last_hb;
        // the device's network is read only when this tick may send something or it was down (a game that saves rarely, online, with its session open asks Java nothing
        // between its heartbeats)
        int ask_net = syncs && (ti.committed || ti.owed || !ti.sess_open || resume || was_net == 0 || t - last_hb >= 60000);
        ti.net = ask_net ? dm_net_state(E, C.act) : syncs ? was_net : -1; ti.was_net = was_net; if (ask_net) was_net = ti.net;
        if (resume && ti.net != 0) { ti.owed = ti.owed || dirty; ti.next_try = 0; }   // back in front: what is owed is tried now
        int act = ss_tick(&ti);
        if (act & SS_DO_RETRY_NOW) { LOG("cloud: the network is back%s", ti.owed || dirty ? ": what is owed goes up now" : ""); pthread_mutex_lock(&C.mu); C.next_try = 0; C.fails = 0; pthread_mutex_unlock(&C.mu); }
        int up = UP_NOTHING;
        if (act & SS_DO_UPLOAD) { dirty = 0; up = upload(E, now_ms() + 60000); }
        if ((act & SS_DO_OPEN) || resume) {   // an offline start, or the app back in front: the session opens (dirty while a save is owed)
            if (syncs && ti.net != 0 && !session_post(E, SESS_OPEN, owed_now(), now_ms() + 5000)) { pthread_mutex_lock(&C.mu); C.sess_open = 1; pthread_mutex_unlock(&C.mu); }
            last_hb = now_ms();
        } else if ((act & SS_DO_ALIVE) || (up == UP_OK && ti.sess_open)) { session_post(E, SESS_ALIVE, owed_now(), now_ms() + 5000); last_hb = now_ms(); }
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

// ---- the pause menu's row and the corner note ----
int dm_cloud_available(void) { return C.mode != SS_RUN_NONE; }
int dm_cloud_status(char *note, int cap) {
    pthread_mutex_lock(&C.mu); note[0] = 0;
    int s = C.mode == SS_RUN_NONE ? DM_CLOUD_OFF : C.st.off ? DM_CLOUD_DISABLED : C.mode == SS_RUN_SUPPRESSED ? (C.trial ? DM_CLOUD_TRIAL : DM_CLOUD_DISABLED) : C.skipped || C.st.offline_play ? DM_CLOUD_SKIPPED
          : C.uploading ? DM_CLOUD_UPLOADING : C.st.auth ? DM_CLOUD_AUTH : C.st.conflict ? DM_CLOUD_CONFLICT : C.st.pending ? (C.offline ? DM_CLOUD_OFFLINE : DM_CLOUD_PENDING) : DM_CLOUD_UPTODATE;
    if (C.st.last_up > 0) { time_t t = (time_t)C.st.last_up; struct tm tm; localtime_r(&t, &tm); strftime(note, (size_t)cap, "%H:%M", &tm); }
    if (s == DM_CLOUD_DISABLED && !C.st.off) snprintf(note, (size_t)cap, "on at next start");
    pthread_mutex_unlock(&C.mu); return s;
}
unsigned dm_cloud_menu_ver(void) { pthread_mutex_lock(&C.mu); unsigned v = C.ver; pthread_mutex_unlock(&C.mu); return v; }
void dm_cloud_menu_press(void) {
    pthread_mutex_lock(&C.mu); int ok = C.up_started && C.st.pending && !C.st.conflict && !C.uploading && !C.st.off;
    if (ok) { C.kick = 1; C.next_try = 0; pthread_cond_broadcast(&C.cv); LOG("[menu] cloud save: upload now"); }
    pthread_mutex_unlock(&C.mu);
}
void dm_cloud_menu_toggle(void) {   // this game's sync off (from now on, and at each start until it is on again) or on (at the next start)
    pthread_mutex_lock(&C.mu);
    if (C.mode != SS_RUN_NONE) {
        C.st.off = !C.st.off; if (C.st.off) ss_mark_offline_play(&C.st);
        st_save_locked(); changed_locked();
        LOG("[menu] cloud save %s", C.st.off ? "switched off for this game" : "switched on for this game (from the next start)");
    }
    pthread_mutex_unlock(&C.mu); menu_redraw();
}
// The game's corner mark (gfx_vk_present.c ov_mark, at each present): the spinner from an upload's start (the upload thread's ss_up_begin) on while uploads follow
// each other, then the check, or the waiting or conflict note (savesync.h ss_mark_now). None while the app leaves the screen. NONE: nothing is drawn. *age: ms since
// the mark began
int dm_cloud_mark(int64_t *age) {
    pthread_mutex_lock(&C.mu); int64_t a = 0; int k = ss_mark_now(&C.notes, now_ms(), &a);
    if (k == SS_MARK_BUSY && C.pause_ev) k = DM_CLOUD_MARK_NONE;
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
    jx(E, NULL, 0); (*E)->PopLocalFrame(E, NULL);
}
int dm_cloud_begin(ANativeActivity *act, ANativeWindow *w, void (*done)(int quit)) {
    const char *root = getenv("DM_ROOT"); char p[700]; size_t n = 0;
    if (!root) return 0;
    snprintf(p, sizeof p, "%s/sync.txt", root); char *txt = ds_read(p, &n, 1 << 16);
    if (!txt) return 0;   // no sync.txt: as before the sync existed
    int on = ss_cfg_text(txt, &C.cfg); memset(txt, 0, n); free(txt);
    if (!on) { LOG("[RED] cloud: %s has no url= (http:// or https://) and token=: the sync is off", p); return 0; }
    C.vm = act->vm; C.act = act->clazz;
    int dbg = 0; app_info(act, &dbg);
    AAsset *as = act->assetManager ? AAssetManager_open(act->assetManager, "nx2a/source.bin", AASSET_MODE_UNKNOWN) : NULL; if (as) AAsset_close(as);
    const char *env = getenv("DM_SAVESYNC"); int env_off = env && (!strcmp(env, "off") || !strcmp(env, "0"));
    C.trial = !as || dbg || env_off;
    const char *gv = getenv("DM_GAME_VERSION"); snprintf(C.gver, sizeof C.gver, "%s", gv ? gv : "");
    snprintf(C.state_path, sizeof C.state_path, "%s/sync-state.txt", dm_saves_dir());
    char pk[600]; ss_qenc(pk, sizeof pk, C.pkg); snprintf(C.base, sizeof C.base, "%s/api/saves/g/%s", C.cfg.url, pk);
    pthread_mutex_lock(&C.mu);
    ss_state_load(C.state_path, &C.st);
    C.mode = ss_run_mode(1, C.trial, C.st.off);
    LOG("cloud: %s, device %s, token %.4s... (%s), package %s, build %s%s", C.cfg.url, C.cfg.device, C.cfg.token, C.st.have ? "state kept" : "first setup", C.pkg, C.build,
        C.mode == SS_RUN_SYNC ? "" : C.trial ? (!as ? ": a trial build (no data server): no sync this run" : dbg ? ": a trial build (debuggable): no sync this run" : ": DM_SAVESYNC=off: no sync this run")
        : ": switched off for this game in the pause menu");
    if (C.mode == SS_RUN_SUPPRESSED) {   // no network at all; the next start with the sync asks before what this run saves goes up
        ss_mark_offline_play(&C.st); st_save_locked();
        changed_locked(); pthread_mutex_unlock(&C.mu);
        dm_start_step(STV_CLOUD, STV_SKIP, korean() ? (C.trial ? "시험판" : "꺼짐") : (C.trial ? "Trial build" : "Off")); return 0;
    }
    memset(&W, 0, sizeof W); W.choice = -1; W.done_cb = done;
    wait_locked("맞추는 중…", "Syncing…");
    pthread_mutex_unlock(&C.mu);
    if (!dm_cloud_screen_begin(act, w, screen_done)) {   // no screen to ask on: this run without the sync (never a start that waits on nothing)
        LOG("[RED] cloud: no start screen: this run plays without the sync");
        pthread_mutex_lock(&C.mu); W.done_cb = NULL; finish_locked(0, 1); pthread_mutex_unlock(&C.mu); return 0;
    }
    pthread_t t;
    if (pthread_create(&t, NULL, start_thread, NULL)) { LOG("[RED] cloud: cannot start the sync thread: this run plays without it"); finish(0, 1); }
    else pthread_detach(t);
    return 1;
}
