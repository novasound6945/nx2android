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
    char foot[160], foot2[160];           // small lines at the bottom (one: the hint; two: a note over the keys' hint)
} dm_cloud_view;
void dm_cloud_view_get(dm_cloud_view *v);   // savecloud_android.c: a copy of the current view

#ifdef __ANDROID__
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
#endif

// the pause menu's Cloud save row (front_android.c, gfx_vk_present.c)
enum { DM_CLOUD_OFF, DM_CLOUD_DISABLED, DM_CLOUD_TRIAL, DM_CLOUD_SKIPPED, DM_CLOUD_UPTODATE, DM_CLOUD_PENDING, DM_CLOUD_OFFLINE, DM_CLOUD_UPLOADING, DM_CLOUD_CONFLICT, DM_CLOUD_AUTH };
int dm_cloud_available(void);              // the row is shown: sync.txt is there
int dm_cloud_status(char *note, int cap);  // DM_CLOUD_*, and a note (when it last went up)
unsigned dm_cloud_menu_ver(void);          // changes with the status: the menu's panel is drawn again
void dm_cloud_menu_press(void);            // the row: Upload now when an upload is owed
void dm_cloud_menu_toggle(void);           // the row's switch: this game's sync on / off (kept in the sync state)
// the game's corner mark (gfx_vk_present.c ov_mark), icons only, on a dark disc in the top right corner: a turning arc while an upload runs; a white check on a green disc that pops (0.2 s) and
// fades after an upload went up; a cloud struck through while the save waits for a network; an orange "!" when another device's save is newer (the next start asks).
// NONE: nothing is drawn. *age_ms: since the mark began
enum { DM_CLOUD_MARK_NONE, DM_CLOUD_MARK_BUSY, DM_CLOUD_MARK_SAVED, DM_CLOUD_MARK_OFFLINE, DM_CLOUD_MARK_CONFLICT };
#define DM_CLOUD_MARK_SAVED_MS 1500
#define DM_CLOUD_MARK_OFFLINE_MS 1500
#define DM_CLOUD_MARK_CONFLICT_MS 2500
int dm_cloud_mark(int64_t *age_ms);
