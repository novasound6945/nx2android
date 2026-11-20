// Android port: the app's data downloaded from a web server at launch (an APK ported with porter --data-url; datasync.h compares and downloads, this is its transport and
// its screen). The APK's asset nx2a/source.bin (porter/publish.py source_asset) holds the server's folder and the Basic authentication's user:password, masked only so the
// password does not show as plain text; neither the password nor the Authorization header is ever logged. Without the asset nothing here runs: the app starts as before.
// HTTP: java.net.HttpURLConnection through JNI (framework classes, no dex of the app's own): the system's DNS, proxy, TLS trust store (a server's Let's Encrypt certificate)
// and redirects, which a socket of our own would each have to redo; cleartext http:// only with mkapk.py --cleartext (Android 9+ refuses it otherwise).
// The screen: no game is running yet, so the window gets a small GLES 2 picture (libEGL / libGLESv2 opened here): EGL leaves the window when its surface is destroyed,
// and the game's Vulkan swapchain can take it after (a window drawn with ANativeWindow_lock stays the CPU's and Vulkan cannot connect to it).
// The front end (front_android.c) starts the game when the sync ends: done, nothing to do, or no server and a whole pack in the files folder (offline play)
// An APK without the asset runs the sync too when its files folder has a bundles folder (a pack whose small files porter install pushed in bundles: datasync.h), with
// no server: the bundles are unpacked, then the game starts
// The same screen shows the shader preparation (dm_prep_begin: gfx_vk_gpl.inc vk_gpl_prepare), after the sync or without one, before the game starts
// An asset of version 2 (porter port --data-pointer) names a pointer file as well: the sync thread reads it before files.json (pointer_resolve), without the Authorization
// header, and takes the server root and credentials it holds (+ the pack's folder under the root); without an answer, the pointer line it last read (kept in the app's
// internal storage, encrypted as it came); without that, the asset's own address. An asset of version 1 never goes that way
// The same screen shows the save's cloud sync at the start (savecloud_android.c dm_cloud_begin, after the shader preparation): draw_cloud draws its view (savecloud.h
// dm_cloud_view, which that file fills), so what the screen looks like is this file's alone; it shows only once a step takes 300 ms, or at once for a question
#include "dm.h"
#include "datasync.h"
#include "savecloud.h"
#include "pixel_font.h"
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
    ds_progress pr; int status, finished, wait_s; char msg[256];   // status: -1 working, else the last error (DS_ERR_*); wait_s: seconds to the next try
    void (*done)(void); int pipe[2];
} S = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, .status = -1, .pipe = {-1, -1}};
static struct { int on, done, total, gl_tried; } P;   // the shader preparation (dm_prep_begin), under S.mu: the screen shows it instead of the sync
static struct { int on, gl_tried; } CL;              // the cloud sync's start (dm_cloud_screen_begin), under S.mu: the screen shows its view
static struct { int on, gl_tried; char ko[3][200], en[3][200], detail[300]; } NT;   // a notice the start stops at (dm_notice_begin), under S.mu: shown until the app ends
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
// cond / got (datasync.h ds_fetch_fn): files.json asked for only if it changed since the copy the app has. HttpURLConnection passes the headers as they are and returns
// the server's 304 (setUseCaches(false), and the app installs no HttpResponseCache, that would answer it itself); a server that does not honour them answers 200 and
// sends the list whole, as before
static int http_fetch(void *u, const char *rel, uint64_t from, uint64_t *start, int64_t *total, int *http, const ds_val *cond, ds_val *got, ds_sink sink, void *su) {
    jhttp *h = u; JNIEnv *E = h->E; char url[2048], why[256] = "", rng[48]; static uint8_t chunk[256 << 10];
    if (got) memset(got, 0, sizeof *got);
    if (ds_url(url, sizeof url, S.base, rel)) { LOG("[RED] data: the address of %s is too long", rel); return DS_ERR_SERVER; }
    if ((*E)->PushLocalFrame(E, 48)) { jx(E, NULL, 0); return DS_ERR_NET; }
    int rc = DS_ERR_NET, probe = !strcmp(rel, DS_LIST);   // the list: short timeouts, so an app without the server starts the game soon (offline play)
    jobject U = (*E)->NewObject(E, h->cUrl, h->mUrl, (*E)->NewStringUTF(E, url)); jobject C = U && !jx(E, why, sizeof why) ? (*E)->CallObjectMethod(E, U, h->mOpen) : NULL;
    jobject in = NULL;
    if (!C || jx(E, why, sizeof why) || !(*E)->IsInstanceOf(E, C, h->cConn)) goto out;
    (*E)->CallVoidMethod(E, C, h->mConnTo, probe ? 4000 : 10000); if (jx(E, why, sizeof why)) goto out;
    (*E)->CallVoidMethod(E, C, h->mReadTo, probe ? 8000 : 30000); if (jx(E, why, sizeof why)) goto out;
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
    if ((*E)->PushLocalFrame(E, 32)) { jx(E, NULL, 0); snprintf(why, wcap, "no JNI frame"); return -1; }
    jstring su = (*E)->NewStringUTF(E, url); if (jx(E, why, wcap) || !su) goto out;
    U = (*E)->NewObject(E, h->cUrl, h->mUrl, su); if (jx(E, why, wcap) || !U) goto out;
    C = (*E)->CallObjectMethod(E, U, h->mOpen); if (jx(E, why, wcap) || !C || !(*E)->IsInstanceOf(E, C, h->cConn)) goto out;
    (*E)->CallVoidMethod(E, C, h->mConnTo, 4000); if (jx(E, why, wcap)) goto out;
    (*E)->CallVoidMethod(E, C, h->mReadTo, 8000); if (jx(E, why, wcap)) goto out;
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
// A pointer that cannot be read costs one short timeout (4 s to connect, 8 s to read), and the sync then goes on as before (no server: the game starts with what it has)
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

// ---- the screen: a canvas 640 wide, drawn by the CPU, shown as one texture through GLES 2 ----
typedef struct { uint32_t *px; int w, h; uint32_t ink; } canvas;
static void plot(void *u, int x, int y) { canvas *c = u; if (x >= 0 && y >= 0 && x < c->w && y < c->h) c->px[y * c->w + x] = c->ink; }
static void fill(canvas *c, int x, int y, int w, int h, uint32_t col) { for (int j = y < 0 ? 0 : y; j < y + h && j < c->h; j++) for (int i = x < 0 ? 0 : x; i < x + w && i < c->w; i++) c->px[j * c->w + i] = col; }
static void text(canvas *c, const char *s, int x, int y, int scale, uint32_t col) { c->ink = col; pf_draw(dm_overlay_font(), s, x, y, scale, plot, c); }
static void ctext(canvas *c, const char *s, int y, int scale, uint32_t col) { text(c, s, (c->w - pf_width(dm_overlay_font(), s) * scale) / 2, y, scale, col); }
static void ctext_fit(canvas *c, const char *s, int y, int scale, uint32_t col) {   // centred, at scale or smaller so it fits with 16 px at each side
    while (scale > 1 && pf_width(dm_overlay_font(), s) * scale > c->w - 32) scale--;
    ctext(c, s, y, scale, col);
}
#define RGB(r, g, b) (0xff000000u | (uint32_t)(b) << 16 | (uint32_t)(g) << 8 | (uint32_t)(r))   // RGBA bytes in memory
static int korean(void) { const pf_index *g = pf_find(dm_overlay_font(), 0xAC00); return g && (g->cp_adv >> 8) == 0xAC00; }   // the APK's font has Hangul (assets/font/ui.bin)
static void dur(char *o, size_t n, double s, int ko) {
    if (s < 0 || s > 359999) { snprintf(o, n, "-"); return; }
    int m = (int)(s / 60), x = (int)s % 60;
    if (ko) { if (m >= 60) snprintf(o, n, "%d시간 %d분", m / 60, m % 60); else if (m) snprintf(o, n, "%d분 %d초", m, x); else snprintf(o, n, "%d초", x); }
    else { if (m >= 60) snprintf(o, n, "%dh %dm", m / 60, m % 60); else if (m) snprintf(o, n, "%dm %ds", m, x); else snprintf(o, n, "%ds", x); }
}
static void draw(canvas *c, const ds_progress *p, int status, int wait_s, const char *msg, double speed) {
    // (30 px higher with the storage line below the retry: the block then ends 24 px lower, which a 640 x 360 canvas (a 16:9 screen) cut off)
    int ko = korean(), y = c->h / 2 - 70 - (status == DS_ERR_SPACE ? 30 : 0); char t[400], a[64];
    fill(c, 0, 0, c->w, c->h, RGB(18, 20, 26));
    ctext(c, ko ? "게임 데이터" : "Game data", y, 3, RGB(235, 238, 245)); y += 36;
    const char *ph = p->phase == DS_PH_CONNECT ? (ko ? "서버에 연결하는 중" : "Connecting to the server") : p->phase == DS_PH_CHECK ? (ko ? "가지고 있는 파일 확인 중" : "Checking the files on the device")
                   : p->phase == DS_PH_DOWNLOAD ? (ko ? "받는 중" : "Downloading") : p->phase == DS_PH_UNPACK ? (ko ? "묶음 파일을 푸는 중" : "Extracting the bundles")
                   : (ko ? "마무리하는 중" : "Finishing");
    ctext(c, ph, y, 2, RGB(170, 180, 200)); y += 26;
    int bw = c->w - 80, bx = 40; double f = p->total ? (double)p->done / (double)p->total : 0; if (f > 1) f = 1;
    fill(c, bx, y, bw, 12, RGB(50, 55, 68)); fill(c, bx, y, (int)(bw * f), 12, RGB(90, 160, 255)); y += 34;
    snprintf(t, sizeof t, "%.0f / %.0f MB  (%d%%)", p->done / 1048576.0, p->total / 1048576.0, (int)(f * 100)); ctext(c, t, y, 2, RGB(235, 238, 245)); y += 24;
    if (p->phase == DS_PH_DOWNLOAD && speed > 0) { dur(a, sizeof a, (double)(p->total - p->done) / speed, ko); snprintf(t, sizeof t, ko ? "%.1f MB/s, 남은 시간 %s" : "%.1f MB/s, %s left", speed / 1048576.0, a); ctext(c, t, y, 2, RGB(170, 180, 200)); }
    if (p->phase == DS_PH_UNPACK) { snprintf(t, sizeof t, ko ? "파일 %d / %d개" : "%d / %d files", p->file_no, p->files); ctext(c, t, y, 2, RGB(170, 180, 200)); }   // the bundle on the line below
    y += 24;
    if (p->file[0]) { if (p->phase == DS_PH_UNPACK) snprintf(t, sizeof t, "%.60s", p->file); else snprintf(t, sizeof t, "%d/%d  %.60s", p->file_no, p->files, p->file); ctext(c, t, y, 1, RGB(120, 130, 150)); }
    y += 30;
    if (status >= 0) {
        const char *w = status == DS_ERR_NET ? (ko ? "서버에 연결할 수 없습니다 (네트워크를 확인해 주세요)" : "The server cannot be reached (check the network)")
                      : status == DS_ERR_LOCAL ? (ko ? "기기에 있는 게임 데이터가 모자랍니다. 다시 설치해 주세요" : "The game data on the device is incomplete: install it again")
                      : status == DS_ERR_SPACE ? (ko ? "저장 공간이 부족합니다" : "Not enough storage space")
                      : status == DS_ERR_PACKAGE ? (ko ? "서버의 데이터가 이 앱의 것이 아닙니다" : "The server's data is not this app's")
                      : status == DS_ERR_DATA ? (!S.base[0] ? (ko ? "기기에 있는 묶음 파일이 손상되었습니다. 다시 설치해 주세요" : "A bundle on the device is damaged: install the pack again")
                                                      : ko ? "받은 파일이 손상되었습니다. 다시 받습니다" : "A downloaded file was damaged; it is downloaded again")
                      : status == DS_ERR_DISK ? (ko ? "파일을 쓸 수 없습니다" : "A file cannot be written") : (ko ? "서버 오류" : "Server error");
        ctext(c, w, y, 2, RGB(255, 150, 120)); y += 22;
        if (msg[0]) { ctext(c, msg, y, 1, RGB(200, 140, 120)); y += 16; }
        snprintf(t, sizeof t, ko ? "%d초 후 다시 시도합니다" : "Trying again in %d s", wait_s); ctext(c, t, y + 6, 2, RGB(170, 180, 200));
        if (status == DS_ERR_SPACE) ctext_fit(c, ko ? "공간을 비우면 자동으로 이어서 받습니다" : "Free some space: the download resumes by itself", y + 36, 2, RGB(170, 180, 200));   // (each try checks the space again)
    }
}
static void draw_prep(canvas *c, int done, int total, double el) {   // el: seconds since the parts began
    int ko = korean(), y = c->h / 2 - 84; char t[200], a[64], b[64];
    fill(c, 0, 0, c->w, c->h, RGB(18, 20, 26));
    ctext(c, ko ? "셰이더 준비" : "Preparing shaders", y, 3, RGB(235, 238, 245)); y += 36;
    // two lines, each made smaller when it would not fit the canvas (one line was cut at both sides of the 640 wide canvas: Retroid Pocket 6)
    ctext_fit(c, ko ? "처음 한 번만 합니다" : "Done once, at the first start", y, 2, RGB(170, 180, 200)); y += 30;   // the font is 12 px: 24 at scale 2 (22 overlapped the next line)
    ctext_fit(c, ko ? "(게임 데이터나 드라이버가 바뀌면 다시 합니다)" : "(and again after a data or driver update)", y, 2, RGB(170, 180, 200)); y += 34;
    int bw = c->w - 80, bx = 40; double f = total ? (double)done / total : 0; if (f > 1) f = 1;
    fill(c, bx, y, bw, 12, RGB(50, 55, 68)); fill(c, bx, y, (int)(bw * f), 12, RGB(90, 160, 255)); y += 34;
    snprintf(t, sizeof t, "%d / %d  (%d%%)", done, total, (int)(f * 100)); ctext(c, t, y, 2, RGB(235, 238, 245)); y += 24;
    dur(a, sizeof a, el, ko); if (done > 16 && done < total) dur(b, sizeof b, el * (total - done) / done, ko); else snprintf(b, sizeof b, "-");
    snprintf(t, sizeof t, ko ? "%s 지남, 남은 시간 약 %s" : "%s so far, about %s left", a, b); ctext(c, t, y, 2, RGB(170, 180, 200));
}
// The cloud sync's view (savecloud.h dm_cloud_view): a title, small lines, two cards side by side or a row of buttons, a bar, a hint at the bottom. cl_layout places the cards
// and buttons, for the drawing and for a touch's hit (dm_cloud_screen_hit) alike
static void draw_notice(canvas *c) {   // the title, two lines in the app's language, the detail (the log's words) small below
    int ko = korean(), y = c->h / 2 - 66;
    fill(c, 0, 0, c->w, c->h, RGB(18, 20, 26));
    ctext_fit(c, ko ? NT.ko[0] : NT.en[0], y, 3, RGB(255, 150, 120)); y += 44;
    for (int i = 1; i < 3; i++) { ctext_fit(c, ko ? NT.ko[i] : NT.en[i], y, 2, i == 1 ? RGB(235, 238, 245) : RGB(170, 180, 200)); y += 28; }
    if (NT.detail[0]) ctext_fit(c, NT.detail, y + 8, 1, RGB(120, 130, 150));
}
static int cl_canvas_h(int ww, int wh) { int ch = ww > 0 && wh > 0 ? 640 * wh / ww : 360; return ch < 240 ? 240 : ch; }
static int cl_layout(int cw, int ch, const dm_cloud_view *v, int r[3][4]) {   // the rectangles (x, y, w, h) of the cards, else of the buttons; their count
    if (v->ncards) { int w = 250, h = 112, gap = 24, x0 = (cw - 2 * w - gap) / 2, y = ch / 2 - 64;
        for (int i = 0; i < 2; i++) { r[i][0] = x0 + i * (w + gap); r[i][1] = y; r[i][2] = w; r[i][3] = h; } return 2; }
    int n = v->nbtn, gap = 16, h = 30, w[3], tot = 0;
    for (int i = 0; i < n; i++) { w[i] = pf_width(dm_overlay_font(), v->btn[i]) + 40; if (w[i] < 150) w[i] = 150; tot += w[i] + (i ? gap : 0); }
    for (int i = 0, x = (cw - tot) / 2; i < n; x += w[i] + gap, i++) { r[i][0] = x; r[i][1] = ch - 92; r[i][2] = w[i]; r[i][3] = h; }
    return n;
}
static void rect_line(canvas *c, int x, int y, int w, int h, int t, uint32_t col) { fill(c, x, y, w, t, col); fill(c, x, y + h - t, w, t, col); fill(c, x, y, t, h, col); fill(c, x + w - t, y, t, h, col); }
static void draw_cloud(canvas *c, const dm_cloud_view *v) {
    const uint32_t ink = RGB(235, 238, 245), dim = RGB(150, 160, 180), acc = RGB(90, 160, 255), box = RGB(34, 38, 48);
    fill(c, 0, 0, c->w, c->h, RGB(18, 20, 26));
    int y = v->ncards ? c->h / 2 - 104 : c->h / 2 - 70;
    ctext_fit(c, v->title, y, 2, ink); y += 26;
    for (int i = 0; i < 3; i++) if (v->line[i][0]) { ctext_fit(c, v->line[i], y, 1, i == 2 ? dim : RGB(200, 206, 220)); y += 16; }
    if (v->progress >= 0) { int bw = c->w - 160, bx = 80, f = v->progress > 1000 ? 1000 : v->progress; y += 6;
        fill(c, bx, y, bw, 8, RGB(50, 55, 68)); fill(c, bx, y, bw * f / 1000, 8, acc);
        char t[16]; snprintf(t, sizeof t, "%d%%", f / 10); ctext(c, t, y + 24, 1, dim); }
    int r[3][4], n = cl_layout(c->w, c->h, v, r);
    for (int i = 0; i < n; i++) {
        int x = r[i][0], yy = r[i][1], w = r[i][2], h = r[i][3], sel = i == v->sel;
        fill(c, x, yy, w, h, sel ? RGB(40, 58, 90) : box);
        if (sel) rect_line(c, x, yy, w, h, 2, acc);
        if (v->ncards) {
            const dm_cloud_card *k = &v->card[i]; int cx = x + 12;
            text(c, k->head, cx, yy + 18, 1, sel ? ink : RGB(200, 206, 220));
            if (k->newer) { const char *b = korean() ? "더 최근" : "newer"; int bw = pf_width(dm_overlay_font(), b) + 10; fill(c, x + w - bw - 8, yy + 6, bw, 16, acc); text(c, b, x + w - bw - 3, yy + 18, 1, ink); }
            text(c, k->who, cx, yy + 36, 1, dim);
            text(c, k->ago, cx, yy + 72, 2, ink);
            text(c, k->date, cx, yy + 96, 1, RGB(200, 206, 220));
        } else { int tw = pf_width(dm_overlay_font(), v->btn[i]); text(c, v->btn[i], x + (w - tw) / 2, yy + h / 2 + 5, 1, ink); }
    }
    if (v->foot[0]) ctext_fit(c, v->foot, c->h - 28, 1, dim);
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
    EGLDisplay dpy; EGLConfig cfg; EGLContext ctx; EGLSurface surf; ANativeWindow *on; GLuint prog, tex; int ok;
} G;
static int gl_load(void) {
    void *e = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL), *g = dlopen("libGLESv2.so", RTLD_NOW | RTLD_LOCAL); if (!e || !g) return -1;
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
static void gl_show(canvas *c, ANativeWindow *w) {
    if (gl_bind(w)) return;
    static const float quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    G.Viewport(0, 0, ANativeWindow_getWidth(w), ANativeWindow_getHeight(w)); G.UseProgram(G.prog); G.BindTexture(GL_TEXTURE_2D, G.tex);
    G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    G.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, c->w, c->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, c->px);
    G.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad); G.EnableVertexAttribArray(0); G.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    G.SwapBuffers(G.dpy, G.surf);
}
static void *screen_thread(void *a) {
    (void)a; pthread_setname_np(pthread_self(), "datascreen");
    int64_t t0 = now_ms(), last_t = 0; uint64_t last_done = 0; double speed = 0; canvas c = {0};
    pthread_mutex_lock(&S.mu);
    for (;;) {
        if (S.lost) { gl_drop(); S.lost = 0; S.released = 1; pthread_cond_broadcast(&S.cv); }
        if (S.finished) break;
        ds_progress p = S.pr; int st = S.status, ws = S.wait_s; char msg[256]; snprintf(msg, sizeof msg, "%s", S.msg); ANativeWindow *w = S.win;
        int64_t t = now_ms();
        if (p.phase == DS_PH_DOWNLOAD) {   // MB/s over the last seconds
            if (last_t && t > last_t && p.net >= last_done) { double v = (double)(p.net - last_done) * 1000.0 / (double)(t - last_t); speed = speed ? speed * 0.85 + v * 0.15 : v; }
            last_t = t; last_done = p.net;   // network bytes only: a resumed part's bytes join done at once, not the speed
        }
        // drawn only once the sync takes a moment: a launch with the data up to date shows no screen at all; the preparation once it makes parts (EGL opened then)
        int pd = P.done, pt = P.total; if (P.on && pt && !P.gl_tried) { P.gl_tried = 1; G.ok = !gl_load(); if (!G.ok) LOG("[RED] shaders: no EGL / GLES 2: the preparation runs without its screen"); t0 = t; }
        if (NT.on && !NT.gl_tried) { NT.gl_tried = 1; G.ok = !gl_load(); if (!G.ok) LOG("[RED] notice: no EGL / GLES 2: the notice is in the log only"); }
        dm_cloud_view cv; int cl_show = 0;
        if (CL.on) { dm_cloud_view_get(&cv); cl_show = cv.urgent || t - t0 > 300;   // (savecloud.h: its lock is never held while this file's is taken)
            if (cl_show && !CL.gl_tried) { CL.gl_tried = 1; G.ok = !gl_load(); if (!G.ok) LOG("[RED] cloud: no EGL / GLES 2: the sync's start runs without its screen"); } }
        if (w && G.ok && (NT.on || (CL.on ? cl_show : P.on ? pt > 0 : t - t0 > 400))) {
            pthread_mutex_unlock(&S.mu);
            int ww = ANativeWindow_getWidth(w), wh = ANativeWindow_getHeight(w), cw = 640, ch = ww > 0 && wh > 0 ? 640 * wh / ww : 360;
            if (ch < 240) ch = 240;
            if (c.w != cw || c.h != ch) { free(c.px); c.px = malloc((size_t)cw * ch * 4); c.w = cw; c.h = ch; }
            if (c.px) { if (NT.on) draw_notice(&c); else if (CL.on) draw_cloud(&c, &cv); else if (P.on) draw_prep(&c, pd, pt, (t - t0) / 1000.0); else draw(&c, &p, st, ws, msg, speed); gl_show(&c, w); }
            pthread_mutex_lock(&S.mu);
        }
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += 200 * 1000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&S.cv, &S.mu, &ts);
    }
    pthread_mutex_unlock(&S.mu);
    gl_drop(); if (G.ok) { G.DestroyContext(G.dpy, G.ctx); G.Terminate(G.dpy); G.ok = 0; }
    free(c.px);
    char b = 1; if (write(S.pipe[1], &b, 1) != 1) LOG("[RED] data: cannot wake the main thread");   // the game starts on the main thread (sync_ready)
    return NULL;
}
void dm_sync_window(ANativeWindow *w) {   // on_window_created (w) and on_window_destroyed (NULL): a destroyed window is let go before the callback returns
    if (!S.act) return;
    pthread_mutex_lock(&S.mu);
    if (w) S.win = w;
    else if (S.win) {
        S.win = NULL;
        if (G.on && !S.finished) { S.lost = 1; S.released = 0; pthread_cond_broadcast(&S.cv);
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 2;
            while (!S.released && !S.finished) if (pthread_cond_timedwait(&S.cv, &S.mu, &ts)) break; }
    }
    pthread_mutex_unlock(&S.mu);
}

// ---- the sync thread ----
static void *sync_thread(void *a) {
    (void)a; pthread_setname_np(pthread_self(), "datasync");
    JavaVM *vm = S.act->vm; jhttp h = {0}; int rc = DS_OFFLINE, net = S.base[0] != 0;   // !net: no server, only the folder's bundles to unpack (dm_sync_begin)
    if (net && ((*vm)->AttachCurrentThread(vm, &h.E, NULL) != JNI_OK || jhttp_init(&h))) LOG("[RED] data: no Java for the download: the game starts with the data the app has");
    else {
        ds_ctx c = {.root = S.root, .package = S.package, .fetch = net ? http_fetch : NULL, .fu = &h, .free_bytes = free_bytes, .note = sync_note, .log = sync_log};
        int delay = 2;
        for (int round = 1;; round++) {
            pthread_mutex_lock(&S.mu); S.status = -1; S.msg[0] = 0; pthread_mutex_unlock(&S.mu);
            if (net && PT.on && !PT.fresh) pointer_resolve(&h);   // (again at each try until the pointer itself answers: a launch before the network is up reaches it later)
            rc = ds_sync(&c);
            if (rc == DS_DONE || rc == DS_UPTODATE || rc == DS_OFFLINE) break;
            if (c.pr.phase == DS_PH_DOWNLOAD && c.pr.done) delay = 2;   // it moved on: the next break waits from the start again
            LOG("[RED] data: try %d failed (%d): %s; again in %d s", round, rc, c.err, delay);
            char msg[256]; snprintf(msg, sizeof msg, "%s", c.err);   // (the log keeps c.err in English; the screen says how much space in the app's language)
            if (rc == DS_ERR_SPACE) {   // (under 0.1 GB short: MB, else it would say 0.0 GB more)
                double more = c.space_need / 1073741824.0, fr = c.space_free / 1073741824.0;
                if (more < 0.1) { if (korean()) snprintf(msg, sizeof msg, "%.0fMB가 더 필요합니다 (남은 공간 %.1fGB)", c.space_need / 1048576.0, fr); else snprintf(msg, sizeof msg, "%.0f MB more space is needed; the device has %.1f GB free", c.space_need / 1048576.0, fr); }
                else if (korean()) snprintf(msg, sizeof msg, "%.1fGB가 더 필요합니다 (남은 공간 %.1fGB)", more, fr);
            }
            for (int s = delay; s > 0; s--) { pthread_mutex_lock(&S.mu); S.status = rc; S.wait_s = s; snprintf(S.msg, sizeof S.msg, "%s", msg); pthread_cond_broadcast(&S.cv); pthread_mutex_unlock(&S.mu); sleep(1); }
            delay = rc == DS_ERR_PACKAGE || rc == DS_ERR_SPACE || rc == DS_ERR_LOCAL ? 30 : delay * 2 > 60 ? 60 : delay * 2;   // (DS_ERR_LOCAL: until an install pushes what is missing)
        }
        LOG("%s", rc == DS_DONE ? c.pr.net ? "data: downloaded, the files folder holds the server's pack"
            : "data: unpacked the bundles in the folder, the files folder holds the server's pack" : rc == DS_UPTODATE ? "data: the files folder holds the server's pack already"
            : "data: no server, the files folder holds a whole pack: the game starts with it");
        if (net) {
            if (h.buf) (*h.E)->DeleteGlobalRef(h.E, h.buf);
            (*vm)->DetachCurrentThread(vm);
        }
    }
    pthread_mutex_lock(&S.mu); S.finished = rc ? rc : 1; pthread_cond_broadcast(&S.cv); pthread_mutex_unlock(&S.mu);
    return NULL;
}
static int sync_ready(int fd, int events, void *data) {   // the main thread's looper: the screen let the window go, the game starts
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
    S.done = done;
    ALooper *lp = ALooper_forThread();
    if (!lp || pipe(S.pipe) || ALooper_addFd(lp, S.pipe[0], ALOOPER_POLL_CALLBACK, ALOOPER_EVENT_INPUT, sync_ready, NULL) != 1) { LOG("[RED] data: no looper on the main thread: the game starts with the data the app has"); return 0; }
    if (S.base[0] && PT.on) LOG("data: the data server is read from the pointer %s first (else the last one read, else %s)", PT.pointer, S.base);
    else if (S.base[0]) LOG("data: the app's data comes from %s%s", S.base, S.auth[0] ? " (Basic authentication)" : "");
    else LOG("data: no data server; the bundles in the files folder are unpacked before the game starts");
    G.ok = !gl_load(); if (!G.ok) LOG("[RED] data: no EGL / GLES 2: the download runs without its screen");
    pthread_mutex_lock(&S.mu); S.act = act; pthread_mutex_unlock(&S.mu);
    pthread_t t;
    if (pthread_create(&t, NULL, screen_thread, NULL)) { LOG("[RED] data: cannot start the screen thread"); return 0; }
    pthread_detach(t);
    if (pthread_create(&t, NULL, sync_thread, NULL)) { LOG("[RED] data: cannot start the download thread"); pthread_mutex_lock(&S.mu); S.finished = 1; pthread_cond_broadcast(&S.cv); pthread_mutex_unlock(&S.mu); return 1; }
    pthread_detach(t);
    return 1;
}

// ---- the shader preparation: gfx_vk_gpl.inc vk_gpl_prepare on a thread of its own, this screen while it makes parts (none when there is nothing to make) ----
__attribute__((weak)) int vk_gpl_prepare(void (*note)(void *u, int done, int total), void *u);   // (a build without the Vulkan backend has none)
static void prep_note(void *u, int done, int total) { (void)u; pthread_mutex_lock(&S.mu); P.done = done; P.total = total; pthread_cond_broadcast(&S.cv); pthread_mutex_unlock(&S.mu); }
static void *prep_thread(void *a) {
    (void)a; pthread_setname_np(pthread_self(), "shaderprep");
    int r = vk_gpl_prepare(prep_note, NULL);
    pthread_mutex_lock(&S.mu); S.finished = 1; pthread_cond_broadcast(&S.cv); pthread_mutex_unlock(&S.mu);
    LOG("shaders: %s", r ? "prepared: the game starts" : "nothing to prepare: the game starts");
    return NULL;
}
// 1: the preparation runs and done() is called on the main thread once the game may start; 0: start the game now. w: the window if there is one already
int dm_prep_begin(ANativeActivity *act, ANativeWindow *w, void (*done)(void)) {
    if (!vk_gpl_prepare) return 0;
    ALooper *lp = ALooper_forThread();
    int np[2];   // a pipe of its own: called from the sync's sync_ready, whose fd the looper removes when it returns
    if (!lp || pipe(np) || ALooper_addFd(lp, np[0], ALOOPER_POLL_CALLBACK, ALOOPER_EVENT_INPUT, sync_ready, NULL) != 1) { LOG("[RED] shaders: no looper on the main thread: the game starts without the preparation"); return 0; }
    pthread_mutex_lock(&S.mu);
    S.pipe[0] = np[0]; S.pipe[1] = np[1]; S.act = act; if (w && !S.win) S.win = w; S.done = done; S.finished = 0; S.lost = 0; S.released = 0; P.on = 1; P.done = P.total = P.gl_tried = 0;
    G.ok = 0; G.prog = 0; G.tex = 0; G.surf = NULL; G.on = NULL;   // (the sync's context is gone with its screen)
    pthread_mutex_unlock(&S.mu);
    pthread_t t;
    if (pthread_create(&t, NULL, screen_thread, NULL)) { LOG("[RED] shaders: cannot start the screen thread: the game starts without the preparation"); ALooper_removeFd(lp, S.pipe[0]); return 0; }
    pthread_detach(t);
    if (pthread_create(&t, NULL, prep_thread, NULL)) { LOG("[RED] shaders: cannot start the preparation thread"); pthread_mutex_lock(&S.mu); S.finished = 1; pthread_cond_broadcast(&S.cv); pthread_mutex_unlock(&S.mu); return 1; }
    pthread_detach(t);
    return 1;
}

// ---- the save's cloud sync at the start (savecloud_android.c): this screen while it runs; done() on the main thread once dm_cloud_screen_end ends it ----
int dm_cloud_screen_begin(ANativeActivity *act, ANativeWindow *w, void (*done)(void)) {
    ALooper *lp = ALooper_forThread();
    int np[2];   // a pipe of its own, as dm_prep_begin's (it may be called from a looper callback whose fd is removed when it returns)
    if (!lp || pipe(np) || ALooper_addFd(lp, np[0], ALOOPER_POLL_CALLBACK, ALOOPER_EVENT_INPUT, sync_ready, NULL) != 1) { LOG("[RED] cloud: no looper on the main thread"); return 0; }
    pthread_mutex_lock(&S.mu);
    S.pipe[0] = np[0]; S.pipe[1] = np[1]; S.act = act; if (w && !S.win) S.win = w; S.done = done; S.finished = 0; S.lost = 0; S.released = 0;
    P.on = 0; CL.on = 1; CL.gl_tried = 0;
    G.ok = 0; G.prog = 0; G.tex = 0; G.surf = NULL; G.on = NULL;   // (an earlier screen's context is gone with it)
    pthread_mutex_unlock(&S.mu);
    pthread_t t;
    if (pthread_create(&t, NULL, screen_thread, NULL)) { LOG("[RED] cloud: cannot start the screen thread"); ALooper_removeFd(lp, np[0]); close(np[0]); close(np[1]); CL.on = 0; return 0; }
    pthread_detach(t);
    return 1;
}
// ---- a notice the start stops at (front_android.c: data rewritten for another host contract): this screen until the player leaves the app; the game is not started ----
// ko, en: the title and two lines in each language; detail: the log's line, small. 0 when the screen cannot start (the caller ends the app)
int dm_notice_begin(ANativeActivity *act, ANativeWindow *w, const char *const ko[3], const char *const en[3], const char *detail) {
    pthread_mutex_lock(&S.mu);
    S.act = act; if (w && !S.win) S.win = w; S.done = NULL; S.finished = 0; S.lost = 0; S.released = 0; P.on = 0; CL.on = 0;
    NT.on = 1; NT.gl_tried = 0;
    for (int i = 0; i < 3; i++) { snprintf(NT.ko[i], sizeof NT.ko[i], "%s", ko[i]); snprintf(NT.en[i], sizeof NT.en[i], "%s", en[i]); }
    snprintf(NT.detail, sizeof NT.detail, "%s", detail ? detail : "");
    G.ok = 0; G.prog = 0; G.tex = 0; G.surf = NULL; G.on = NULL;   // (an earlier screen's context is gone with it)
    pthread_mutex_unlock(&S.mu);
    pthread_t t;
    if (pthread_create(&t, NULL, screen_thread, NULL)) { LOG("[RED] notice: cannot start the screen thread"); NT.on = 0; return 0; }
    pthread_detach(t);
    return 1;
}
void dm_cloud_screen_end(void) { pthread_mutex_lock(&S.mu); if (CL.on) { S.finished = 1; pthread_cond_broadcast(&S.cv); } pthread_mutex_unlock(&S.mu); }
void dm_cloud_screen_poke(void) { pthread_mutex_lock(&S.mu); pthread_cond_broadcast(&S.cv); pthread_mutex_unlock(&S.mu); }
int dm_cloud_screen_hit(float x, float y, int *kind) {
    pthread_mutex_lock(&S.mu); ANativeWindow *w = S.win; int ww = w ? ANativeWindow_getWidth(w) : 0, wh = w ? ANativeWindow_getHeight(w) : 0; pthread_mutex_unlock(&S.mu);
    if (ww <= 0 || wh <= 0) return -1;
    dm_cloud_view v; dm_cloud_view_get(&v); int ch = cl_canvas_h(ww, wh), r[3][4], n = cl_layout(640, ch, &v, r);
    float cx = x * 640.0f / ww, cy = y * (float)ch / wh;
    for (int i = 0; i < n; i++) if (cx >= r[i][0] && cx < r[i][0] + r[i][2] && cy >= r[i][1] && cy < r[i][1] + r[i][3]) { *kind = v.ncards ? 1 : 2; return i; }
    return -1;
}
