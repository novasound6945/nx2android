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
typedef struct { int phase, status, wait_s, wait_tot, need_net, pd, pt; ds_progress p; char msg[256], nt_ko[3][200], nt_en[3][200], nt_detail[300]; } snap_t;
// the picture's state, from what the phases left (under no lock: snap and cv are copies)
static void build_view(stv_view *v, const snap_t *x, const stv_rate *rate, dm_cloud_view *cv, dm_cloud_view *dq, const stv_step *steps) {
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
        snap_t x = {V.phase, S.status, S.wait_s, S.wait_tot, V.need_net, P.done, P.total}; x.p = S.pr; snprintf(x.msg, sizeof x.msg, "%s", S.msg);
        if (x.phase == PH_NOTICE) { memcpy(x.nt_ko, NT.ko, sizeof NT.ko); memcpy(x.nt_en, NT.en, sizeof NT.en); memcpy(x.nt_detail, NT.detail, sizeof NT.detail); }
        stv_step steps[STV_NSTEPS]; memcpy(steps, V.step, sizeof steps); ANativeWindow *w = S.win; int64_t t = now_ms();
        dm_cloud_view cv; memset(&cv, 0, sizeof cv); cv.progress = -1;
        if (x.phase == PH_CLOUD) dm_cloud_view_get(&cv);   // (savecloud.h: its lock is never held while this file's is taken)
        if (x.phase != last_phase) { stv_rate_reset(&rate); last_phase = x.phase; }
        if (x.phase == PH_DATA && (x.p.phase == DS_PH_DOWNLOAD || x.p.phase == DS_PH_UNPACK) && x.p.total)   // network bytes only: a resumed part's bytes join done at once, not the speed
            stv_rate_feed(&rate, t, x.p.phase == DS_PH_DOWNLOAD ? (double)x.p.net : (double)x.p.done, (double)(x.p.total - (x.p.done < x.p.total ? x.p.done : x.p.total)));
        if (x.phase == PH_PREP && x.pt) stv_rate_feed(&rate, t, x.pd, x.pt - x.pd);
        int urgent = x.phase == PH_NOTICE || (x.phase == PH_CLOUD && cv.urgent) || (x.phase == PH_DATA && x.status >= 0);
        int show = w && x.phase != PH_NONE && (V.shown || urgent || t - V.t0 > 300), animated = 0;
        if (show && !gl_tried) { gl_tried = 1; G.ok = !gl_load(); if (!G.ok) { LOG("[RED] start screen: no EGL / GLES 2: the start runs without its screen"); gl_end(); } }
        if (show && G.ok) {
            pthread_mutex_unlock(&S.mu);
            if (!R.tried) res_load();
            int ww = ANativeWindow_getWidth(w), wh = ANativeWindow_getHeight(w), cw = ww, ch = wh;
            if (cw > 2560) { cw /= 2; ch /= 2; }   // (a screen bigger than that: half its size, filtered up)
            if (cw > 0 && ch > 0 && (c.w != cw || c.h != ch)) { free(c.px); c.px = malloc((size_t)cw * ch * 4); c.w = c.px ? cw : 0; c.h = c.px ? ch : 0; }
            if (c.px) { stv_view v; dm_cloud_view dq; build_view(&v, &x, &rate, &cv, &dq, steps); stv_draw(&c, &v, t / 1000.0); animated = stv_animated(&v); gl_show(&c, w); }
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
    if (stat(p, &st)) { V.step[STV_CLOUD].state = STV_SKIP; snprintf(V.step[STV_CLOUD].note, sizeof V.step[STV_CLOUD].note, "%s", L("사용 안 함", "Off")); }
    checked_path(p, sizeof p); size_t n = 0; char *ck = p[0] ? ds_read(p, &n, 64) : NULL; V.checked[0] = 0;
    if (ck) { time_t t = (time_t)strtoll(ck, NULL, 10); struct tm tm; if (t > 0 && localtime_r(&t, &tm)) snprintf(V.checked, sizeof V.checked, korean() ? "%d월 %d일" : "%d/%d", tm.tm_mon + 1, tm.tm_mday); free(ck); }
    pthread_mutex_unlock(&S.mu);
    pthread_t t;
    if (pthread_create(&t, NULL, screen_thread, NULL)) { LOG("[RED] start screen: cannot start its thread: the start runs without it"); return; }
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
    if (!on) return 0;
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
    pthread_mutex_lock(&S.mu); V.need_net = 0; S.status = -1; V.phase = PH_NONE; V.unreached = unreached; V.step[STV_DATA].state = STV_DONE; snprintf(V.step[STV_DATA].note, sizeof V.step[STV_DATA].note, "%s", note);
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
