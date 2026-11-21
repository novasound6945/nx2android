// The save's cloud sync on Android (savecloud_android.c), as the front end (front_android.c), the start screen (datasync_android.c) and the overlay (gfx_vk_present.c) see it
#pragma once
#include <stdint.h>
#ifdef __ANDROID__
#include <android/input.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <jni.h>
#endif

// ---- the start screen: what it shows (savecloud_android.c fills it, datasync_android.c and startscreen.c draw it: the drawing is one function of this state, so the screen's
// design can change without the sync's logic) ----
enum { DM_CLOUD_ICON_NONE, DM_CLOUD_ICON_OFFLINE, DM_CLOUD_ICON_WARN };
// a question's layout: ASK an icon, a title, a line or two, a row of buttons or the two cards (Conflict / Offline / OtherDevice.dc.html); LIST a title and a line, a card of
// rows, a column of buttons, the first the accent's (AfterTest.dc.html); PAIR the same with two buttons side by side, A's and B's (ManualPending.dc.html)
enum { DM_CLOUD_STYLE_ASK, DM_CLOUD_STYLE_LIST, DM_CLOUD_STYLE_PAIR };
typedef struct { char head[96], ago[32], date[48], size[64]; int newer; } dm_cloud_card;   // head: "This device · rp6"; size: "1.2 MB · 9 files" ("" not known)
typedef struct {
    unsigned ver;          // changes with every update
    int urgent;            // a question: drawn at once (a step that only waits is drawn once the start took 300 ms, so a quick start shows no screen at all)
    int ask;               // a question (its own screen); 0: the cloud's row in the start's steps (note, progress, hint)
    int icon;              // DM_CLOUD_ICON_*
    char title[96];
    char line[2][200];     // the question's body (one line: a subtitle; two: a paragraph)
    char note[64];         // the row's state ("Syncing…") while ask is 0
    char hint[160];        // a line under the steps while ask is 0 (another device runs the game)
    int progress;          // -1 none, else 0..1000: the cloud save's download, as the steps' progress block
    int ncards;            // 2: two cards side by side (the conflict's choice), chosen by sel
    dm_cloud_card card[2];
    char badge[24];        // the newer card's badge ("More recent")
    int nbtn; char btn[3][64];   // buttons under it, chosen by sel (when there are no cards); while ask is 0, btn[0] is the bottom line's (B)
    int sel;
    char count[64]; int count_permille;   // the countdown to Play anyway ("Starting by itself in 7 s") and the time gone of it; "" none
    char foot[160], foot2[160];           // small lines at the bottom (one: the hint; two: a note over the keys' hint). LIST, PAIR: foot under the buttons, foot2 the keys
    int style;                            // DM_CLOUD_STYLE_*
    int nrow; char row_k[2][48], row_v[2][96];   // LIST, PAIR: the card of rows (a name at the left, its value at the right)
    char bnote[3][64];                    // LIST: the words at a button's right
} dm_cloud_view;
void dm_cloud_view_get(dm_cloud_view *v);   // savecloud_android.c: a copy of the current view (ask 0 once no check runs)

// ---- the launcher's Cloud save page and its line (datasync_android.c draws them from this; savecloud_android.c fills it) ----
enum { DM_CLOUD_ACT_NONE, DM_CLOUD_ACT_UPLOAD, DM_CLOUD_ACT_RESTORE, DM_CLOUD_ACT_CHECK };   // what the launcher asked for (Upload now, Back to the cloud save, a check again)
enum { DM_CLOUD_LOCAL_UNKNOWN, DM_CLOUD_LOCAL_SAME, DM_CLOUD_LOCAL_OWED, DM_CLOUD_LOCAL_ONLY, DM_CLOUD_LOCAL_PLAYED, DM_CLOUD_LOCAL_CONFLICT };   // (savesync.h SS_LOCAL_*)
typedef struct {
    int avail;            // sync.txt is there and the start reached the cloud's part (else nothing below is known)
    int trial;            // a trial build: this device only, not to be changed
    int mode;             // savesync.h SS_MODE_* (0 automatic, 1 manual, 2 this device only)
    int checking;         // a check runs (the start's, or one the launcher asked for)
    int act;              // DM_CLOUD_ACT_* running
    int progress;         // -1, else 0..1000: the cloud save's download
    int online;           // this run's check reached the server
    int offline;          // the check found no network or no answer
    int skipped;          // this run plays without the sync
    int auth;             // the server refused the token
    int local;            // DM_CLOUD_LOCAL_*
    int can_upload, can_restore;
    char cloud_dev[64]; int64_t cloud_time;   // the cloud's save: the device it came from and when (time 0: none known)
    int64_t checked;      // when the server last answered (0: never)
    uint64_t size; int64_t files;   // this device's save as its archive (files -1: not known)
    char device[64];      // this device's name (the server's, as sync.txt device= follows it)
    char result[96]; int result_bad;   // the last action's outcome ("" none)
    int linkable;         // no sync.txt, and the device link can run (not a trial build, the app known, no check running): the page offers Link this device
    int can_unlink;       // sync.txt is there and nothing runs: the page offers Unlink this app
    int noserver;         // no sync.txt, and no cloud save server to link with (no assets/nx2a/savecloud.txt, no DM_SAVESYNC_URL): the cloud save is off, the page says so
} dm_cloud_info;
void dm_cloud_info_get(dm_cloud_info *o);

// ---- the device link (savecloud_android.c: rooms' link/start and link/poll; the launcher's link page draws it from this) ----
enum { DM_LINK_NONE, DM_LINK_BUSY, DM_LINK_CODE, DM_LINK_EXPIRED, DM_LINK_FAIL, DM_LINK_DONE };   // no page; a code asked for; shown; run out; none came; linked
typedef struct { int state, left_s, limit; char code[16], qr[400], host[160], device[64]; } dm_cloud_link;   // left_s: the code's seconds left; limit: FAIL from a 429
void dm_cloud_link_get(dm_cloud_link *o);

#ifdef __ANDROID__
int dm_cloud_no_server(ANativeActivity *act);   // 1: no cloud save server but a sync.txt's (savesync.h ss_link_server: env.txt, the APK's assets/nx2a/savecloud.txt)
// datasync_android.c: the screen (its EGL window picture), shown while the cloud's start runs: 1 when it runs and done() is called on the main thread once it ended
// (dm_cloud_screen_end); 0: no screen (no looper or thread), the caller goes on at once
int dm_cloud_screen_begin(ANativeActivity *act, ANativeWindow *w, void (*done)(void));
void dm_start_step(int step, int state, const char *note);   // datasync_android.c: a step of the start screen's list (startscreen.h STV_*)
void dm_start_screen_open(ANativeActivity *act, const char *root);   // datasync_android.c: the start screen, from onCreate (it shows once the start took 300 ms)
void dm_start_screen_close(void);                       // before the game takes the window: its thread ended, EGL gone, its fonts, glyphs, icon and canvas freed
int dm_start_input(const AInputEvent *e);               // the start's input that is not the cloud's (the first start's Retry without a network): 1 when it was the screen's
int dm_data_unreached(void);                           // datasync_android.c: 1 when this start's data step could not reach its server (savesync.h ss_start_offline)
int dm_net_state(JNIEnv *E, jobject ctx);               // 1: the device has a connected network, 0: none, -1: not known (ConnectivityManager, ctx the activity)
void dm_cloud_screen_end(void);
void dm_cloud_screen_poke(void);                        // the view changed: drawn again now
int dm_cloud_screen_hit(float x, float y, int *kind);   // a touch at window pixels: the card (kind 1) or button (kind 2) under it, -1 none

// savecloud_android.c, from the front end (front_android.c)
// game_start, after the save manager's requests and before the host mounts the save: 1 when the cloud's start runs (its screen; done() is called on the main thread once
// the game may start, *quit set when the player chose to end the app instead), 0 when there is none (no sync.txt, or a trial build): the game starts at once
int dm_cloud_begin(ANativeActivity *act, ANativeWindow *w, void (*done)(int quit));
int dm_cloud_input(const AInputEvent *e);   // an input event before the game (the start screen's): 1 when it was the screen's
void dm_cloud_pause(void);    // onPause: the pending upload at once (under a wake lock), then the session closed
void dm_cloud_resume(void);   // onResume: a pending upload tried again now, the session open again
int dm_cloud_flush(int ms);   // the menu's Exit game: what onPause starts (the owed upload, the session closed), done now and waited for at most ms; 1: not done in time
void dm_cloud_game_start(void);   // game_run: the upload thread starts (when this run syncs)
// the launcher (datasync_android.c), on the main thread or the screen's: 0 when done (or started), -1 when it cannot be now
int dm_cloud_set_mode(int mode);       // this game's SS_MODE_*; back from this device only: a check runs again at once (a first connection's)
int dm_cloud_act(int what);            // DM_CLOUD_ACT_*, on a thread of its own (dm_cloud_info_get: act, result)
int dm_cloud_play_unsynced(void);      // Play without the sync: the check that runs ends without it, one still to come is not made, a check done is not followed by uploads
void dm_cloud_local_refresh(void);     // this device's save archived (its size and hash for the page), once, when not known yet
void dm_launcher_start_now(int nosync);   // datasync_android.c: the player chose to play (B on a card): the launcher starts the game once the start is ready
void dm_cloud_attach(ANativeActivity *act);   // onCreate, on the main thread: the activity, the app's names, the device's key and model (the launcher may link before the start reaches the cloud)
int dm_cloud_link_begin(void);   // the launcher's Link this device (and New code): a code asked for on a thread of its own, then polled every 3 s; -1 when it cannot be now
void dm_cloud_link_cancel(void); // B on the link page: the code is let go (a poll that runs is not waited for)
void dm_cloud_link_end(void);    // Continue on Done: the page goes and, once the start has reached the cloud, its check runs (dm_cloud_act DM_CLOUD_ACT_CHECK)
int dm_cloud_unlink(void);       // Unlink this app: sync.txt removed (the device's other games and the device stay linked on the server); 0, or -1 when it cannot be now
#endif

// the game's corner mark (gfx_vk_present.c ov_mark), icons only, on a dark disc in the top right corner: a turning arc while an upload runs; a white check on a green disc that pops (0.2 s) and
// fades after an upload went up; a cloud struck through while the save waits for a network; an orange "!" when another device's save is newer (the next start asks).
// NONE: nothing is drawn. *age_ms: since the mark began
enum { DM_CLOUD_MARK_NONE, DM_CLOUD_MARK_BUSY, DM_CLOUD_MARK_SAVED, DM_CLOUD_MARK_OFFLINE, DM_CLOUD_MARK_CONFLICT };
#define DM_CLOUD_MARK_SAVED_MS 1500
#define DM_CLOUD_MARK_OFFLINE_MS 1500
#define DM_CLOUD_MARK_CONFLICT_MS 2500
int dm_cloud_mark(int64_t *age_ms);
