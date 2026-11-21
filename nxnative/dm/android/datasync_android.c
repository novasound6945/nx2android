// Android port: the app's data downloaded from a web server at launch (an APK ported with porter --data-url; datasync.h compares and downloads, this is its transport and
// its screen). The APK's asset nx2a/source.bin (porter/publish.py source_asset) holds the server's folder and the Basic authentication's user:password, masked only so the
// password does not show as plain text; neither the password nor the Authorization header is ever logged. Without the asset nothing here runs: the app starts as before.
// HTTP: java.net.HttpURLConnection through JNI (framework classes, no dex of the app's own): the system's DNS, proxy, TLS trust store (a server's Let's Encrypt certificate)
// and redirects, which a socket of our own would each have to redo; cleartext http:// only with mkapk.py --cleartext (Android 9+ refuses it otherwise).
// The screen: the start screen (startscreen.c draws it: the steps game data, shaders, cloud save, a progress block, the cloud's questions), one for the whole start.
// No game is running yet, so the window gets a GLES 2 picture (libEGL / libGLESv2 opened here): EGL leaves the window when its surface is destroyed, and the game's
// Vulkan swapchain can take it after (a window drawn with ANativeWindow_lock stays the CPU's and Vulkan cannot connect to it).
// The front end (front_android.c) starts the game when the sync ends: done, nothing to do, or no server and a whole pack in the files folder (offline play)
// An APK without the asset runs the sync too when its files folder has a bundles folder (a pack whose small files porter install pushed in bundles: datasync.h), with
// no server: the bundles are unpacked, then the game starts
// The same screen shows the shader preparation (dm_prep_begin: gfx_vk_gpl.inc vk_gpl_prepare), after the sync or without one, before the game starts, as its second step
// An asset of version 2 (porter port --data-pointer) names a pointer file as well: the sync thread reads it before files.json (pointer_resolve), without the Authorization
// header, and takes the server root and credentials it holds (+ the pack's folder under the root); without an answer, the pointer line it last read (kept in the app's
// internal storage, encrypted as it came); without that, the asset's own address. An asset of version 1 never goes that way
// The same screen shows the save's cloud sync at the start (savecloud_android.c dm_cloud_begin, after the shader preparation): its view (savecloud.h dm_cloud_view, which
// that file fills) is the third step, or a question of its own; the screen shows only once the start took 300 ms, or at once for a question
#include "dm.h"
#include "datasync.h"
#include "savesync.h"   // (the launcher's times and sizes: ss_fmt_when, ss_fmt_ago, ss_fmt_size)
#include "savecloud.h"
#include "pixel_font.h"
#include "startscreen.h"
#include <android/keycodes.h>
#include <sys/stat.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <android/asset_manager.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <jni.h>
#include <math.h>
#include <pthread.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

const uint8_t *dm_overlay_font(void);   // overlay_theme.c
static struct {
    pthread_mutex_t mu; pthread_cond_t cv;
    ANativeActivity *act; char root[512], package[256], base[1024], auth[600];
    ANativeWindow *win; int lost, released;           // the window (on_window_created / destroyed: dm_sync_window), and a destroy waiting for the screen to let it go
    ds_progress pr; int status, wait_s, wait_tot; char msg[256];   // status: -1 working, else the last error (DS_ERR_*); wait_s: seconds to the next try (of wait_tot)
    int retry_now, netdown; int64_t deadline;   // Retry pressed; the device has no network (nothing is asked); the time the server's answer is given up at (0: none)
    void (*done)(void); int pipe[2];
} S = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, .status = -1, .pipe = {-1, -1}};
static struct { int on, done, total; } P;   // the shader preparation (dm_prep_begin), under S.mu
static struct { char ko[3][200], en[3][200], detail[300]; } NT;   // a notice the start stops at (dm_notice_begin, PH_NOTICE), under S.mu: shown until the app ends
// the pointer (an asset of version 2): its address, key and the pack's folder under the root; the asset's own server (base, auth) for when neither the pointer nor the
// cache answers; cache: the file in the app's internal storage the last pointer line read is kept in (as it came, encrypted). fresh: the pointer itself answered this run
static struct { int on, fresh; char pointer[1024], sub[1024], base[1024], auth[600], cache[600]; uint8_t key[32]; } PT;
static int64_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }

// ---- JNI ----
static int jx(JNIEnv *E, char *why, size_t cap) {   // an exception: cleared, its toString() in why (an IOException's message: "Cleartext HTTP traffic … not permitted", a timeout)
    if (!(*E)->ExceptionCheck(E)) return 0;
    jthrowable t = (*E)->ExceptionOccurred(E); (*E)->ExceptionClear(E);
    if (why && cap) { why[0] = 0;
        jclass c = t ? (*E)->FindClass(E, "java/lang/Object") : NULL; jmethodID m = c ? (*E)->GetMethodID(E, c, "toString", "()Ljava/lang/String;") : NULL;
        jstring s = m ? (*E)->CallObjectMethod(E, t, m) : NULL; if ((*E)->ExceptionCheck(E)) { (*E)->ExceptionClear(E); s = NULL; }
        const char *u = s ? (*E)->GetStringUTFChars(E, s, NULL) : NULL; if (u) { snprintf(why, cap, "%s", u); (*E)->ReleaseStringUTFChars(E, s, u); } }
    return 1;
}
typedef struct { JNIEnv *E; jclass cUrl, cConn; jmethodID mUrl, mOpen, mConnTo, mReadTo, mProp, mCaches, mCode, mHeader, mLen, mIn, mDisc, mRead, mClose; jobject buf; } jhttp;
static int jhttp_init(jhttp *h) {
    JNIEnv *E = h->E; char why[256];
    jclass u = (*E)->FindClass(E, "java/net/URL"), c = (*E)->FindClass(E, "java/net/HttpURLConnection"), is = (*E)->FindClass(E, "java/io/InputStream");
    if (jx(E, why, sizeof why) || !u || !c || !is) { LOG("[RED] data: no java.net classes (%s)", why); return -1; }
    h->cUrl = (*E)->NewGlobalRef(E, u); h->cConn = (*E)->NewGlobalRef(E, c);
    h->mUrl = (*E)->GetMethodID(E, u, "<init>", "(Ljava/lang/String;)V"); h->mOpen = (*E)->GetMethodID(E, u, "openConnection", "()Ljava/net/URLConnection;");
    h->mConnTo = (*E)->GetMethodID(E, c, "setConnectTimeout", "(I)V"); h->mReadTo = (*E)->GetMethodID(E, c, "setReadTimeout", "(I)V");
    h->mProp = (*E)->GetMethodID(E, c, "setRequestProperty", "(Ljava/lang/String;Ljava/lang/String;)V"); h->mCaches = (*E)->GetMethodID(E, c, "setUseCaches", "(Z)V");
    h->mCode = (*E)->GetMethodID(E, c, "getResponseCode", "()I"); h->mHeader = (*E)->GetMethodID(E, c, "getHeaderField", "(Ljava/lang/String;)Ljava/lang/String;");
    h->mLen = (*E)->GetMethodID(E, c, "getContentLengthLong", "()J"); h->mIn = (*E)->GetMethodID(E, c, "getInputStream", "()Ljava/io/InputStream;");
    h->mDisc = (*E)->GetMethodID(E, c, "disconnect", "()V"); h->mRead = (*E)->GetMethodID(E, is, "read", "([B)I"); h->mClose = (*E)->GetMethodID(E, is, "close", "()V");
    jbyteArray b = (*E)->NewByteArray(E, 256 << 10); h->buf = b ? (*E)->NewGlobalRef(E, b) : NULL;
    if (jx(E, why, sizeof why) || !h->mUrl || !h->mOpen || !h->mCode || !h->mIn || !h->mRead || !h->buf) { LOG("[RED] data: java.net methods missing (%s)", why); return -1; }
    return 0;
}
static int jprop(jhttp *h, jobject c, const char *k, const char *v, char *why, size_t cap) {   // 0, or -1 with the exception's text
    JNIEnv *E = h->E; jstring a = (*E)->NewStringUTF(E, k), b = a ? (*E)->NewStringUTF(E, v) : NULL; if (b) (*E)->CallVoidMethod(E, c, h->mProp, a, b);
    return jx(E, why, cap) || !b ? -1 : 0;
}
static void jheader(jhttp *h, jobject C, const char *k, char *out, size_t cap) {   // a response header's value; "" when the answer has none (or one longer than cap - 1)
    JNIEnv *E = h->E; out[0] = 0;
    jstring n = (*E)->NewStringUTF(E, k); jstring v = n ? (*E)->CallObjectMethod(E, C, h->mHeader, n) : NULL;
    if (jx(E, NULL, 0) || !v) return;
    const char *s = (*E)->GetStringUTFChars(E, v, NULL); if (!s) { jx(E, NULL, 0); return; }
    size_t len = strlen(s); if (len < cap) memcpy(out, s, len + 1);
    (*E)->ReleaseStringUTFChars(E, v, s);
}
// What is left of the time the server gets (ms): none when the device has no network; S.deadline (3 s from the first try when the folder has a pack) else 3 s a request
static int64_t budget_left(void) {
    pthread_mutex_lock(&S.mu); int down = S.netdown; int64_t d = S.deadline; pthread_mutex_unlock(&S.mu);
    if (down) return 0;
    if (!d) return 3000;
    int64_t l = d - now_ms(); return l < 300 ? 0 : l;
}
// cond / got (datasync.h ds_fetch_fn): files.json asked for only if it changed since the copy the app has. HttpURLConnection passes the headers as they are and returns
// the server's 304 (setUseCaches(false), and the app installs no HttpResponseCache, that would answer it itself); a server that does not honour them answers 200 and
// sends the list whole, as before
static int http_fetch(void *u, const char *rel, uint64_t from, uint64_t *start, int64_t *total, int *http, const ds_val *cond, ds_val *got, ds_sink sink, void *su) {
    jhttp *h = u; JNIEnv *E = h->E; char url[2048], why[256] = "", rng[48]; static uint8_t chunk[256 << 10];
    if (got) memset(got, 0, sizeof *got);
    if (ds_url(url, sizeof url, S.base, rel)) { LOG("[RED] data: the address of %s is too long", rel); return DS_ERR_SERVER; }
    if ((*E)->PushLocalFrame(E, 48)) { jx(E, NULL, 0); return DS_ERR_NET; }
    int rc = DS_ERR_NET, probe = !strcmp(rel, DS_LIST);   // the list: short timeouts, so an app without the server starts the game soon (offline play)
    int64_t left = probe ? budget_left() : 1 << 30;
    if (left <= 0) { (*E)->PopLocalFrame(E, NULL); LOG("data: %s: %s", rel, S.netdown ? "the device has no network" : "out of the 3 s the server gets"); return DS_ERR_NET; }
    int cto = probe ? (int)(left < 3000 ? left : 3000) : 10000, rto = probe ? (int)(left < 3000 ? left : 3000) : 30000;
    jobject U = (*E)->NewObject(E, h->cUrl, h->mUrl, (*E)->NewStringUTF(E, url)); jobject C = U && !jx(E, why, sizeof why) ? (*E)->CallObjectMethod(E, U, h->mOpen) : NULL;
    jobject in = NULL;
    if (!C || jx(E, why, sizeof why) || !(*E)->IsInstanceOf(E, C, h->cConn)) goto out;
    (*E)->CallVoidMethod(E, C, h->mConnTo, cto); if (jx(E, why, sizeof why)) goto out;
    (*E)->CallVoidMethod(E, C, h->mReadTo, rto); if (jx(E, why, sizeof why)) goto out;
    (*E)->CallVoidMethod(E, C, h->mCaches, JNI_FALSE); if (jx(E, why, sizeof why)) goto out;
    if (S.auth[0] && jprop(h, C, "Authorization", S.auth, why, sizeof why)) goto out;
    if (jprop(h, C, "Accept-Encoding", "identity", why, sizeof why)) goto out;   // the bytes as they are on the server: Range offsets and sizes are the file's
    if (from) { snprintf(rng, sizeof rng, "bytes=%llu-", (unsigned long long)from); if (jprop(h, C, "Range", rng, why, sizeof why)) goto out; }
    if (cond && cond->etag[0] && jprop(h, C, "If-None-Match", cond->etag, why, sizeof why)) goto out;
    if (cond && cond->modified[0] && jprop(h, C, "If-Modified-Since", cond->modified, why, sizeof why)) goto out;
    int code = (*E)->CallIntMethod(E, C, h->mCode); if (jx(E, why, sizeof why)) goto out;
    *http = code;
    if (got && (code == 200 || code == 304)) { jheader(h, C, "ETag", got->etag, sizeof got->etag); jheader(h, C, "Last-Modified", got->modified, sizeof got->modified); }
    if (code == 304 && cond) { *start = 0; *total = -1; rc = 0; goto out; }   // not modified: no body
    if (code == 206) {
        jstring cr = (*E)->CallObjectMethod(E, C, h->mHeader, (*E)->NewStringUTF(E, "Content-Range")); jx(E, NULL, 0);
        const char *s = cr ? (*E)->GetStringUTFChars(E, cr, NULL) : NULL; unsigned long long a = 0, b = 0, t = 0;
        int ok = s && sscanf(s, "bytes %llu-%llu/%llu", &a, &b, &t) >= 2; if (s) (*E)->ReleaseStringUTFChars(E, cr, s);
        if (!ok) { snprintf(why, sizeof why, "HTTP 206 without a Content-Range"); rc = DS_ERR_SERVER; goto out; }
        *start = a; *total = t ? (int64_t)t : -1;
    } else if (code == 200) { *start = 0; jlong n = (*E)->CallLongMethod(E, C, h->mLen); jx(E, NULL, 0); *total = n; }
    else { rc = DS_ERR_SERVER; snprintf(why, sizeof why, "HTTP %d", code); goto out; }
    in = (*E)->CallObjectMethod(E, C, h->mIn); if (!in || jx(E, why, sizeof why)) goto out;
    for (;;) {
        jint n = (*E)->CallIntMethod(E, in, h->mRead, h->buf); if (jx(E, why, sizeof why)) goto out;
        if (n < 0) break;
        if (!n) continue;
        (*E)->GetByteArrayRegion(E, h->buf, 0, n, (jbyte *)chunk);
        if (sink(su, chunk, (size_t)n)) { snprintf(why, sizeof why, "the download was stopped (more bytes than the list says, or a full disk)"); goto out; }
    }
    rc = 0;
out:
    if (in) { (*E)->CallVoidMethod(E, in, h->mClose); jx(E, NULL, 0); }
    if (C && h->mDisc) { (*E)->CallVoidMethod(E, C, h->mDisc); jx(E, NULL, 0); }
    (*E)->PopLocalFrame(E, NULL);
    if (rc) LOG("data: %s%s: %s", rel, from ? " (resumed)" : "", why[0] ? why : "failed");   // the address only: the credentials are in a header, never in the log
    return rc;
}
// The pointer file, an absolute https:// address: GET with no Authorization header (the data server's credentials never go to the pointer's host, another one), the list's
// short timeouts, nothing from a cache; its body (at most cap - 1 bytes) into out, NUL-terminated. 0, or -1 with why (the reason, never the body)
static int http_get_plain(jhttp *h, const char *url, char *out, size_t cap, size_t *got, char *why, size_t wcap) {
    JNIEnv *E = h->E; int rc = -1; size_t n = 0; jobject U = NULL, C = NULL, in = NULL; why[0] = 0; *got = 0;
    int64_t left = budget_left(); if (left <= 0) { snprintf(why, wcap, "%s", S.netdown ? "the device has no network" : "out of the 3 s the server gets"); return -1; }
    int to = (int)(left < 3000 ? left : 3000);
    if ((*E)->PushLocalFrame(E, 32)) { jx(E, NULL, 0); snprintf(why, wcap, "no JNI frame"); return -1; }
    jstring su = (*E)->NewStringUTF(E, url); if (jx(E, why, wcap) || !su) goto out;
    U = (*E)->NewObject(E, h->cUrl, h->mUrl, su); if (jx(E, why, wcap) || !U) goto out;
    C = (*E)->CallObjectMethod(E, U, h->mOpen); if (jx(E, why, wcap) || !C || !(*E)->IsInstanceOf(E, C, h->cConn)) goto out;
    (*E)->CallVoidMethod(E, C, h->mConnTo, to); if (jx(E, why, wcap)) goto out;
    (*E)->CallVoidMethod(E, C, h->mReadTo, to); if (jx(E, why, wcap)) goto out;
    (*E)->CallVoidMethod(E, C, h->mCaches, JNI_FALSE); if (jx(E, why, wcap)) goto out;
    if (jprop(h, C, "Cache-Control", "no-cache", why, wcap) || jprop(h, C, "Accept-Encoding", "identity", why, wcap)) goto out;
    int code = (*E)->CallIntMethod(E, C, h->mCode); if (jx(E, why, wcap)) goto out;
    if (code != 200) { snprintf(why, wcap, "HTTP %d", code); goto out; }
    in = (*E)->CallObjectMethod(E, C, h->mIn); if (jx(E, why, wcap) || !in) goto out;
    for (int idle = 0;;) {
        jint k = (*E)->CallIntMethod(E, in, h->mRead, h->buf); if (jx(E, why, wcap)) goto out;
        if (k < 0) break;
        if (!k) { if (++idle > 1000) { snprintf(why, wcap, "the answer did not end"); goto out; } continue; }
        if ((size_t)k > cap - 1 - n) { snprintf(why, wcap, "the answer is longer than %d bytes", (int)(cap - 1)); goto out; }
        (*E)->GetByteArrayRegion(E, h->buf, 0, k, (jbyte *)(out + n)); n += (size_t)k;
    }
    out[n] = 0; *got = n; rc = 0;
out:
    jx(E, why[0] ? NULL : why, why[0] ? 0 : wcap);
    if (in) { (*E)->CallVoidMethod(E, in, h->mClose); jx(E, NULL, 0); }
    if (C && h->mDisc) { (*E)->CallVoidMethod(E, C, h->mDisc); jx(E, NULL, 0); }
    (*E)->PopLocalFrame(E, NULL);
    if (rc && !why[0]) snprintf(why, wcap, "failed");
    if (rc) memset(out, 0, cap);
    return rc;
}
static void basic_auth(char *out, size_t cap, const char *cred) {   // "Basic <base64 of user:password>", "" for no credentials (or ones too long for out)
    out[0] = 0; size_t n = strlen(cred); if (!n) return;
    char b64[sizeof S.auth]; if (4 * ((n + 2) / 3) + 1 + 6 > sizeof b64 || 4 * ((n + 2) / 3) + 1 + 6 > cap) { LOG("[RED] data: the pointer's credentials are too long: none are sent"); return; }
    ds_base64(b64, cred, n); snprintf(out, cap, "Basic %s", b64); memset(b64, 0, sizeof b64);
}
// The server for this try (an asset of version 2), on the sync thread before files.json: the pointer, else the pointer line kept from the last time, else the asset's own.
// A pointer that cannot be read costs one short timeout (3 s, within the 3 s the first try has in all when the folder has a pack; none without a network), and the sync then goes on as before (no server: the game starts with what it has)
static void pointer_resolve(jhttp *h) {
    char line[DS_PTR_MAX + 1], why[256] = "", root[1024], cred[512], base[1100]; const char *bad = NULL; size_t n = 0; int src = 0;   // src: 1 the pointer, 2 the cache, 0 the asset
    if (!http_get_plain(h, PT.pointer, line, sizeof line, &n, why, sizeof why)) {
        if (!ds_pointer_open(line, n, PT.key, root, sizeof root, cred, sizeof cred, &bad)) src = 1;
        else { LOG("[RED] data: the pointer file %s is refused: %s", PT.pointer, bad); snprintf(why, sizeof why, "it was refused: %s", bad); }
    }
    if (src == 1 && PT.cache[0]) {   // kept for a launch the pointer does not answer (written only when it changed)
        size_t cn = 0; char *had = ds_read(PT.cache, &cn, DS_PTR_MAX + 1);
        if (!had || cn != n || memcmp(had, line, n)) { if (ds_write(PT.cache, line, n)) LOG("[RED] data: cannot keep the pointer in the app's storage (errno %d)", errno); }
        if (had) { memset(had, 0, cn); free(had); }
    }
    if (!src && PT.cache[0]) {
        size_t cn = 0; char *had = ds_read(PT.cache, &cn, DS_PTR_MAX + 1);
        if (had && !ds_pointer_open(had, cn, PT.key, root, sizeof root, cred, sizeof cred, &bad)) src = 2;
        else if (had) LOG("[RED] data: the pointer kept in the app's storage is refused: %s", bad);
        if (had) { memset(had, 0, cn); free(had); }
    }
    if (src && (size_t)snprintf(base, sizeof base, "%s%s", root, PT.sub) >= sizeof S.base) { LOG("[RED] data: the pointer's server address is too long"); src = 0; }
    pthread_mutex_lock(&S.mu);
    if (src) { snprintf(S.base, sizeof S.base, "%s", base); basic_auth(S.auth, sizeof S.auth, cred); }
    else { snprintf(S.base, sizeof S.base, "%s", PT.base); snprintf(S.auth, sizeof S.auth, "%s", PT.auth); }
    int auth = S.auth[0] != 0;
    pthread_mutex_unlock(&S.mu);
    if (src == 1) LOG("data: server from the pointer");
    else if (src == 2) LOG("data: server from the cached pointer (the pointer could not be read: %s)", why);
    else LOG("data: server from the APK (no pointer answer and no cache: %s)", why[0] ? why : "the cache could not be read");
    LOG("data: the app's data comes from %s%s", S.base, auth ? " (Basic authentication)" : "");   // the address only: the credentials are in a header, never in the log
    PT.fresh = src == 1;
    memset(line, 0, sizeof line); memset(cred, 0, sizeof cred);
}
static uint64_t free_bytes(void *u, const char *root) { (void)u; struct statvfs v; return statvfs(root, &v) ? (uint64_t)-1 : (uint64_t)v.f_bavail * v.f_frsize; }
static void sync_note(void *u, const ds_progress *p) { (void)u; pthread_mutex_lock(&S.mu); S.pr = *p; pthread_mutex_unlock(&S.mu); }
static void sync_log(void *u, const char *m) { (void)u; LOG("%s", m); }

// ---- the screen: one for the whole start (V), from onCreate (dm_start_screen_open) through the data, the shader preparation and the cloud's start, to the game's first
// frame (dm_start_screen_close from front_android.c game_run): startscreen.c draws its picture into a canvas the window's size, shown as one texture through GLES 2.
// It shows once the start took 300 ms, at once for a question or an error: a start with nothing to do shows nothing. Closing it ends its thread, lets the window go
// (EGL's surface and context destroyed) and frees the fonts, the glyphs, the icon, the background and the canvas: nothing of it is left while the game runs ----
enum { PH_NONE, PH_DATA, PH_PREP, PH_CLOUD, PH_NOTICE };
static struct {
    int open, closing, closed, phase, shown, first_run, need_net, retry_now, has_server;
    int unreached;   // the data's step started the game with the device's pack because its server was not reached (dm_data_unreached)
    int64_t t0;
    char name[160], version[128], checked[48];
    stv_step step[STV_NSTEPS];
} V;   // under S.mu
// the launcher (its code at the end of this file), under S.mu
typedef struct {
    int on, quick, page, sel[4];              // shown (not start at once, or − pressed); the setting; the page (0 the launcher, 1 Cloud save, 2 App settings, 3 the device link), each page's choice
    int64_t unarm;                            // Unlink this app pressed once: the next press until then unlinks
    int want, nosync, gate, gpipe[2];         // Start pressed (without the sync); the start is ready and waits for it (front_android.c's go, called through gpipe)
    void (*go)(void);
    int holding, hold_touch; int64_t hold_t0, hint_until;   // Back to the cloud save held (by the finger); "hold it" shown until
    int vrun, vbad; ds_progress vpr; char amsg[160];        // Check the data again runs, failed; its progress; the App page's last outcome
    int data_rc, data_net, data_off, data_known, has_server, verify_ok, last_act; int64_t checked_t, shader_bytes; uint64_t data_bytes;
} lnr_t;
static lnr_t LN = {.gpipe = {-1, -1}, .data_rc = -1};
static int lnr_input(const AInputEvent *e);
static int lnr_busy_locked(const dm_cloud_info *ci); static void go_locked(void); static int verify_ok_locked(const dm_cloud_info *ci);
static uint64_t data_bytes_now(void); static int64_t shader_bytes_now(void); static void lnr_path(char *o, size_t n);
static struct { uint8_t *ttf[2]; size_t n[2]; uint32_t *icon; int iw, ih; uint32_t glow; int tried; } R;   // the screen thread's own
static int korean(void) { const pf_index *g = pf_find(dm_overlay_font(), 0xAC00); return g && (g->cp_adv >> 8) == 0xAC00; }   // the APK's font has Hangul (assets/font/ui.bin)
void dm_start_step(int step, int state, const char *note) {
    if (step < 0 || step >= STV_NSTEPS) return;
    pthread_mutex_lock(&S.mu); V.step[step].state = state; snprintf(V.step[step].note, sizeof V.step[step].note, "%s", note ? note : ""); pthread_cond_broadcast(&S.cv); pthread_mutex_unlock(&S.mu);
}
static const char *L(const char *ko, const char *en) { return korean() ? ko : en; }
static uint8_t *asset_all(const char *name, size_t *n) {
    AAssetManager *am = S.act ? S.act->assetManager : NULL; AAsset *a = am ? AAssetManager_open(am, name, AASSET_MODE_STREAMING) : NULL;
    if (!a) return NULL;
    off_t len = AAsset_getLength(a); uint8_t *b = len > 0 ? malloc((size_t)len) : NULL;
    if (b && AAsset_read(a, b, (size_t)len) != len) { free(b); b = NULL; }
    AAsset_close(a); if (b) *n = (size_t)len;
    return b;
}
static void res_load(void) {   // the first time the screen shows: Noto Sans KR (assets/font/start-*.ttf, porter's) and the game's icon (assets/start/icon.bin: "NXIC", u16 w, h, RGBA)
    R.tried = 1;
    static const char *const NAME[2] = {"font/start-regular.ttf", "font/start-bold.ttf"};
    for (int i = 0; i < 2; i++) { R.ttf[i] = asset_all(NAME[i], &R.n[i]); if (R.ttf[i] && stv_font_set(i, R.ttf[i], R.n[i])) { free(R.ttf[i]); R.ttf[i] = NULL; } }
    if (!R.ttf[0]) LOG("[RED] start screen: the APK has no assets/font/start-regular.ttf (an APK ported before it): its text is drawn with the overlay's pixel font");
    stv_font_fallback(dm_overlay_font());
    size_t n = 0; uint8_t *ic = asset_all("start/icon.bin", &n);
    if (ic && n >= 8 && !memcmp(ic, "NXIC", 4)) { int w = ic[4] | ic[5] << 8, h = ic[6] | ic[7] << 8;
        if (w > 0 && h > 0 && (size_t)w * h * 4 + 8 <= n && (R.icon = malloc((size_t)w * h * 4))) { memcpy(R.icon, ic + 8, (size_t)w * h * 4); R.iw = w; R.ih = h; } }
    free(ic);
    R.glow = stv_icon_colour(R.icon, R.iw, R.ih);
}
static void res_free(void) { stv_release(); for (int i = 0; i < 2; i++) { free(R.ttf[i]); } free(R.icon); memset(&R, 0, sizeof R); }
static const char *ds_error_text(int status) {
    return status == DS_ERR_NET ? L("서버에 연결할 수 없어요 · 네트워크를 확인해 주세요", "The server cannot be reached · check the network")
         : status == DS_ERR_LOCAL ? L("기기에 있는 게임 데이터가 모자라요 · 다시 설치해 주세요", "The game data on the device is incomplete · install it again")
         : status == DS_ERR_SPACE ? L("저장 공간이 부족해요 · 공간을 비우면 이어서 받아요", "Not enough storage space · the download resumes once there is room")
         : status == DS_ERR_PACKAGE ? L("서버의 데이터가 이 앱의 것이 아니에요", "The server's data is not this app's")
         : status == DS_ERR_DATA ? (!S.base[0] ? L("기기에 있는 묶음 파일이 손상됐어요 · 다시 설치해 주세요", "A bundle on the device is damaged · install the pack again")
                                               : L("받은 파일이 손상돼서 다시 받아요", "A downloaded file was damaged; it is downloaded again"))
         : status == DS_ERR_DISK ? L("파일을 쓸 수 없어요", "A file cannot be written") : L("서버 오류", "Server error");
}
// the first start without a network: a question of its own (Retry; it goes on by itself once the network is back)
static void need_net_view(dm_cloud_view *q, int ws, int wtot) {
    memset(q, 0, sizeof *q); q->ask = 1; q->urgent = 1; q->icon = DM_CLOUD_ICON_OFFLINE; q->progress = -1; q->nbtn = 1;
    snprintf(q->title, sizeof q->title, "%s", L("인터넷 연결이 필요해요", "An internet connection is needed"));
    snprintf(q->line[0], sizeof q->line[0], "%s", V.first_run ? L("처음 한 번 게임 데이터를 받아야 해요. 연결되면 바로 이어서 받아요.", "The game data has to be downloaded once. It starts as soon as there is a connection.")
                                                              : L("남은 게임 데이터를 받아야 해요. 연결되면 바로 이어서 받아요.", "The rest of the game data has to be downloaded. It goes on as soon as there is a connection."));
    snprintf(q->btn[0], sizeof q->btn[0], "%s", L("다시 시도", "Retry"));
    snprintf(q->count, sizeof q->count, korean() ? "%d초 뒤 다시 확인해요" : "Trying again in %d s", ws);
    q->count_permille = wtot > 0 ? (wtot - ws) * 1000 / wtot : -1;
}
typedef struct { int phase, status, wait_s, wait_tot, need_net, pd, pt; ds_progress p; char msg[256], nt_ko[3][200], nt_en[3][200], nt_detail[300]; lnr_t ln; } snap_t;
// the picture's state, from what the phases left (under no lock: snap and cv are copies)
// the launcher's picture: its statuses, items and pages from the start's state (x), the launcher's (x->ln) and the cloud's (ci)
static void fmt_gb(char *o, size_t n, uint64_t b) { if (b >= 1024ull << 20) snprintf(o, n, "%.1f GB", b / 1073741824.0); else snprintf(o, n, "%.0f MB", b / 1048576.0); }
static void st_set(stv_status *s, int state, const char *t) { s->state = state; snprintf(s->text, sizeof s->text, "%s", t); }
// the Cloud save page's link button (stv_cloudpage.link): 1 Link this device (no sync.txt and the link can run), 2 Unlink this app (linked, not a trial build), 0 none
static int cp_link_of(const dm_cloud_info *ci) { return !ci->avail ? (ci->linkable ? 1 : 0) : !ci->trial ? 2 : 0; }
// the link page from the link's state (savecloud.h dm_cloud_link): its layout's part (state, buttons) too, which lnr_hit needs without the words
static void lk_fill(stv_linkpage *p, const dm_cloud_link *k, const char *game, int sel) {
    int ko = korean(); memset(p, 0, sizeof *p);
    p->state = k->state == DM_LINK_CODE ? STV_LINK_CODE : k->state == DM_LINK_EXPIRED ? STV_LINK_EXPIRED : k->state == DM_LINK_FAIL ? STV_LINK_FAIL : k->state == DM_LINK_DONE ? STV_LINK_DONE : STV_LINK_BUSY;
    int two = p->state == STV_LINK_EXPIRED || p->state == STV_LINK_FAIL; p->nbtn = two ? 2 : 1; p->sel = two ? (sel == 1 ? 1 : 0) : p->state == STV_LINK_DONE ? 0 : -1;
    if (p->state == STV_LINK_DONE) {
        snprintf(p->title, sizeof p->title, "%s", L("연결됐어요", "Linked")); snprintf(p->line, sizeof p->line, "%s", L("이제 저장하면 클라우드에 올라가요.", "From now on each save goes up to the cloud."));
        p->nrow = 2; snprintf(p->key[0], sizeof p->key[0], "%s", L("기기 이름", "Device name")); snprintf(p->val[0], sizeof p->val[0], "%s", k->device);
        snprintf(p->key[1], sizeof p->key[1], "%s", L("이 게임", "This game")); snprintf(p->val[1], sizeof p->val[1], "%s", game);
        char site[160]; snprintf(site, sizeof site, "%s", k->host[0] ? k->host : "saves.example.com"); char *sl = strchr(site, '/'); if (sl) *sl = 0;
        snprintf(p->foot, sizeof p->foot, ko ? "기기 이름은 웹 %s/saves 에서 바꾸거나 연결을 끊을 수 있어요" : "The device's name can be changed, or the link ended, at %s/saves", site);
        snprintf(p->btn[0], sizeof p->btn[0], "%s", L("계속", "Continue"));   // (no keys line: the one button is chosen, and the page is tall)
        return;
    }
    snprintf(p->title, sizeof p->title, "%s", p->state == STV_LINK_FAIL ? L("연결 코드를 받지 못했어요", "No link code came") : L("휴대폰으로 이 기기를 연결하세요", "Link this device with your phone"));
    if (p->state == STV_LINK_BUSY) snprintf(p->line, sizeof p->line, "%s", L("코드를 받는 중…", "Getting a code…"));
    if (p->state == STV_LINK_FAIL) snprintf(p->line, sizeof p->line, "%s", k->limit ? L("너무 자주 요청했어요. 잠시 뒤에 다시 해 주세요.", "Asked too often. Try again in a while.") : L("인터넷 연결을 확인하고 다시 해 주세요.", "Check the internet connection and try again."));
    if (two) { snprintf(p->btn[0], sizeof p->btn[0], "%s", p->state == STV_LINK_FAIL ? L("다시 시도", "Retry") : L("새 코드", "New code")); snprintf(p->btn[1], sizeof p->btn[1], "%s", L("취소", "Cancel"));
               snprintf(p->keys, sizeof p->keys, "%s", L("← → 고르기 · A 선택 · B 취소", "← → choose · A select · B cancel")); }
    else { snprintf(p->btn[0], sizeof p->btn[0], "%s", L("취소", "Cancel")); snprintf(p->keys, sizeof p->keys, "%s", L("B 취소", "B cancel")); }
    if (p->state == STV_LINK_CODE || p->state == STV_LINK_EXPIRED) {
        snprintf(p->qr, sizeof p->qr, "%s", k->qr); snprintf(p->host, sizeof p->host, "%s", k->host); snprintf(p->code, sizeof p->code, "%s", k->code);
        snprintf(p->step1, sizeof p->step1, "%s", L("1. 휴대폰 카메라로 QR을 찍거나, 아래 주소로 들어가요", "1. Scan the QR code with the phone's camera, or open this address"));
        snprintf(p->step2, sizeof p->step2, "%s", L("2. 카카오로 로그인하고 이 코드를 확인해요", "2. Sign in with Kakao and confirm this code"));
        if (p->state == STV_LINK_EXPIRED) snprintf(p->status, sizeof p->status, "%s", L("코드 시간이 지났어요 · A 새 코드", "The code ran out · A new code"));
        else snprintf(p->status, sizeof p->status, ko ? "휴대폰에서 연결을 누르길 기다리는 중 · %d:%02d 남음" : "Waiting for Link on the phone · %d:%02d left", k->left_s / 60, k->left_s % 60);
    }
}
static void build_launcher(stv_view *v, const snap_t *x, const stv_step *steps, const dm_cloud_info *ci) {
    const lnr_t *n = &x->ln; int ko = korean(); int64_t now = (int64_t)time(NULL), mono = now_ms(); char t[200], a[64], b[64];
    v->screen = n->page == 1 ? STV_SCREEN_CLOUD : n->page == 2 ? STV_SCREEN_APP : n->page == 3 ? STV_SCREEN_LINK : STV_SCREEN_LAUNCHER;
    if (v->screen == STV_SCREEN_LINK) { dm_cloud_link k; dm_cloud_link_get(&k); lk_fill(&v->lk, &k, V.name, n->sel[3]); v->anim = k.state == DM_LINK_BUSY || k.state == DM_LINK_CODE; return; }
    // the three statuses
    stv_launcher *l = &v->ln; stv_status *s = l->status;
    if (n->vrun) { int p = n->vpr.total ? (int)(n->vpr.done * 100 / n->vpr.total) : 0;
        snprintf(t, sizeof t, ko ? "게임 데이터 %s · %d%%" : "Game data: %s · %d%%", n->vpr.phase == DS_PH_DOWNLOAD || n->vpr.phase == DS_PH_UNPACK ? L("받는 중", "downloading") : L("확인 중", "checking"), p); st_set(&s[0], STV_RUN, t); }
    else if (x->phase == PH_DATA || steps[STV_DATA].state == STV_RUN) st_set(&s[0], STV_RUN, L("게임 데이터 확인 중", "Checking the game data"));
    else if (steps[STV_DATA].state == STV_WAIT) st_set(&s[0], STV_WAIT, L("게임 데이터 대기", "Game data waiting"));
    else if (n->data_off) st_set(&s[0], STV_SKIP, L("게임 데이터 · 오프라인", "Game data · offline"));
    else st_set(&s[0], STV_DONE, n->data_rc == DS_DONE ? L("게임 데이터 받음", "Game data downloaded") : n->data_rc == DS_UPTODATE ? L("게임 데이터 최신", "Game data up to date") : L("게임 데이터 준비됨", "Game data ready"));
    if (x->phase == PH_PREP) { if (x->pt) { snprintf(t, sizeof t, ko ? "셰이더 준비 중 · %d%%" : "Preparing shaders · %d%%", (int)((int64_t)x->pd * 100 / x->pt)); st_set(&s[1], STV_RUN, t); } else st_set(&s[1], STV_RUN, L("셰이더 확인 중", "Checking the shaders")); }
    else st_set(&s[1], steps[STV_SHADERS].state, steps[STV_SHADERS].state == STV_DONE ? L("셰이더 준비 완료", "Shaders ready") : steps[STV_SHADERS].state == STV_SKIP ? L("셰이더 필요 없음", "No shaders to prepare") : L("셰이더 준비 대기", "Shaders waiting"));
    if (ci->act == DM_CLOUD_ACT_UPLOAD) st_set(&s[2], STV_RUN, L("클라우드 세이브 올리는 중", "Uploading the cloud save"));
    else if (ci->act || ci->checking || steps[STV_CLOUD].state == STV_RUN) {
        if (ci->progress >= 0) { snprintf(t, sizeof t, ko ? "클라우드 세이브 받는 중 · %d%%" : "Downloading the cloud save · %d%%", ci->progress / 10); st_set(&s[2], STV_RUN, t); }
        else st_set(&s[2], STV_RUN, L("클라우드 세이브 확인 중", "Checking the cloud save")); }
    else if (ci->skipped) st_set(&s[2], STV_SKIP, L("클라우드 세이브 · 건너뜀", "Cloud save · skipped"));
    else if (!ci->avail) st_set(&s[2], steps[STV_CLOUD].state == STV_SKIP ? STV_SKIP : STV_WAIT, steps[STV_CLOUD].state == STV_SKIP ? L("클라우드 세이브 · 기기 연결 필요", "Cloud save · not linked") : L("클라우드 세이브 대기", "Cloud save waiting"));
    else if (ci->trial) st_set(&s[2], STV_SKIP, L("클라우드 세이브 · 시험판", "Cloud save · trial build"));
    else if (ci->mode == 2) st_set(&s[2], STV_SKIP, L("클라우드 세이브 · 이 기기만", "Cloud save · this device only"));
    else if (ci->auth) st_set(&s[2], STV_FAIL, L("클라우드 세이브 · 연결 끊김", "Cloud save · unlinked"));
    else if (ci->offline) st_set(&s[2], STV_SKIP, L("클라우드 세이브 · 오프라인", "Cloud save · offline"));
    else if (ci->local == DM_CLOUD_LOCAL_CONFLICT) st_set(&s[2], STV_FAIL, L("클라우드 세이브 · 고를 게 있어요", "Cloud save · a choice waits"));
    else if (ci->local == DM_CLOUD_LOCAL_OWED) st_set(&s[2], STV_FAIL, L("클라우드 세이브 · 안 올린 진행", "Cloud save · not uploaded"));
    else if (ci->local == DM_CLOUD_LOCAL_PLAYED) st_set(&s[2], STV_SKIP, L("클라우드 세이브 · 동기화 없이 한 플레이", "Cloud save · played without the sync"));
    else st_set(&s[2], STV_DONE, L("클라우드 세이브 최신", "Cloud save up to date"));
    static const char *const MKO[3] = {"자동", "수동", "이 기기만"}, *const MEN[3] = {"Automatic", "Manual", "This device only"};
    const char *mname = ci->mode >= 0 && ci->mode <= 2 ? (ko ? MKO[ci->mode] : MEN[ci->mode]) : "";
    if (v->screen == STV_SCREEN_LAUNCHER) {
        snprintf(l->item[0], sizeof l->item[0], "%s", L("동기화 없이 플레이", "Play without the sync"));
        snprintf(l->note[0], sizeof l->note[0], "%s", ci->avail && ci->mode != 2 ? L("테스트 · 이번 판은 올리지 않아요", "A test · this run is not uploaded") : L("이번 판은 올리지 않아요", "This run is not uploaded"));
        snprintf(l->item[1], sizeof l->item[1], "%s", L("클라우드 세이브", "Cloud save"));
        if (!ci->avail) snprintf(l->note[1], sizeof l->note[1], "%s", steps[STV_CLOUD].state == STV_SKIP ? L("기기 연결 필요", "Not linked") : L("확인 전", "Not checked yet"));
        else if (ci->trial) snprintf(l->note[1], sizeof l->note[1], "%s", L("시험판 · 이 기기만", "Trial build · this device only"));
        else if (ci->mode == 2) snprintf(l->note[1], sizeof l->note[1], "%s", mname);
        else if (ci->cloud_time > 0) { ss_fmt_ago(a, sizeof a, ci->cloud_time, now, ko); snprintf(l->note[1], sizeof l->note[1], ko ? "%s · %s에서 %s" : "%s · from %s, %s", mname, ci->cloud_dev[0] ? ci->cloud_dev : "-", a); }
        else snprintf(l->note[1], sizeof l->note[1], "%s · %s", mname, ci->online ? L("클라우드에 세이브 없음", "none in the cloud") : ci->offline ? L("오프라인", "offline") : L("확인 전", "not checked yet"));
        snprintf(l->item[2], sizeof l->item[2], "%s", L("앱 설정", "App settings"));
        if (n->data_known && n->data_bytes) { fmt_gb(a, sizeof a, n->data_bytes); snprintf(l->note[2], sizeof l->note[2], ko ? "데이터 %s · 바로 시작 %s" : "Data %s · start at once %s", a, n->quick ? L("켜짐", "on") : L("꺼짐", "off")); }
        else snprintf(l->note[2], sizeof l->note[2], ko ? "바로 시작 %s" : "Start at once %s", n->quick ? L("켜짐", "on") : L("꺼짐", "off"));
        snprintf(l->start, sizeof l->start, "%s", L("게임 시작", "Start the game")); l->sel = n->sel[0]; l->waiting = n->want;
        snprintf(l->start_sub, sizeof l->start_sub, "%s", n->nosync ? L("준비가 끝나면 동기화 없이 시작해요 · B 취소", "Starts without the sync once ready · B cancel") : L("준비가 끝나면 바로 시작해요 · B 취소", "Starts once ready · B cancel"));
        snprintf(l->keys, sizeof l->keys, "%s", n->want ? L("B 취소", "B cancel") : L("↑↓ 고르기 · A 선택", "↑↓ choose · A select"));
        v->anim = n->want; for (int i = 0; i < STV_NSTEPS; i++) if (s[i].state == STV_RUN) v->anim = 1;
    } else if (v->screen == STV_SCREEN_CLOUD) {
        stv_cloudpage *p = &v->cp;
        snprintf(p->title, sizeof p->title, "%s", L("클라우드 세이브", "Cloud save")); snprintf(p->label[0], sizeof p->label[0], "%s", L("동기화 방식", "How it syncs")); snprintf(p->label[1], sizeof p->label[1], "%s", L("지금 상태", "Now"));
        static const char *const DKO[3] = {"저장하면 바로 올리고, 게임을 끌 때도 올려요", "올리기는 내가 정해요. 시작할 때 올릴지 물어봐요", "받지도 올리지도 않아요. 이 기기 세이브로만 테스트해요"},
                          *const DEN[3] = {"Each save goes up, and when the game closes", "You decide when it goes up; the start asks", "Nothing down or up: this device's save only, for tests"};
        for (int i = 0; i < 3; i++) { snprintf(p->mode[i], sizeof p->mode[i], "%s", ko ? MKO[i] : MEN[i]); snprintf(p->desc[i], sizeof p->desc[i], "%s", ko ? DKO[i] : DEN[i]); }
        p->checked = ci->mode; p->modes_off = !ci->avail || ci->trial || ci->act || ci->checking;
        p->link = cp_link_of(ci);
        snprintf(p->note, sizeof p->note, "%s", p->link == 1 ? L("휴대폰으로 한 번 연결하면 이 기기의 다른 게임도 함께 연결돼요.", "Linked once with a phone, this device's other games are linked too.")
                 : !ci->avail ? (steps[STV_CLOUD].state == STV_SKIP ? L("이 기기는 클라우드 세이브에 연결되지 않았어요.", "This device is not linked to the cloud save.")
                                                                     : L("시작 준비가 끝나면 바꿀 수 있어요.", "It can be changed once the start is ready."))
                 : ci->trial ? L("시험판이라 이 기기 세이브로만 해요. 바꿀 수 없어요.", "A trial build keeps this device's save only. It cannot be changed.")
                 : ci->act || ci->checking ? L("확인하는 중이에요. 끝나면 바꿀 수 있어요.", "Checking. It can be changed once done.")
                 : L("이 기기만에서 다시 자동·수동으로 바꾸면, 처음 연결할 때처럼 어느 세이브로 할지 물어봐요.", "Back from this device only to automatic or manual, it asks which save to keep, as at the first link."));
        p->nrow = 4; snprintf(p->key[0], sizeof p->key[0], "%s", L("클라우드", "Cloud")); snprintf(p->key[1], sizeof p->key[1], "%s", L("이 기기", "This device"));
        snprintf(p->key[2], sizeof p->key[2], "%s", L("크기", "Size")); snprintf(p->key[3], sizeof p->key[3], "%s", L("계정", "Account"));
        if (ci->avail && !ci->online && (ci->offline || ci->auth) && ci->mode != 2) { ss_fmt_date(a, sizeof a, ci->checked, ko); snprintf(p->val[0], sizeof p->val[0], ko ? "연결 안 됨 · 마지막 확인 %s" : "Not connected · last checked %s", ci->checked ? a : "-"); p->col[0] = STV_COL_DIM; }
        else if (ci->cloud_time > 0) { ss_fmt_when(a, sizeof a, ci->cloud_time, now, ko); snprintf(p->val[0], sizeof p->val[0], "%s · %s", ci->cloud_dev[0] ? ci->cloud_dev : "-", a); }
        else snprintf(p->val[0], sizeof p->val[0], "%s", ci->online ? L("아직 없음", "None yet") : "-");
        static const char *const LKO[6] = {"확인 전", "클라우드와 같아요", "안 올린 진행 있음", "이 기기만", "동기화 없이 한 플레이 있음", "고를 게 있어요 (다음 시작)"},
                          *const LEN[6] = {"Not checked", "Same as the cloud", "Progress not uploaded", "This device only", "Played without the sync", "A choice waits (next start)"};
        static const int LCOL[6] = {STV_COL_DIM, STV_COL_GOOD, STV_COL_WARN, STV_COL_DIM, STV_COL_WARN, STV_COL_WARN};
        int lk = ci->local >= 0 && ci->local < 6 ? ci->local : 0; snprintf(p->val[1], sizeof p->val[1], "%s", ko ? LKO[lk] : LEN[lk]); p->col[1] = LCOL[lk];
        if (ci->files >= 0) { char z[32]; ss_fmt_size(z, sizeof z, ci->size); if (ko) snprintf(p->val[2], sizeof p->val[2], "%s · 파일 %lld개", z, (long long)ci->files); else snprintf(p->val[2], sizeof p->val[2], "%s · %lld files", z, (long long)ci->files); }
        else snprintf(p->val[2], sizeof p->val[2], "-");
        if (ci->avail) snprintf(p->val[3], sizeof p->val[3], ko ? "기기 이름 %s" : "Device name %s", ci->device); else snprintf(p->val[3], sizeof p->val[3], "%s", L("연결 안 됨", "Not linked"));
        snprintf(p->up, sizeof p->up, "%s", L("지금 올리기", "Upload now")); p->up_off = !ci->can_upload;
        if (ci->act == DM_CLOUD_ACT_UPLOAD) snprintf(p->up_note, sizeof p->up_note, "%s", L("올리는 중…", "Uploading…"));
        else if (n->last_act == DM_CLOUD_ACT_UPLOAD && ci->result[0]) snprintf(p->up_note, sizeof p->up_note, "%s", ci->result);
        else snprintf(p->up_note, sizeof p->up_note, "%s", ci->can_upload ? L("안 올린 진행 있어요", "Progress not uploaded") : !ci->avail || ci->mode == 2 || ci->trial ? L("쓸 수 없어요", "Not available")
                      : !ci->online ? L("연결 안 됨", "Not connected") : L("올릴 진행 없음", "Nothing to upload"));
        snprintf(p->back, sizeof p->back, "%s", L("클라우드 세이브로 되돌리기", "Back to the cloud save")); p->back_off = !ci->can_restore;
        if (ci->act == DM_CLOUD_ACT_RESTORE) { if (ci->progress >= 0) snprintf(p->back_note, sizeof p->back_note, ko ? "받는 중 · %d%%" : "Downloading · %d%%", ci->progress / 10); else snprintf(p->back_note, sizeof p->back_note, "%s", L("되돌리는 중…", "Restoring…")); }
        else if (n->hint_until > mono) snprintf(p->back_note, sizeof p->back_note, "%s", L("길게 눌러요 (1.5초)", "Hold it (1.5 s)"));
        else if (n->last_act == DM_CLOUD_ACT_RESTORE && ci->result[0]) snprintf(p->back_note, sizeof p->back_note, "%s", ci->result);
        else snprintf(p->back_note, sizeof p->back_note, "%s", ci->can_restore ? L("A 길게 · 이 기기 세이브는 백업 1칸에", "Hold A · this device's save to the backup slot") : !ci->avail || ci->mode == 2 || ci->trial ? L("쓸 수 없어요", "Not available")
                      : !ci->online ? L("연결 안 됨", "Not connected") : L("클라우드에 세이브 없음", "None in the cloud"));
        p->hold = n->holding ? (int)((mono - n->hold_t0) * 1000 / 1500) : 0;
        if (p->link == 1) { snprintf(p->lbtn, sizeof p->lbtn, "%s", L("기기 연결하기", "Link this device")); snprintf(p->lnote, sizeof p->lnote, "%s", L("휴대폰으로 QR · 코드", "QR · code on a phone")); }
        else if (p->link == 2) { p->larm = n->unarm > mono && ci->can_unlink; snprintf(p->lbtn, sizeof p->lbtn, "%s", L("이 앱 연결 끊기", "Unlink this app"));
            snprintf(p->lnote, sizeof p->lnote, "%s", p->larm ? L("A 한 번 더 누르면 끊어요", "A again to unlink") : !ci->can_unlink ? L("확인이 끝나면 할 수 있어요", "Once the check is done")
                     : L("다른 게임과 기기 전체는 웹에서 끊을 수 있어요", "Other games and the whole device: on the web")); }
        snprintf(p->keys, sizeof p->keys, "%s", L("↑↓←→ 고르기 · A 선택 · B 돌아가기", "↑↓←→ choose · A select · B back")); p->sel = n->sel[1];
        v->anim = n->holding || ci->act || ci->checking || n->hint_until > mono || n->unarm > mono;
    } else {
        stv_apppage *p = &v->ap;
        snprintf(p->title, sizeof p->title, "%s", L("앱 설정", "App settings")); snprintf(p->label[0], sizeof p->label[0], "%s", L("시작", "Start")); snprintf(p->label[1], sizeof p->label[1], "%s", L("게임 데이터", "Game data"));
        snprintf(p->quick, sizeof p->quick, "%s", L("바로 시작", "Start at once")); p->quick_on = n->quick;
        snprintf(p->quick_desc[0], sizeof p->quick_desc[0], "%s", L("켜면 이 런처 없이 바로 게임을 시작해요.", "On: the game starts without this launcher."));
        snprintf(p->quick_desc[1], sizeof p->quick_desc[1], "%s", L("켤 때 −를 누르고 있으면 런처가 다시 열려요.", "Hold − while it starts to open the launcher again."));
        p->nrow = 3; snprintf(p->key[0], sizeof p->key[0], "%s", L("게임 데이터", "Game data")); snprintf(p->key[1], sizeof p->key[1], "%s", L("셰이더 캐시", "Shader cache")); snprintf(p->key[2], sizeof p->key[2], "%s", L("마지막 확인", "Last checked"));
        const char *st = n->vrun ? L("확인 중", "checking") : n->data_off ? L("오프라인", "offline") : n->data_rc == DS_DONE || n->data_rc == DS_UPTODATE ? L("최신", "up to date") : L("준비됨", "ready");
        if (n->data_known && n->data_bytes) { fmt_gb(a, sizeof a, n->data_bytes); snprintf(p->val[0], sizeof p->val[0], "%s · %s", a, st); } else snprintf(p->val[0], sizeof p->val[0], "%s", st);
        if (n->shader_bytes > 0) fmt_gb(p->val[1], sizeof p->val[1], (uint64_t)n->shader_bytes); else snprintf(p->val[1], sizeof p->val[1], "%s", L("없음", "None"));
        if (n->checked_t > 0) ss_fmt_when(p->val[2], sizeof p->val[2], n->checked_t, now, ko); else snprintf(p->val[2], sizeof p->val[2], "-");
        dm_cloud_info ci2 = *ci; (void)ci2;
        if (n->vrun) { int pc = n->vpr.total ? (int)(n->vpr.done * 100 / n->vpr.total) : 0;
            snprintf(p->verify, sizeof p->verify, ko ? "%s · %d%%" : "%s · %d%%", n->vpr.phase == DS_PH_DOWNLOAD || n->vpr.phase == DS_PH_UNPACK ? L("다시 받는 중", "Downloading") : L("확인 중", "Checking"), pc); }
        else snprintf(p->verify, sizeof p->verify, "%s", L("데이터 다시 확인", "Check the data again"));
        p->verify_off = !n->verify_ok; snprintf(p->clear, sizeof p->clear, "%s", L("셰이더 캐시 비우기", "Clear the shader cache")); p->clear_off = n->shader_bytes <= 0 || x->phase == PH_PREP;
        if (n->amsg[0]) snprintf(p->note, sizeof p->note, "%s", n->amsg);
        else if (!n->verify_ok && !n->vrun && (n->data_off || !n->has_server)) snprintf(p->note, sizeof p->note, "%s", n->has_server ? L("오프라인이라 데이터 다시 확인을 할 수 없어요.", "Offline: the data cannot be checked again.")
                                                                                                               : L("이 앱은 데이터 서버가 없어 다시 확인할 수 없어요.", "This app has no data server to check against."));
        else snprintf(p->note, sizeof p->note, "%s", L("데이터 다시 확인은 파일을 하나씩 검사해 망가진 것만 다시 받아요.", "Checking the data again reads every file and downloads only the damaged ones."));
        snprintf(p->keys, sizeof p->keys, "%s", L("↑↓←→ 고르기 · A 선택 · B 돌아가기", "↑↓←→ choose · A select · B back")); p->sel = n->sel[2];
        v->anim = n->vrun;
    }
    (void)b;
}
static void build_view(stv_view *v, const snap_t *x, const stv_rate *rate, dm_cloud_view *cv, dm_cloud_view *dq, const stv_step *steps, const dm_cloud_info *ci) {
    memset(v, 0, sizeof *v); v->bar = -1; int ko = korean(); char a[48], b[48], t[200];
    snprintf(v->name, sizeof v->name, "%s", V.name); snprintf(v->version, sizeof v->version, "%s%s", V.version, V.first_run ? L(" · 처음 실행", " · first start") : "");
    v->icon = R.icon; v->icon_w = R.iw; v->icon_h = R.ih; v->glow = R.glow;
    memcpy(v->step, steps, sizeof v->step);
    snprintf(v->hint, sizeof v->hint, "%s", L("곧 시작해요", "Starting soon"));
    if (x->phase == PH_DATA) {
        const ds_progress *p = &x->p; stv_step *s = &v->step[STV_DATA]; s->state = STV_RUN;
        stv_fmt_int(a, sizeof a, p->file_no); stv_fmt_int(b, sizeof b, p->files);
        if (p->phase == DS_PH_CONNECT) snprintf(s->note, sizeof s->note, "%s", L("서버 확인 중…", "Checking the server…"));
        else if (p->phase == DS_PH_CHECK) snprintf(s->note, sizeof s->note, "%s", L("파일 확인 중…", "Checking the files…"));
        else if (p->phase == DS_PH_FINISH) snprintf(s->note, sizeof s->note, "%s", L("마무리 중…", "Finishing…"));
        else snprintf(s->note, sizeof s->note, ko ? "파일 %s / %s" : "%s / %s files", a, b);
        if ((p->phase == DS_PH_DOWNLOAD || p->phase == DS_PH_UNPACK) && p->total) {
            v->bar = (int)(p->done * 1000 / p->total);
            snprintf(v->bar_title, sizeof v->bar_title, "%s", p->phase == DS_PH_DOWNLOAD ? L("게임 데이터 받는 중", "Downloading the game data") : L("묶음 파일 푸는 중", "Extracting the bundles"));
            stv_fmt_bytes(v->bar_left, sizeof v->bar_left, p->done, p->total);
            char e[64]; stv_fmt_eta(e, sizeof e, rate->eta, ko);
            if (p->phase == DS_PH_DOWNLOAD && rate->speed > 0) { char sp[32]; stv_fmt_speed(sp, sizeof sp, rate->speed); snprintf(v->bar_right, sizeof v->bar_right, e[0] ? "%s · %s" : "%s", sp, e); }
            else snprintf(v->bar_right, sizeof v->bar_right, "%s", e);
            if (p->phase == DS_PH_DOWNLOAD) snprintf(v->hint, sizeof v->hint, "%s", L("와이파이에서 받는 걸 권해요 · 중간에 꺼도 받은 파일은 그대로예요", "Wi-Fi is best · what was downloaded stays if the app is closed"));
            else v->hint[0] = 0;
        }
        if (x->status >= 0) {
            if (x->need_net) { need_net_view(dq, x->wait_s, x->wait_tot); v->ask = dq; }
            else { s->state = STV_FAIL; snprintf(s->note, sizeof s->note, ko ? "%d초 뒤 다시 시도" : "again in %d s", x->wait_s);
                   v->hint_warn = 1; snprintf(v->hint, sizeof v->hint, "%s", ds_error_text(x->status)); snprintf(v->hint2, sizeof v->hint2, "%s", x->msg); }
        }
    } else if (x->phase == PH_PREP) {
        stv_step *s = &v->step[STV_SHADERS]; s->state = STV_RUN;
        if (!x->pt) snprintf(s->note, sizeof s->note, "%s", L("확인 중…", "Checking…"));
        else {
            stv_fmt_int(a, sizeof a, x->pd); stv_fmt_int(b, sizeof b, x->pt); snprintf(t, sizeof t, "%s / %s", a, b); snprintf(s->note, sizeof s->note, "%s", t);
            v->bar = (int)((int64_t)x->pd * 1000 / x->pt); snprintf(v->bar_title, sizeof v->bar_title, "%s", L("셰이더 준비 중", "Preparing the shaders"));
            snprintf(v->bar_left, sizeof v->bar_left, "%s", t); stv_fmt_eta(v->bar_right, sizeof v->bar_right, x->pd > 16 ? rate->eta : -1, ko);
            snprintf(v->hint, sizeof v->hint, "%s", L("처음 한 번만 해요 · 게임 데이터나 드라이버가 바뀌면 다시 해요", "Done once · again after a data or driver update"));
        }
    } else if (x->phase == PH_NOTICE) {   // a question's screen without buttons: the warning's icon, the title, two lines, the log's words small at the bottom
        int k = korean(); memset(dq, 0, sizeof *dq); dq->ask = 1; dq->urgent = 1; dq->icon = DM_CLOUD_ICON_WARN; dq->progress = -1;
        snprintf(dq->title, sizeof dq->title, "%s", k ? x->nt_ko[0] : x->nt_en[0]);
        for (int i = 0; i < 2; i++) snprintf(dq->line[i], sizeof dq->line[i], "%s", k ? x->nt_ko[i + 1] : x->nt_en[i + 1]);
        snprintf(dq->foot, sizeof dq->foot, "%s", x->nt_detail);
        v->ask = dq;
    } else if (x->phase == PH_CLOUD) {
        if (cv->ask) v->ask = cv;
        else {
            stv_step *s = &v->step[STV_CLOUD]; s->state = STV_RUN; snprintf(s->note, sizeof s->note, "%s", cv->note);
            if (cv->progress >= 0) { v->bar = cv->progress; snprintf(v->bar_title, sizeof v->bar_title, "%s", L("클라우드 세이브 받는 중", "Downloading the cloud save")); }
            if (cv->hint[0]) snprintf(v->hint, sizeof v->hint, "%s", cv->hint);
            if (cv->nbtn) snprintf(v->foot, sizeof v->foot, "%s", L("B · 동기화 없이 플레이", "B · play without the sync"));
        }
    }
    if (x->ln.on && cv->ask && !v->ask) v->ask = cv;   // (a question of a check the launcher asked for)
    if (x->ln.on && !v->ask && x->phase != PH_NOTICE && !(x->phase == PH_DATA && (v->bar >= 0 || x->status >= 0))) build_launcher(v, x, steps, ci);   // else the steps: a download, an error
}
static struct {   // libEGL / libGLESv2, opened here (the host links neither)
    EGLDisplay (*GetDisplay)(EGLNativeDisplayType); EGLBoolean (*Initialize)(EGLDisplay, EGLint *, EGLint *); EGLBoolean (*ChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
    EGLContext (*CreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *); EGLSurface (*CreateWindowSurface)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *);
    EGLBoolean (*MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext); EGLBoolean (*SwapBuffers)(EGLDisplay, EGLSurface); EGLBoolean (*DestroySurface)(EGLDisplay, EGLSurface);
    EGLBoolean (*DestroyContext)(EGLDisplay, EGLContext); EGLBoolean (*Terminate)(EGLDisplay);
    GLuint (*CreateShader)(GLenum); void (*ShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *); void (*CompileShader)(GLuint); GLuint (*CreateProgram)(void);
    void (*AttachShader)(GLuint, GLuint); void (*BindAttribLocation)(GLuint, GLuint, const GLchar *); void (*LinkProgram)(GLuint); void (*UseProgram)(GLuint);
    void (*GenTextures)(GLsizei, GLuint *); void (*BindTexture)(GLenum, GLuint); void (*TexParameteri)(GLenum, GLenum, GLint);
    void (*TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *); void (*VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
    void (*EnableVertexAttribArray)(GLuint); void (*DrawArrays)(GLenum, GLint, GLsizei); void (*Viewport)(GLint, GLint, GLsizei, GLsizei);
    void *libe, *libg; EGLDisplay dpy; EGLConfig cfg; EGLContext ctx; EGLSurface surf; ANativeWindow *on; GLuint prog, tex; int ok;
} G;
static int gl_load(void) {
    void *e = G.libe = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL), *g = G.libg = dlopen("libGLESv2.so", RTLD_NOW | RTLD_LOCAL); if (!e || !g) return -1;
#define LE(n) if (!(*(void **)&G.n = dlsym(e, "egl" #n))) return -1
#define LG(n) if (!(*(void **)&G.n = dlsym(g, "gl" #n))) return -1
    LE(GetDisplay); LE(Initialize); LE(ChooseConfig); LE(CreateContext); LE(CreateWindowSurface); LE(MakeCurrent); LE(SwapBuffers); LE(DestroySurface); LE(DestroyContext); LE(Terminate);
    LG(CreateShader); LG(ShaderSource); LG(CompileShader); LG(CreateProgram); LG(AttachShader); LG(BindAttribLocation); LG(LinkProgram); LG(UseProgram); LG(GenTextures);
    LG(BindTexture); LG(TexParameteri); LG(TexImage2D); LG(VertexAttribPointer); LG(EnableVertexAttribArray); LG(DrawArrays); LG(Viewport);
    G.dpy = G.GetDisplay(EGL_DEFAULT_DISPLAY); if (!G.dpy || !G.Initialize(G.dpy, NULL, NULL)) return -1;
    const EGLint ca[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE}; EGLint n = 0;
    if (!G.ChooseConfig(G.dpy, ca, &G.cfg, 1, &n) || n < 1) return -1;
    const EGLint xa[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    G.ctx = G.CreateContext(G.dpy, G.cfg, EGL_NO_CONTEXT, xa); return G.ctx ? 0 : -1;
}
static void gl_drop(void) { if (G.surf) { G.MakeCurrent(G.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); G.DestroySurface(G.dpy, G.surf); G.surf = NULL; G.on = NULL; } }   // the window is free for Vulkan after this
static void gl_end(void) {   // everything EGL: the context, the display's use, the libraries
    gl_drop();
    if (G.ctx && G.DestroyContext) G.DestroyContext(G.dpy, G.ctx);
    if (G.dpy && G.Terminate) G.Terminate(G.dpy);
    if (G.libg) dlclose(G.libg);
    if (G.libe) dlclose(G.libe);
    memset(&G, 0, sizeof G);
}
static int gl_bind(ANativeWindow *w) {
    if (G.on == w && G.surf) return 0;
    gl_drop();
    if (!(G.surf = G.CreateWindowSurface(G.dpy, G.cfg, w, NULL)) || !G.MakeCurrent(G.dpy, G.surf, G.surf, G.ctx)) { G.surf = NULL; return -1; }
    G.on = w;
    if (!G.prog) {
        static const char *vs = "attribute vec2 p; varying vec2 t; void main() { t = vec2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5); gl_Position = vec4(p, 0.0, 1.0); }";
        static const char *fs = "precision mediump float; varying vec2 t; uniform sampler2D s; void main() { gl_FragColor = texture2D(s, t); }";
        GLuint a = G.CreateShader(GL_VERTEX_SHADER), b = G.CreateShader(GL_FRAGMENT_SHADER);
        G.ShaderSource(a, 1, &vs, NULL); G.CompileShader(a); G.ShaderSource(b, 1, &fs, NULL); G.CompileShader(b);
        G.prog = G.CreateProgram(); G.AttachShader(G.prog, a); G.AttachShader(G.prog, b); G.BindAttribLocation(G.prog, 0, "p"); G.LinkProgram(G.prog);
        G.GenTextures(1, &G.tex);
    }
    return 0;
}
static void gl_show(stv_canvas *c, ANativeWindow *w) {
    if (gl_bind(w)) return;
    static const float quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    G.Viewport(0, 0, ANativeWindow_getWidth(w), ANativeWindow_getHeight(w)); G.UseProgram(G.prog); G.BindTexture(GL_TEXTURE_2D, G.tex);
    G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    G.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, c->w, c->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, c->px);
    G.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad); G.EnableVertexAttribArray(0); G.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    G.SwapBuffers(G.dpy, G.surf);
}
static void *screen_thread(void *a) {
    (void)a; pthread_setname_np(pthread_self(), "startscreen");
    stv_canvas c = {0}; stv_rate rate; stv_rate_reset(&rate); int last_phase = -1, gl_tried = 0;
    pthread_mutex_lock(&S.mu);
    for (;;) {
        if (S.lost) { gl_drop(); S.lost = 0; S.released = 1; pthread_cond_broadcast(&S.cv); }
        if (V.closing) break;
        dm_cloud_info ci; memset(&ci, 0, sizeof ci); ci.progress = -1; int fire = 0;
        if (LN.on) {   // (savecloud.h: its lock is never held while this file's is taken)
            dm_cloud_info_get(&ci); int64_t tm = now_ms();
            if (LN.holding && tm - LN.hold_t0 >= 1500) { LN.holding = 0; fire = 1; LN.last_act = DM_CLOUD_ACT_RESTORE; }   // Back to the cloud save held 1.5 s
            if (LN.want && LN.gate && !lnr_busy_locked(&ci)) go_locked();
            LN.verify_ok = verify_ok_locked(&ci);
        }
        snap_t x = {V.phase, S.status, S.wait_s, S.wait_tot, V.need_net, P.done, P.total}; x.p = S.pr; snprintf(x.msg, sizeof x.msg, "%s", S.msg); x.ln = LN;
        if (x.phase == PH_NOTICE) { memcpy(x.nt_ko, NT.ko, sizeof NT.ko); memcpy(x.nt_en, NT.en, sizeof NT.en); memcpy(x.nt_detail, NT.detail, sizeof NT.detail); }
        stv_step steps[STV_NSTEPS]; memcpy(steps, V.step, sizeof steps); ANativeWindow *w = S.win; int64_t t = now_ms();
        dm_cloud_view cv; memset(&cv, 0, sizeof cv); cv.progress = -1;
        if (x.phase == PH_CLOUD || LN.on) dm_cloud_view_get(&cv);   // (savecloud.h: its lock is never held while this file's is taken)
        if (x.phase != last_phase) { stv_rate_reset(&rate); last_phase = x.phase; }
        if (x.phase == PH_DATA && (x.p.phase == DS_PH_DOWNLOAD || x.p.phase == DS_PH_UNPACK) && x.p.total)   // network bytes only: a resumed part's bytes join done at once, not the speed
            stv_rate_feed(&rate, t, x.p.phase == DS_PH_DOWNLOAD ? (double)x.p.net : (double)x.p.done, (double)(x.p.total - (x.p.done < x.p.total ? x.p.done : x.p.total)));
        if (x.phase == PH_PREP && x.pt) stv_rate_feed(&rate, t, x.pd, x.pt - x.pd);
        int urgent = x.phase == PH_NOTICE || (x.phase == PH_CLOUD && cv.urgent) || (x.phase == PH_DATA && x.status >= 0);
        int show = w && (LN.on || (x.phase != PH_NONE && (V.shown || urgent || t - V.t0 > 300))), animated = 0;   // (the launcher: at once, and with no step running)
        if (show && !gl_tried) { gl_tried = 1; G.ok = !gl_load(); if (!G.ok) { LOG("[RED] start screen: no EGL / GLES 2: the start runs without its screen"); gl_end(); } }
        if (show && G.ok) {
            pthread_mutex_unlock(&S.mu);
            if (fire && dm_cloud_act(DM_CLOUD_ACT_RESTORE)) LOG("[RED] launcher: Back to the cloud save cannot run now");
            if (x.ln.on) {   // the pages' numbers: the pack's size once its data step ended, the shader cache's, this device's save on the Cloud page
                if (!x.ln.data_known && V.step[STV_DATA].state == STV_DONE && x.phase != PH_DATA && !x.ln.vrun) { uint64_t db = data_bytes_now(); pthread_mutex_lock(&S.mu); LN.data_bytes = db; LN.data_known = 1; pthread_mutex_unlock(&S.mu); x.ln.data_bytes = db; x.ln.data_known = 1; }
                if (x.ln.page == 2 || !x.ln.shader_bytes) { int64_t sb = shader_bytes_now(); pthread_mutex_lock(&S.mu); LN.shader_bytes = sb; pthread_mutex_unlock(&S.mu); x.ln.shader_bytes = sb; }
                if (x.ln.page == 1) dm_cloud_local_refresh();
            }
            if (!R.tried) res_load();
            int ww = ANativeWindow_getWidth(w), wh = ANativeWindow_getHeight(w), cw = ww, ch = wh;
            if (cw > 2560) { cw /= 2; ch /= 2; }   // (a screen bigger than that: half its size, filtered up)
            if (cw > 0 && ch > 0 && (c.w != cw || c.h != ch)) { free(c.px); c.px = malloc((size_t)cw * ch * 4); c.w = c.px ? cw : 0; c.h = c.px ? ch : 0; }
            if (c.px) { stv_view v; dm_cloud_view dq; build_view(&v, &x, &rate, &cv, &dq, steps, &ci); stv_draw(&c, &v, t / 1000.0); animated = stv_animated(&v); gl_show(&c, w); }
            pthread_mutex_lock(&S.mu);
            if (!V.shown) LOG("start screen: shown (%lld ms after the start)", (long long)(t - V.t0));
            V.shown = 1;
        }
        int ms = animated ? 66 : 200;   // a spinner turns at 15 frames a second; else a picture when something changed (a poke) or every 200 ms
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += ms * 1000000L; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&S.cv, &S.mu, &ts);
    }
    pthread_mutex_unlock(&S.mu);
    gl_end(); res_free(); free(c.px);
    pthread_mutex_lock(&S.mu); V.closed = 1; pthread_cond_broadcast(&S.cv); pthread_mutex_unlock(&S.mu);
    return NULL;
}
void dm_sync_window(ANativeWindow *w) {   // on_window_created (w) and on_window_destroyed (NULL): a destroyed window is let go before the callback returns
    if (!S.act) return;
    pthread_mutex_lock(&S.mu);
    if (w) S.win = w;
    else if (S.win) {
        S.win = NULL;
        if (G.on && V.open && !V.closed) { S.lost = 1; S.released = 0; pthread_cond_broadcast(&S.cv);
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 2;
            while (!S.released && !V.closed) if (pthread_cond_timedwait(&S.cv, &S.mu, &ts)) break; }
    }
    pthread_mutex_unlock(&S.mu);
}
// "1.4.1-3" (porter: the game's version, then the build number) as "1.4.1 · build 3"
static void version_line(char *o, size_t n, const char *vn) {
    const char *d = strrchr(vn, '-'); int num = d && d[1]; for (const char *q = d ? d + 1 : ""; *q; q++) if (*q < '0' || *q > '9') num = 0;
    if (num) snprintf(o, n, korean() ? "%.*s · 빌드 %s" : "%.*s · build %s", (int)(d - vn), vn, d + 1); else snprintf(o, n, "%s", vn);
}
static void app_names(ANativeActivity *act, char *label, size_t lcap, char *ver, size_t vcap) {   // the app's label and versionName (on the main thread)
    JNIEnv *E = act->env; label[0] = ver[0] = 0;
    if ((*E)->PushLocalFrame(E, 32)) { jx(E, NULL, 0); return; }
    jobject a = act->clazz; jclass cA = (*E)->GetObjectClass(E, a);
    jmethodID mPm = cA ? (*E)->GetMethodID(E, cA, "getPackageManager", "()Landroid/content/pm/PackageManager;") : NULL; jobject pm = mPm && !jx(E, NULL, 0) ? (*E)->CallObjectMethod(E, a, mPm) : NULL;
    jmethodID mAi = !jx(E, NULL, 0) && cA ? (*E)->GetMethodID(E, cA, "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;") : NULL; jobject ai = mAi && !jx(E, NULL, 0) ? (*E)->CallObjectMethod(E, a, mAi) : NULL;
    jmethodID mL = ai && pm && !jx(E, NULL, 0) ? (*E)->GetMethodID(E, (*E)->GetObjectClass(E, ai), "loadLabel", "(Landroid/content/pm/PackageManager;)Ljava/lang/CharSequence;") : NULL;
    jobject cs = mL && !jx(E, NULL, 0) ? (*E)->CallObjectMethod(E, ai, mL, pm) : NULL;
    jmethodID mS = cs && !jx(E, NULL, 0) ? (*E)->GetMethodID(E, (*E)->GetObjectClass(E, cs), "toString", "()Ljava/lang/String;") : NULL;
    jstring ls = mS && !jx(E, NULL, 0) ? (*E)->CallObjectMethod(E, cs, mS) : NULL;
    const char *u = ls && !jx(E, NULL, 0) ? (*E)->GetStringUTFChars(E, ls, NULL) : NULL; if (u) { snprintf(label, lcap, "%s", u); (*E)->ReleaseStringUTFChars(E, ls, u); }
    jmethodID mN = !jx(E, NULL, 0) && cA ? (*E)->GetMethodID(E, cA, "getPackageName", "()Ljava/lang/String;") : NULL; jobject pn = mN && !jx(E, NULL, 0) ? (*E)->CallObjectMethod(E, a, mN) : NULL;
    jmethodID mPi = pm && pn && !jx(E, NULL, 0) ? (*E)->GetMethodID(E, (*E)->GetObjectClass(E, pm), "getPackageInfo", "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;") : NULL;
    jobject pi = mPi && !jx(E, NULL, 0) ? (*E)->CallObjectMethod(E, pm, mPi, pn, 0) : NULL;
    jfieldID fV = pi && !jx(E, NULL, 0) ? (*E)->GetFieldID(E, (*E)->GetObjectClass(E, pi), "versionName", "Ljava/lang/String;") : NULL;
    jstring vn = fV && !jx(E, NULL, 0) ? (*E)->GetObjectField(E, pi, fV) : NULL;
    u = vn && !jx(E, NULL, 0) ? (*E)->GetStringUTFChars(E, vn, NULL) : NULL; if (u) { snprintf(ver, vcap, "%s", u); (*E)->ReleaseStringUTFChars(E, vn, u); }
    jx(E, NULL, 0); (*E)->PopLocalFrame(E, NULL);
}
__attribute__((weak)) int vk_gpl_prepare(void (*note)(void *u, int done, int total), void *u);   // (a build without the Vulkan backend has none)
static void checked_path(char *o, size_t n) { if (S.act && S.act->internalDataPath) snprintf(o, n, "%s/nx2a_data_checked.txt", S.act->internalDataPath); else o[0] = 0; }   // when the server last answered
// The start screen, opened in onCreate before anything of the start runs: its steps as they stand, its thread waiting to show (it draws nothing in the first 300 ms)
void dm_start_screen_open(ANativeActivity *act, const char *root) {
    if (V.open) return;
    char label[160], vn[96], p[1100]; struct stat st; app_names(act, label, sizeof label, vn, sizeof vn);
    pthread_mutex_lock(&S.mu);
    S.act = act; V.t0 = now_ms(); V.closing = V.closed = V.shown = 0; V.phase = PH_NONE;
    snprintf(V.name, sizeof V.name, "%s", label); version_line(V.version, sizeof V.version, vn);
    AAsset *as = act->assetManager ? AAssetManager_open(act->assetManager, "nx2a/source.bin", AASSET_MODE_UNKNOWN) : NULL; if (as) AAsset_close(as);
    V.has_server = as != NULL; snprintf(p, sizeof p, "%s/%s", root, DS_LIST); V.first_run = V.has_server && stat(p, &st);
    static const char *const KO[] = {"게임 데이터", "셰이더 준비", "클라우드 세이브"}, *const EN[] = {"Game data", "Shaders", "Cloud save"};
    for (int i = 0; i < STV_NSTEPS; i++) { snprintf(V.step[i].name, sizeof V.step[i].name, "%s", korean() ? KO[i] : EN[i]); V.step[i].state = STV_WAIT; snprintf(V.step[i].note, sizeof V.step[i].note, "%s", L("대기", "Waiting")); }
    if (!V.has_server && !ds_bundles_dir(root)) { V.step[STV_DATA].state = STV_DONE; snprintf(V.step[STV_DATA].note, sizeof V.step[STV_DATA].note, "%s", L("준비됨", "Ready")); }
    if (!vk_gpl_prepare) { V.step[STV_SHADERS].state = STV_SKIP; snprintf(V.step[STV_SHADERS].note, sizeof V.step[STV_SHADERS].note, "%s", L("필요 없음", "Not needed")); }
    snprintf(p, sizeof p, "%s/sync.txt", root);
    if (stat(p, &st)) { V.step[STV_CLOUD].state = STV_SKIP; snprintf(V.step[STV_CLOUD].note, sizeof V.step[STV_CLOUD].note, "%s", L("기기 연결 필요", "Not linked")); }
    checked_path(p, sizeof p); size_t n = 0; char *ck = p[0] ? ds_read(p, &n, 64) : NULL; V.checked[0] = 0;
    if (ck) LN.checked_t = strtoll(ck, NULL, 10);
    if (ck) { time_t t = (time_t)strtoll(ck, NULL, 10); struct tm tm; if (t > 0 && localtime_r(&t, &tm)) snprintf(V.checked, sizeof V.checked, korean() ? "%d월 %d일" : "%d/%d", tm.tm_mon + 1, tm.tm_mday); free(ck); }
    lnr_path(p, sizeof p); char *lq = p[0] ? ds_read(p, &n, 256) : NULL; LN.quick = lq && strstr(lq, "quick=1") != NULL; free(lq);
    LN.on = !LN.quick; LN.sel[0] = 3; LN.has_server = V.has_server;
    LOG("launcher: %s", LN.on ? "shown while the start runs; the game starts with Start" : "start at once (− while it starts opens it)");
    pthread_mutex_unlock(&S.mu);
    pthread_t t;
    if (pthread_create(&t, NULL, screen_thread, NULL)) { LOG("[RED] start screen: cannot start its thread: the start runs without it"); pthread_mutex_lock(&S.mu); LN.on = 0; pthread_mutex_unlock(&S.mu); return; }
    pthread_detach(t);
    pthread_mutex_lock(&S.mu); V.open = 1; pthread_mutex_unlock(&S.mu);
}
// Before the game takes the window (front_android.c game_run, and an app that ends before its game): the screen's thread ends, EGL lets the window go, everything the screen
// held is freed. Waits for it (a frame being drawn: tens of ms)
void dm_start_screen_close(void) {
    pthread_mutex_lock(&S.mu);
    if (V.open && !V.closed) {
        V.closing = 1; pthread_cond_broadcast(&S.cv);
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 3;
        while (!V.closed) if (pthread_cond_timedwait(&S.cv, &S.mu, &ts)) { LOG("[RED] start screen: its thread did not end within 3 s"); break; }
        if (V.closed) LOG("start screen: closed, its resources freed (%s)", V.shown ? "it was shown" : "never shown: the start took under 300 ms");
    }
    V.open = 0; V.phase = PH_NONE;
    pthread_mutex_unlock(&S.mu);
}
// The input before the game that is not the cloud's (front_android.c on_input_early): the first start's Retry when it has no network. 1 when it was the screen's
int dm_start_input(const AInputEvent *e) {
    pthread_mutex_lock(&S.mu); int on = V.phase == PH_DATA && V.need_net && S.status >= 0; ANativeWindow *w = S.win; int ws = S.wait_s, wt = S.wait_tot; pthread_mutex_unlock(&S.mu);
    if (!on) return lnr_input(e);   // (the launcher's, and − at a start without it)
    int go = 0;
    if (AInputEvent_getType(e) == AINPUT_EVENT_TYPE_KEY) {
        int k = AKeyEvent_getKeyCode(e);
        if (k == AKEYCODE_VOLUME_UP || k == AKEYCODE_VOLUME_DOWN || k == AKEYCODE_VOLUME_MUTE) return 0;
        go = AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_DOWN && !AKeyEvent_getRepeatCount(e) && (k == AKEYCODE_BUTTON_A || k == AKEYCODE_DPAD_CENTER || k == AKEYCODE_ENTER || k == AKEYCODE_BUTTON_START);
    } else if (AInputEvent_getType(e) == AINPUT_EVENT_TYPE_MOTION && (AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_MASK) == AMOTION_EVENT_ACTION_UP && w) {
        dm_cloud_view q; need_net_view(&q, ws, wt); int kind = 0;
        go = stv_hit(&q, ANativeWindow_getWidth(w), ANativeWindow_getHeight(w), AMotionEvent_getX(e, 0), AMotionEvent_getY(e, 0), &kind) == 0;
    }
    if (go) { LOG("data: the player asked to try again now"); pthread_mutex_lock(&S.mu); S.retry_now = 1; pthread_cond_broadcast(&S.cv); pthread_mutex_unlock(&S.mu); }
    return 1;
}
// 1: the device has a connected network (ConnectivityManager.getActiveNetworkInfo), 0: none, -1: not known (a Java failure). ctx: the activity
int dm_net_state(JNIEnv *E, jobject ctx) {
    if (!E || !ctx) return -1;
    if ((*E)->PushLocalFrame(E, 16)) { jx(E, NULL, 0); return -1; }
    int r = -1;
    jclass cc = (*E)->GetObjectClass(E, ctx); jmethodID ms = cc ? (*E)->GetMethodID(E, cc, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;") : NULL;
    jstring nm = ms && !jx(E, NULL, 0) ? (*E)->NewStringUTF(E, "connectivity") : NULL;
    jobject cm = nm && !jx(E, NULL, 0) ? (*E)->CallObjectMethod(E, ctx, ms, nm) : NULL;
    jmethodID ma = cm && !jx(E, NULL, 0) ? (*E)->GetMethodID(E, (*E)->GetObjectClass(E, cm), "getActiveNetworkInfo", "()Landroid/net/NetworkInfo;") : NULL;
    if (ma && !jx(E, NULL, 0)) {
        jobject ni = (*E)->CallObjectMethod(E, cm, ma);
        if (!jx(E, NULL, 0)) {
            if (!ni) r = 0;
            else { jmethodID mc = (*E)->GetMethodID(E, (*E)->GetObjectClass(E, ni), "isConnected", "()Z");
                   if (mc && !jx(E, NULL, 0)) { jboolean b = (*E)->CallBooleanMethod(E, ni, mc); if (!jx(E, NULL, 0)) r = b ? 1 : 0; } }
        }
    }
    jx(E, NULL, 0); (*E)->PopLocalFrame(E, NULL);
    return r;
}

// ---- the sync thread ----
// Without a network the start does not wait: a device with none (ConnectivityManager) asks no server at all; one with a network the server does not answer from gives it 3 s
// in all (the pointer and the list together) when the files folder holds a pack, which the game then starts with (the data's step says offline, and when the server
// last answered). A folder without a whole pack cannot start: the screen asks for a connection (Retry), and tries again after 2, 4, 8 … 60 s, at once when the device's
// network comes back
static void phase_end(void) { char b = 1; if (write(S.pipe[1], &b, 1) != 1) LOG("[RED] start: cannot wake the main thread"); }   // the main thread's sync_ready runs the next step
static void *sync_thread(void *a) {
    (void)a; pthread_setname_np(pthread_self(), "datasync");
    JavaVM *vm = S.act->vm; jhttp h = {0}; int rc = DS_OFFLINE, net = S.base[0] != 0, unreached = 0;   // !net: no server, only the folder's bundles to unpack (dm_sync_begin)
    int64_t t_start = now_ms(); struct stat lst; char lp[1100]; snprintf(lp, sizeof lp, "%s/%s", S.root, DS_LIST); int have_list = !stat(lp, &lst);
    if (net && ((*vm)->AttachCurrentThread(vm, &h.E, NULL) != JNI_OK || jhttp_init(&h))) LOG("[RED] data: no Java for the download: the game starts with the data the app has");
    else {
        ds_ctx c = {.root = S.root, .package = S.package, .fetch = net ? http_fetch : NULL, .fu = &h, .free_bytes = free_bytes, .note = sync_note, .log = sync_log};
        int delay = 2, last_net = -1;
        for (int round = 1;; round++) {
            int ns = net ? dm_net_state(h.E, S.act->clazz) : -1;
            pthread_mutex_lock(&S.mu); S.status = -1; S.msg[0] = 0; S.retry_now = 0; S.netdown = ns == 0;
            S.deadline = round == 1 && have_list ? now_ms() + 3000 : 0;   // the folder has a pack: the server gets 3 s in all, then the game starts with it
            pthread_mutex_unlock(&S.mu);
            if (ns == 0 && round == 1) LOG("data: the device has no network: the server is not asked");
            if (net && PT.on && !PT.fresh) pointer_resolve(&h);   // (again at each try until the pointer itself answers: a launch before the network is up reaches it later)
            rc = ds_sync(&c); unreached = rc == DS_OFFLINE && c.unreached;
            if (rc == DS_DONE || rc == DS_UPTODATE || rc == DS_OFFLINE) break;
            if (c.pr.phase == DS_PH_DOWNLOAD && c.pr.done) delay = 2;   // it moved on: the next break waits from the start again
            LOG("[RED] data: try %d failed (%d): %s; again in %d s", round, rc, c.err, delay);
            char msg[256]; snprintf(msg, sizeof msg, "%s", c.err);   // (the log keeps c.err in English; the screen says how much space in the app's language)
            if (rc == DS_ERR_SPACE) {   // (under 0.1 GB short: MB, else it would say 0.0 GB more)
                double more = c.space_need / 1073741824.0, fr = c.space_free / 1073741824.0;
                if (more < 0.1) { if (korean()) snprintf(msg, sizeof msg, "%.0fMB가 더 필요해요 (남은 공간 %.1fGB)", c.space_need / 1048576.0, fr); else snprintf(msg, sizeof msg, "%.0f MB more space is needed; the device has %.1f GB free", c.space_need / 1048576.0, fr); }
                else if (korean()) snprintf(msg, sizeof msg, "%.1fGB가 더 필요해요 (남은 공간 %.1fGB)", more, fr);
            }
            int need_net = rc == DS_ERR_NET && net;   // (DS_ERR_NET with a server: the folder has no whole pack, so the game cannot start without it)
            pthread_mutex_lock(&S.mu); V.need_net = need_net; pthread_mutex_unlock(&S.mu);
            last_net = ns;
            for (int s = delay; s > 0; s--) {   // the countdown, a second a step; Retry, or the device's network coming back, ends it
                pthread_mutex_lock(&S.mu); S.status = rc; S.wait_s = s; S.wait_tot = delay; snprintf(S.msg, sizeof S.msg, "%s", need_net ? "" : msg); pthread_cond_broadcast(&S.cv);
                struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 1; int now_retry = S.retry_now;
                while (!S.retry_now && pthread_cond_timedwait(&S.cv, &S.mu, &ts) == 0) {}
                now_retry = S.retry_now; pthread_mutex_unlock(&S.mu);
                if (now_retry) break;
                if (need_net) { int n2 = dm_net_state(h.E, S.act->clazz); if (n2 == 1 && last_net == 0) { LOG("data: the device's network is back: trying again now"); break; } if (n2 >= 0) last_net = n2; }
            }
            delay = rc == DS_ERR_PACKAGE || rc == DS_ERR_SPACE || rc == DS_ERR_LOCAL ? 30 : delay * 2 > 60 ? 60 : delay * 2;   // (DS_ERR_LOCAL: until an install pushes what is missing)
        }
        LOG("%s (%lld ms)", rc == DS_DONE ? c.pr.net ? "data: downloaded, the files folder holds the server's pack"
            : "data: unpacked the bundles in the folder, the files folder holds the server's pack" : rc == DS_UPTODATE ? "data: the files folder holds the server's pack already"
            : "data: no server, the files folder holds a whole pack: the game starts with it", (long long)(now_ms() - t_start));
        if (net && (rc == DS_DONE || rc == DS_UPTODATE)) {   // the server answered: kept for the offline note ("last checked")
            char p[700], t[32]; checked_path(p, sizeof p); int n = snprintf(t, sizeof t, "%lld\n", (long long)time(NULL));
            if (p[0] && ds_write(p, t, (size_t)n)) LOG("[RED] data: cannot write %s (errno %d)", p, errno);
        }
        if (net) {
            if (h.buf) (*h.E)->DeleteGlobalRef(h.E, h.buf);
            (*vm)->DetachCurrentThread(vm);
        }
    }
    char note[96];
    if (rc == DS_OFFLINE && net) { if (V.checked[0]) snprintf(note, sizeof note, korean() ? "오프라인 · 마지막 확인 %s" : "Offline · last checked %s", V.checked); else snprintf(note, sizeof note, "%s", L("오프라인", "Offline")); }
    else snprintf(note, sizeof note, "%s", rc == DS_DONE && net ? L("받음", "Downloaded") : rc == DS_OFFLINE ? L("준비됨", "Ready") : L("최신", "Up to date"));
    pthread_mutex_lock(&S.mu); V.need_net = 0; S.status = -1; V.phase = PH_NONE; V.unreached = unreached;
    LN.data_rc = rc; LN.data_net = net && (rc == DS_DONE || rc == DS_UPTODATE); LN.data_off = net && rc == DS_OFFLINE; if (LN.data_net) LN.checked_t = (int64_t)time(NULL); V.step[STV_DATA].state = STV_DONE; snprintf(V.step[STV_DATA].note, sizeof V.step[STV_DATA].note, "%s", note);
    pthread_cond_broadcast(&S.cv); pthread_mutex_unlock(&S.mu);
    phase_end();
    return NULL;
}
static int sync_ready(int fd, int events, void *data) {   // the main thread's looper: a step of the start ended, the next one (or the game) starts
    (void)events; (void)data; char b; if (read(fd, &b, 1) != 1) return 1;
    void (*d)(void) = S.done; S.done = NULL; if (d) d();
    return 0;   // the fd is removed
}
static int package_name(ANativeActivity *act, char *out, size_t cap) {
    JNIEnv *E = act->env; jclass c = (*E)->GetObjectClass(E, act->clazz); jmethodID m = c ? (*E)->GetMethodID(E, c, "getPackageName", "()Ljava/lang/String;") : NULL;
    jstring s = m ? (*E)->CallObjectMethod(E, act->clazz, m) : NULL; if (jx(E, NULL, 0) || !s) return -1;
    const char *u = (*E)->GetStringUTFChars(E, s, NULL); if (!u) return -1;
    snprintf(out, cap, "%s", u); (*E)->ReleaseStringUTFChars(E, s, u); return 0;
}
static int phase_pipe(const char *what) {   // a pipe of its own for each step (called from the last step's sync_ready, whose fd the looper removes when it returns)
    ALooper *lp = ALooper_forThread(); int np[2];
    if (!lp || pipe(np)) { LOG("[RED] %s: no looper on the main thread", what); return -1; }
    if (ALooper_addFd(lp, np[0], ALOOPER_POLL_CALLBACK, ALOOPER_EVENT_INPUT, sync_ready, NULL) != 1) { LOG("[RED] %s: no looper on the main thread", what); close(np[0]); close(np[1]); return -1; }
    S.pipe[0] = np[0]; S.pipe[1] = np[1]; return 0;
}
// 1: the APK has a data server (or the files folder has bundles to unpack), the sync runs and done() is called on the main thread once the game may start; 0: none, start the game now
int dm_sync_begin(ANativeActivity *act, const char *root, void (*done)(void)) {
    AAsset *as = act->assetManager ? AAssetManager_open(act->assetManager, "nx2a/source.bin", AASSET_MODE_BUFFER) : NULL;
    int unpack = ds_bundles_dir(root);   // bundles porter install pushed: unpacked before the game starts, with a server or without
    if (!as && !unpack) return 0;
    if (as) {
        static ds_src src; const void *b = AAsset_getBuffer(as); int bad = !b || ds_source(b, (size_t)AAsset_getLength(as), &src);
        AAsset_close(as);
        char *cred = src.cred;
        if (!bad) snprintf(S.base, sizeof S.base, "%s", src.url);
        if (bad) {
            S.base[0] = 0; memset(&src, 0, sizeof src);
            LOG("[RED] data: the APK's nx2a/source.bin cannot be read: the game starts with the data the app has%s", unpack ? " (once its bundles are unpacked)" : "");
            if (!unpack) return 0;
        } else {
            if (cred[0]) { char b64[sizeof S.auth - 8]; if (strlen(cred) * 4 / 3 + 8 < sizeof b64) { ds_base64(b64, cred, strlen(cred)); snprintf(S.auth, sizeof S.auth, "Basic %s", b64); memset(b64, 0, sizeof b64); } }
            if (src.version == 2) {   // a pointer: the sync thread decides the server (pointer_resolve); the asset's own is the fallback
                PT.on = 1; memcpy(PT.key, src.key, sizeof PT.key); snprintf(PT.pointer, sizeof PT.pointer, "%s", src.pointer); snprintf(PT.sub, sizeof PT.sub, "%s", src.sub);
                snprintf(PT.base, sizeof PT.base, "%s", S.base); snprintf(PT.auth, sizeof PT.auth, "%s", S.auth);
                if (act->internalDataPath && (size_t)snprintf(PT.cache, sizeof PT.cache, "%s/nx2a_pointer.txt", act->internalDataPath) >= sizeof PT.cache) PT.cache[0] = 0;   // (none: no cache)
            }
            memset(&src, 0, sizeof src);
        }
    }
    if (package_name(act, S.package, sizeof S.package)) { LOG("[RED] data: the app's package is not known: the game starts with the data the app has"); return 0; }
    snprintf(S.root, sizeof S.root, "%s", root);
    pthread_mutex_lock(&S.mu); S.done = done; S.act = act; int ok = !phase_pipe("data"); pthread_mutex_unlock(&S.mu);
    if (!ok) { LOG("[RED] data: the game starts with the data the app has"); return 0; }
    if (S.base[0] && PT.on) LOG("data: the data server is read from the pointer %s first (else the last one read, else %s)", PT.pointer, S.base);
    else if (S.base[0]) LOG("data: the app's data comes from %s%s", S.base, S.auth[0] ? " (Basic authentication)" : "");
    else LOG("data: no data server; the bundles in the files folder are unpacked before the game starts");
    pthread_mutex_lock(&S.mu); V.phase = PH_DATA; V.step[STV_DATA].state = STV_RUN; S.status = -1; pthread_cond_broadcast(&S.cv); pthread_mutex_unlock(&S.mu);
    pthread_t t;
    if (pthread_create(&t, NULL, sync_thread, NULL)) { LOG("[RED] data: cannot start the download thread"); pthread_mutex_lock(&S.mu); V.phase = PH_NONE; pthread_mutex_unlock(&S.mu); phase_end(); return 1; }
    pthread_detach(t);
    return 1;
}

// ---- the shader preparation: gfx_vk_gpl.inc vk_gpl_prepare on a thread of its own, the screen's second step ----
static void prep_note(void *u, int done, int total) { (void)u; pthread_mutex_lock(&S.mu); P.done = done; P.total = total; pthread_cond_broadcast(&S.cv); pthread_mutex_unlock(&S.mu); }
static void *prep_thread(void *a) {
    (void)a; pthread_setname_np(pthread_self(), "shaderprep");
    int64_t t0 = now_ms(); int r = vk_gpl_prepare(prep_note, NULL);
    pthread_mutex_lock(&S.mu); V.phase = PH_NONE; V.step[STV_SHADERS].state = STV_DONE; snprintf(V.step[STV_SHADERS].note, sizeof V.step[STV_SHADERS].note, "%s", L("완료", "Done"));
    pthread_cond_broadcast(&S.cv); pthread_mutex_unlock(&S.mu);
    LOG("shaders: %s (%lld ms)", r ? "prepared: the game starts" : "nothing to prepare: the game starts", (long long)(now_ms() - t0));
    phase_end();
    return NULL;
}
// 1: the preparation runs and done() is called on the main thread once the game may start; 0: start the game now. w: the window if there is one already
int dm_prep_begin(ANativeActivity *act, ANativeWindow *w, void (*done)(void)) {
    if (!vk_gpl_prepare) return 0;
    pthread_mutex_lock(&S.mu);
    int ok = !phase_pipe("shaders");
    if (ok) { S.act = act; if (w && !S.win) S.win = w; S.done = done; P.on = 1; P.done = P.total = 0; V.phase = PH_PREP; V.step[STV_SHADERS].state = STV_RUN; pthread_cond_broadcast(&S.cv); }
    pthread_mutex_unlock(&S.mu);
    if (!ok) { LOG("[RED] shaders: the game starts without the preparation"); return 0; }
    pthread_t t;
    if (pthread_create(&t, NULL, prep_thread, NULL)) { LOG("[RED] shaders: cannot start the preparation thread"); pthread_mutex_lock(&S.mu); V.phase = PH_NONE; pthread_mutex_unlock(&S.mu); phase_end(); return 1; }
    pthread_detach(t);
    return 1;
}

// ---- the save's cloud sync at the start (savecloud_android.c): the screen's third step; done() on the main thread once dm_cloud_screen_end ends it ----
int dm_cloud_screen_begin(ANativeActivity *act, ANativeWindow *w, void (*done)(void)) {
    pthread_mutex_lock(&S.mu);
    int ok = V.open && !V.closing && !phase_pipe("cloud");
    if (ok) { S.act = act; if (w && !S.win) S.win = w; S.done = done; V.phase = PH_CLOUD; V.step[STV_CLOUD].state = STV_RUN; pthread_cond_broadcast(&S.cv); }
    pthread_mutex_unlock(&S.mu);
    return ok;
}
// 1: the data's step of this start could not reach its server and the game starts with the pack on the device (savecloud_android.c: then the cloud's step asks
// nothing of its server either, savesync.h ss_start_offline)
int dm_data_unreached(void) { pthread_mutex_lock(&S.mu); int u = V.unreached; pthread_mutex_unlock(&S.mu); return u; }
// ---- a notice the start stops at (front_android.c: data rewritten for another host contract): the start screen shows it until the player leaves the app; the game is
// not started. ko, en: the title and two lines in each language; detail: the log's line, small. 0 when the screen is not open (the caller ends the app) ----
int dm_notice_begin(ANativeActivity *act, ANativeWindow *w, const char *const ko[3], const char *const en[3], const char *detail) {
    pthread_mutex_lock(&S.mu);
    int ok = V.open && !V.closing;
    if (ok) {
        S.act = act; if (w && !S.win) S.win = w; S.done = NULL;
        for (int i = 0; i < 3; i++) { snprintf(NT.ko[i], sizeof NT.ko[i], "%s", ko[i]); snprintf(NT.en[i], sizeof NT.en[i], "%s", en[i]); }
        snprintf(NT.detail, sizeof NT.detail, "%s", detail ? detail : "");
        V.phase = PH_NOTICE; pthread_cond_broadcast(&S.cv);
    }
    pthread_mutex_unlock(&S.mu);
    return ok;
}
void dm_cloud_screen_end(void) {
    pthread_mutex_lock(&S.mu); int on = V.phase == PH_CLOUD; if (on) { V.phase = PH_NONE; pthread_cond_broadcast(&S.cv); } pthread_mutex_unlock(&S.mu);
    if (on) phase_end();
}
void dm_cloud_screen_poke(void) { pthread_mutex_lock(&S.mu); pthread_cond_broadcast(&S.cv); pthread_mutex_unlock(&S.mu); }
int dm_cloud_screen_hit(float x, float y, int *kind) {
    pthread_mutex_lock(&S.mu); ANativeWindow *w = S.win; int ww = w ? ANativeWindow_getWidth(w) : 0, wh = w ? ANativeWindow_getHeight(w) : 0; pthread_mutex_unlock(&S.mu);
    if (ww <= 0 || wh <= 0) return -1;
    dm_cloud_view v; dm_cloud_view_get(&v);
    return stv_hit(&v, ww, wh, x, y, kind);
}

// ---- the launcher (Launcher.dc.html and its pages): the start screen from onCreate on while the start runs behind it, the game started by its Start (A) once the
// start is ready (dm_launcher_gate, front_android.c, where the game would start). Off when the game's "start at once" is on (nx2a_launcher.txt in the app's storage):
// then the start is as it was (the steps, nothing drawn in the first 300 ms), unless − (Select) is pressed while it runs. A question of the start (the cloud's cards,
// the first start's download) is drawn over it; everything it loaded goes with the screen (dm_start_screen_close) ----
static void lnr_path(char *o, size_t n) { if (S.act && S.act->internalDataPath) snprintf(o, n, "%s/nx2a_launcher.txt", S.act->internalDataPath); else o[0] = 0; }
static void lnr_save_quick(int on) {
    char p[700]; lnr_path(p, sizeof p); if (!p[0]) return;
    char t[32]; int n = snprintf(t, sizeof t, "quick=%d\n", on ? 1 : 0); if (ds_write(p, t, (size_t)n)) LOG("[RED] launcher: cannot write %s (errno %d)", p, errno);
}
static int lnr_busy_locked(const dm_cloud_info *ci) { return LN.vrun || ci->act || ci->checking; }   // a launcher's action runs: the game waits for it
static void go_locked(void) {   // the game starts (the main thread's gate_ready)
    char b = 1; LN.gate = 0; LN.want = 0;
    if (LN.gpipe[1] < 0 || write(LN.gpipe[1], &b, 1) != 1) LOG("[RED] launcher: cannot wake the main thread");
    else LOG("launcher: the game starts%s", LN.nosync ? " (without the sync)" : "");
}
static int gate_ready(int fd, int events, void *data) {
    (void)events; (void)data; char b; if (read(fd, &b, 1) != 1) return 1;
    pthread_mutex_lock(&S.mu); void (*go)(void) = LN.go; LN.go = NULL; int w = LN.gpipe[1]; LN.gpipe[0] = LN.gpipe[1] = -1; pthread_mutex_unlock(&S.mu);
    close(fd); if (w >= 0) close(w);
    if (go) go();
    return 0;   // (the fd is removed)
}
// front_android.c, on the main thread where the game would start: 1 when the launcher holds it (go() is called on the main thread once Start is pressed and the start is
// ready), 0 to start it now (no launcher, or Start was pressed before and nothing runs)
int dm_launcher_gate(void (*go)(void)) {
    pthread_mutex_lock(&S.mu);
    if (!LN.on || !V.open || V.closing) { pthread_mutex_unlock(&S.mu); return 0; }
    dm_cloud_info ci; dm_cloud_info_get(&ci);
    if (LN.want && !lnr_busy_locked(&ci)) { LN.want = 0; pthread_mutex_unlock(&S.mu); LOG("launcher: ready, Start was pressed before: the game starts"); return 0; }
    ALooper *lp = ALooper_forThread(); int np[2];
    if (!lp || pipe(np)) { pthread_mutex_unlock(&S.mu); LOG("[RED] launcher: no looper: the game starts"); return 0; }
    if (ALooper_addFd(lp, np[0], ALOOPER_POLL_CALLBACK, ALOOPER_EVENT_INPUT, gate_ready, NULL) != 1) { close(np[0]); close(np[1]); pthread_mutex_unlock(&S.mu); LOG("[RED] launcher: no looper: the game starts"); return 0; }
    LN.gpipe[0] = np[0]; LN.gpipe[1] = np[1]; LN.go = go; LN.gate = 1; pthread_cond_broadcast(&S.cv);
    pthread_mutex_unlock(&S.mu);
    LOG("launcher: the start is ready; the game starts with Start");
    return 1;
}
void dm_launcher_start_now(int nosync) {   // (savecloud_android.c, a card's play without the sync: as Start pressed)
    pthread_mutex_lock(&S.mu); if (LN.on) { LN.want = 1; LN.nosync |= nosync; LN.page = 0; pthread_cond_broadcast(&S.cv); } pthread_mutex_unlock(&S.mu);
}
static uint64_t data_bytes_now(void) {   // the pack's size, from files.json ("bytes", else its files summed); 0 when there is none
    char p[1100]; snprintf(p, sizeof p, "%s/%s", S.root[0] ? S.root : getenv("DM_ROOT") ? getenv("DM_ROOT") : ".", DS_LIST);
    size_t n = 0; char *t = ds_read(p, &n, 256u << 20); ds_list l; uint64_t b = 0;
    if (t && !ds_parse(t, n, &l)) { b = l.bytes; if (!b) for (int i = 0; i < l.n; i++) b += l.f[i].size; ds_free(&l); }
    free(t); return b;
}
static int64_t shader_bytes_now(void) {   // the shader cache: the pipeline cache and the preparation's mark (gfx_vk_pipeline_cache.inc, gfx_vk_gpl.inc)
    const char *root = getenv("DM_ROOT"); char p[700]; struct stat st; int64_t b = 0; static const char *const F[2] = {"vk_pipeline_cache.bin", "vk_gpl.id"};
    for (int i = 0; i < 2 && root; i++) { snprintf(p, sizeof p, "%s/%s", root, F[i]); if (!stat(p, &st)) b += (int64_t)st.st_size; }
    return b;
}
static void shader_clear(void) {   // the next start prepares the shaders again (this run's game keeps what it loaded)
    const char *root = getenv("DM_ROOT"); char p[700]; static const char *const F[2] = {"vk_pipeline_cache.bin", "vk_gpl.id"}; int bad = 0;
    for (int i = 0; i < 2 && root; i++) { snprintf(p, sizeof p, "%s/%s", root, F[i]); if (unlink(p) && errno != ENOENT) bad = 1; }
    LOG("launcher: the shader cache %s", bad ? "could not be removed whole" : "removed: the next start prepares the shaders again");
    pthread_mutex_lock(&S.mu); snprintf(LN.amsg, sizeof LN.amsg, "%s", bad ? L("셰이더 캐시를 다 지우지 못했어요", "The shader cache could not be cleared whole") : L("셰이더 캐시를 비웠어요 · 다음 시작 때 다시 준비해요", "Cleared · the shaders are prepared again at the next start"));
    LN.shader_bytes = shader_bytes_now(); pthread_cond_broadcast(&S.cv); pthread_mutex_unlock(&S.mu);
}
static void verify_note(void *u, const ds_progress *p) { (void)u; pthread_mutex_lock(&S.mu); LN.vpr = *p; pthread_cond_broadcast(&S.cv); pthread_mutex_unlock(&S.mu); }
static void *verify_thread(void *a) {   // Check the data again: every file's SHA-256 against the server's list, what is damaged downloaded again (datasync.h ds_ctx.verify)
    (void)a; pthread_setname_np(pthread_self(), "dataverify");
    JavaVM *vm = S.act->vm; jhttp h = {0}; int rc = DS_ERR_NET, fixed = 0; char err[256] = "";
    if ((*vm)->AttachCurrentThread(vm, &h.E, NULL) != JNI_OK || jhttp_init(&h)) LOG("[RED] launcher: no Java for the data's check");
    else {
        pthread_mutex_lock(&S.mu); S.deadline = 0; S.netdown = 0; pthread_mutex_unlock(&S.mu);
        ds_ctx c = {.root = S.root, .package = S.package, .fetch = http_fetch, .fu = &h, .free_bytes = free_bytes, .note = verify_note, .log = sync_log, .verify = 1};
        int64_t t0 = now_ms(); rc = ds_sync(&c); fixed = c.fixed; snprintf(err, sizeof err, "%s", c.err);
        LOG("launcher: the data checked file by file: %s (%lld ms)", rc == DS_DONE ? "damaged files downloaded again" : rc == DS_UPTODATE ? "nothing damaged" : err, (long long)(now_ms() - t0));
        if (rc == DS_DONE || rc == DS_UPTODATE) { char p[700], t[32]; checked_path(p, sizeof p); int n = snprintf(t, sizeof t, "%lld\n", (long long)time(NULL)); if (p[0]) ds_write(p, t, (size_t)n); }
        if (h.buf) (*h.E)->DeleteGlobalRef(h.E, h.buf);
        (*vm)->DetachCurrentThread(vm);
    }
    pthread_mutex_lock(&S.mu);
    LN.vrun = 0; LN.vbad = rc != DS_DONE && rc != DS_UPTODATE;
    if (rc == DS_DONE) snprintf(LN.amsg, sizeof LN.amsg, korean() ? "망가진 파일 %d개를 다시 받았어요" : "%d damaged files downloaded again", fixed);
    else if (rc == DS_UPTODATE) snprintf(LN.amsg, sizeof LN.amsg, "%s", L("망가진 파일이 없어요", "No file is damaged"));
    else snprintf(LN.amsg, sizeof LN.amsg, "%s", ds_error_text(rc));
    if (!LN.vbad) { LN.checked_t = (int64_t)time(NULL); LN.data_known = 0; }   // (counted again)
    pthread_cond_broadcast(&S.cv); pthread_mutex_unlock(&S.mu);
    return NULL;
}
static int verify_ok_locked(const dm_cloud_info *ci) { return S.base[0] && LN.data_net && !LN.vrun && V.phase == PH_NONE && !lnr_busy_locked(ci); }

// what a key does on the launcher's page (the main thread)
enum { K_UP, K_DOWN, K_LEFT, K_RIGHT, K_A, K_B };
static void lnr_activate(int page, int sel, int touch) {   // A (or a touch) on the page's sel
    dm_cloud_info ci; dm_cloud_info_get(&ci);
    pthread_mutex_lock(&S.mu);
    int act = 0;   // what runs once the lock is let go: 1 play without the sync, 2 a mode, 3 Upload now, 4 verify, 5 shader cache, 6 a link code, 7 unlink, 8 linked: Continue, 9 the link cancelled
    if (page == 0) {
        if (sel == 0) { LN.want = 1; LN.nosync = 1; act = 1; }
        else if (sel == 1) { LN.page = 1; LN.sel[1] = cp_link_of(&ci) == 1 ? 5 : ci.mode >= 0 && ci.mode <= 2 ? ci.mode : 0; LN.last_act = 0; LN.unarm = 0; }
        else if (sel == 2) { LN.page = 2; LN.sel[2] = 0; LN.amsg[0] = 0; }
        else if (sel == 3) { LN.want = 1; LOG("launcher: Start%s", LN.gate ? "" : " (the start is not ready: it starts once it is)"); }
    } else if (page == 1) {
        if (sel <= 2) act = 2;
        else if (sel == 3 && ci.can_upload) { act = 3; LN.last_act = DM_CLOUD_ACT_UPLOAD; }
        else if (sel == 4 && ci.can_restore && !touch) { LN.holding = 1; LN.hold_t0 = now_ms(); LN.hold_touch = 0; }
        else if (sel == 5 && cp_link_of(&ci) == 1) act = 6;
        else if (sel == 5 && cp_link_of(&ci) == 2 && ci.can_unlink) { if (LN.unarm > now_ms()) { LN.unarm = 0; act = 7; } else LN.unarm = now_ms() + 4000; }
    } else if (page == 3) {
        dm_cloud_link k; dm_cloud_link_get(&k);
        if (k.state == DM_LINK_DONE) act = 8;
        else if ((k.state == DM_LINK_EXPIRED || k.state == DM_LINK_FAIL) && sel == 0) act = 6;
        else if (sel == 1 || (touch && (k.state == DM_LINK_CODE || k.state == DM_LINK_BUSY))) act = 9;   // Cancel (a touch on the code's one button)
    } else if (page == 2) {
        if (sel == 0) { LN.quick = !LN.quick; lnr_save_quick(LN.quick); LOG("launcher: start at once %s", LN.quick ? "on (− at the start opens the launcher)" : "off"); }
        else if (sel == 1 && verify_ok_locked(&ci)) { act = 4; LN.vrun = 1; LN.amsg[0] = 0; memset(&LN.vpr, 0, sizeof LN.vpr); }
        else if (sel == 2 && LN.shader_bytes > 0 && V.phase != PH_PREP) act = 5;
    }
    pthread_cond_broadcast(&S.cv); pthread_mutex_unlock(&S.mu);
    if (act == 1) { LOG("launcher: Play without the sync"); if (dm_cloud_play_unsynced() < 0) { pthread_mutex_lock(&S.mu); LN.nosync = 0; pthread_mutex_unlock(&S.mu); } }
    else if (act == 2) dm_cloud_set_mode(sel);
    else if (act == 3) dm_cloud_act(DM_CLOUD_ACT_UPLOAD);
    else if (act == 4) { pthread_t t; if (pthread_create(&t, NULL, verify_thread, NULL)) { pthread_mutex_lock(&S.mu); LN.vrun = 0; pthread_mutex_unlock(&S.mu); } else pthread_detach(t); }
    else if (act == 5) shader_clear();
    else if (act == 6) {   // a code (again): the link page, or back to the Cloud save page when the link cannot run now
        pthread_mutex_lock(&S.mu); LN.page = 3; LN.sel[3] = 0; pthread_cond_broadcast(&S.cv); pthread_mutex_unlock(&S.mu);
        if (dm_cloud_link_begin()) { LOG("[RED] launcher: the device link cannot run now"); pthread_mutex_lock(&S.mu); LN.page = 1; LN.sel[1] = 5; pthread_cond_broadcast(&S.cv); pthread_mutex_unlock(&S.mu); }
    }
    else if (act == 7) { if (dm_cloud_unlink()) LOG("[RED] launcher: Unlink this app cannot run now"); pthread_mutex_lock(&S.mu); LN.sel[1] = 5; pthread_cond_broadcast(&S.cv); pthread_mutex_unlock(&S.mu); }
    else if (act == 8) { pthread_mutex_lock(&S.mu); LN.page = 0; LN.sel[0] = 3; pthread_cond_broadcast(&S.cv); pthread_mutex_unlock(&S.mu); dm_cloud_link_end(); }
    else if (act == 9) { dm_cloud_link_cancel(); pthread_mutex_lock(&S.mu); LN.page = 1; LN.sel[1] = 5; pthread_cond_broadcast(&S.cv); pthread_mutex_unlock(&S.mu); }
}
static void lnr_hold_end(int touch) {   // A (or the finger) let go of Back to the cloud save before 1.5 s: "hold it"
    pthread_mutex_lock(&S.mu);
    if (LN.holding && LN.hold_touch == touch) { LN.holding = 0; LN.hint_until = now_ms() + 1500; pthread_cond_broadcast(&S.cv); }
    pthread_mutex_unlock(&S.mu);
}
static void lnr_key(int k) {
    pthread_mutex_lock(&S.mu); int page = LN.page, *sel = &LN.sel[page], s0 = *sel, want = LN.want, go = -1;
    if (want && page == 0) { if (k == K_B) { LN.want = LN.nosync = 0; LOG("launcher: Start cancelled"); } pthread_cond_broadcast(&S.cv); pthread_mutex_unlock(&S.mu); return; }
    if (LN.holding && k != K_A) LN.holding = 0;
    if (page == 0) { if (k == K_UP && s0 > 0) (*sel)--; else if (k == K_DOWN && s0 < 3) (*sel)++; else if (k == K_A) go = s0; }
    else if (page == 1) {   // the right column top to bottom: linked Unlink (5), Upload now (3), Back (4); not linked Link this device (5) only; else 3, 4
        dm_cloud_info ci; dm_cloud_info_get(&ci); int lk = cp_link_of(&ci);
        if (k != K_A) LN.unarm = 0;
        if (k == K_UP) *sel = s0 <= 2 ? (s0 > 0 ? s0 - 1 : 0) : lk == 1 ? 5 : lk == 2 ? (s0 == 4 ? 3 : 5) : 3;
        else if (k == K_DOWN) *sel = s0 <= 2 ? (s0 < 2 ? s0 + 1 : 2) : lk == 1 ? 5 : lk == 2 ? (s0 == 5 ? 3 : 4) : 4;
        else if (k == K_RIGHT && s0 <= 2) *sel = lk == 1 ? 5 : 3; else if (k == K_LEFT && s0 >= 3) *sel = ci.mode >= 0 && ci.mode <= 2 ? ci.mode : 0;
        else if (k == K_A) go = s0; else if (k == K_B) { LN.page = 0; LN.sel[0] = 1; }
    } else if (page == 3) {   // the link: ← → between New code and Cancel, A chooses (Continue once linked), B cancels (or continues)
        dm_cloud_link lkk; dm_cloud_link_get(&lkk); int two = lkk.state == DM_LINK_EXPIRED || lkk.state == DM_LINK_FAIL;
        if ((k == K_LEFT || k == K_RIGHT) && two) *sel = k == K_LEFT ? 0 : 1;
        else if (k == K_A && (two || lkk.state == DM_LINK_DONE)) go = two ? s0 : 0;
        else if (k == K_B) go = lkk.state == DM_LINK_DONE ? 0 : 1;
    } else {
        if (k == K_UP) *sel = 0; else if (k == K_DOWN && s0 == 0) *sel = 1; else if (k == K_LEFT && s0 == 2) *sel = 1; else if (k == K_RIGHT && s0 == 1) *sel = 2;
        else if (k == K_A) go = s0; else if (k == K_B) { LN.page = 0; LN.sel[0] = 2; }
    }
    pthread_cond_broadcast(&S.cv); pthread_mutex_unlock(&S.mu);
    if (go >= 0) lnr_activate(page, go, 0);
}
static int lnr_hit(float x, float y, int *page) {   // a touch on the page shown (its layout: startscreen.c stv_page_hit)
    pthread_mutex_lock(&S.mu); ANativeWindow *w = S.win; *page = LN.page; pthread_mutex_unlock(&S.mu);
    if (!w) return -1;
    stv_view v; memset(&v, 0, sizeof v); v.screen = *page == 0 ? STV_SCREEN_LAUNCHER : *page == 1 ? STV_SCREEN_CLOUD : *page == 3 ? STV_SCREEN_LINK : STV_SCREEN_APP; v.cp.nrow = 4;
    if (*page == 1) { dm_cloud_info ci; dm_cloud_info_get(&ci); v.cp.link = cp_link_of(&ci); }
    if (*page == 3) { dm_cloud_link k; dm_cloud_link_get(&k); lk_fill(&v.lk, &k, "", 0); }
    return stv_page_hit(&v, ANativeWindow_getWidth(w), ANativeWindow_getHeight(w), x, y);
}
static void lnr_touch(int act, float x, float y) {
    int page, i = lnr_hit(x, y, &page);
    if (act == AMOTION_EVENT_ACTION_DOWN) {
        if (page == 1 && i == 4) { dm_cloud_info ci; dm_cloud_info_get(&ci);
            if (ci.can_restore) { pthread_mutex_lock(&S.mu); LN.sel[1] = 4; LN.holding = 1; LN.hold_t0 = now_ms(); LN.hold_touch = 1; pthread_cond_broadcast(&S.cv); pthread_mutex_unlock(&S.mu); } }
    } else if (act == AMOTION_EVENT_ACTION_MOVE) { if (!(page == 1 && i == 4)) { pthread_mutex_lock(&S.mu); if (LN.holding && LN.hold_touch) LN.holding = 0; pthread_mutex_unlock(&S.mu); } }
    else if (act == AMOTION_EVENT_ACTION_CANCEL) { pthread_mutex_lock(&S.mu); if (LN.hold_touch) LN.holding = 0; pthread_mutex_unlock(&S.mu); }
    else if (act == AMOTION_EVENT_ACTION_UP) {
        pthread_mutex_lock(&S.mu); int held = LN.holding && LN.hold_touch, want = LN.want; pthread_mutex_unlock(&S.mu);
        if (held) { lnr_hold_end(1); return; }
        if (i < 0 || (want && page == 0)) return;
        pthread_mutex_lock(&S.mu); LN.sel[page] = i; pthread_cond_broadcast(&S.cv); pthread_mutex_unlock(&S.mu);
        lnr_activate(page, i, 1);
    }
}
// the launcher is what the screen shows (not a download, an error of the data or a notice)
static int lnr_visible_locked(void) {
    if (!LN.on || V.phase == PH_NOTICE) return 0;
    if (V.phase == PH_DATA && (S.status >= 0 || V.need_net || ((S.pr.phase == DS_PH_DOWNLOAD || S.pr.phase == DS_PH_UNPACK) && S.pr.total))) return 0;
    return 1;
}
static int lnr_input(const AInputEvent *e) {   // 1 when it was the launcher's
    pthread_mutex_lock(&S.mu); int open = V.open && !V.closing, on = LN.on, vis = on && lnr_visible_locked(); pthread_mutex_unlock(&S.mu);
    if (!open) return 0;
    int type = AInputEvent_getType(e);
    if (type == AINPUT_EVENT_TYPE_KEY) {
        int k = AKeyEvent_getKeyCode(e), a = AKeyEvent_getAction(e);
        if (k == AKEYCODE_VOLUME_UP || k == AKEYCODE_VOLUME_DOWN || k == AKEYCODE_VOLUME_MUTE) return 0;
        if (!on) {   // start at once: − (Select) while the start runs opens the launcher
            if (k != AKEYCODE_BUTTON_SELECT || a != AKEY_EVENT_ACTION_DOWN) return 0;
            pthread_mutex_lock(&S.mu); LN.on = 1; LN.page = 0; LN.sel[0] = 3; pthread_cond_broadcast(&S.cv); pthread_mutex_unlock(&S.mu);
            LOG("launcher: − at the start: the launcher opens"); return 1;
        }
        if (!vis) return 0;
        dm_cloud_view cv; dm_cloud_view_get(&cv); if (cv.ask) return 0;   // a question of the cloud's: its input
        int isA = k == AKEYCODE_BUTTON_A || k == AKEYCODE_DPAD_CENTER || k == AKEYCODE_ENTER || k == AKEYCODE_NUMPAD_ENTER || k == AKEYCODE_BUTTON_START;
        if (a == AKEY_EVENT_ACTION_UP) { if (isA) lnr_hold_end(0); return 1; }
        if (a != AKEY_EVENT_ACTION_DOWN || AKeyEvent_getRepeatCount(e)) return 1;
        if (k == AKEYCODE_DPAD_UP) lnr_key(K_UP); else if (k == AKEYCODE_DPAD_DOWN) lnr_key(K_DOWN); else if (k == AKEYCODE_DPAD_LEFT) lnr_key(K_LEFT); else if (k == AKEYCODE_DPAD_RIGHT) lnr_key(K_RIGHT);
        else if (isA) lnr_key(K_A); else if (k == AKEYCODE_BUTTON_B || k == AKEYCODE_BACK || k == AKEYCODE_ESCAPE) lnr_key(K_B);
        return 1;
    }
    if (type != AINPUT_EVENT_TYPE_MOTION || !vis) return 0;
    dm_cloud_view cv; dm_cloud_view_get(&cv); if (cv.ask) return 0;
    int src = AInputEvent_getSource(e), act = AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_MASK;
    if ((src & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK) {   // the hat and the left stick: one step a push
        static int hx, hy; float x = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_X, 0), y = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_Y, 0);
        float sx = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_X, 0), sy = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_Y, 0);
        if (fabsf(x) < 0.5f && fabsf(y) < 0.5f) { if (fabsf(sx) > 0.7f) x = sx; if (fabsf(sy) > 0.7f) y = sy; }
        int dx = x < -0.5f ? -1 : x > 0.5f ? 1 : 0, dy = y < -0.5f ? -1 : y > 0.5f ? 1 : 0;
        if (dx && dx != hx) lnr_key(dx < 0 ? K_LEFT : K_RIGHT);
        if (dy && dy != hy) lnr_key(dy < 0 ? K_UP : K_DOWN);
        hx = dx; hy = dy; return 1;
    }
    lnr_touch(act, AMotionEvent_getX(e, 0), AMotionEvent_getY(e, 0));
    return 1;
}
