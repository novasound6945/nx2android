// The save's cloud sync on Android (savecloud_android.c), as the front end (front_android.c), the start screen (datasync_android.c) and the overlay (gfx_vk_present.c) see it
#pragma once
#include <stdint.h>
#ifdef __ANDROID__
#include <android/input.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#endif

// ---- the start screen: what it shows (savecloud_android.c fills it, datasync_android.c draws it: the drawing is one function of this state, so the screen's design can change
// without the sync's logic) ----
typedef struct { char head[64], who[64], date[48], ago[32]; int newer; } dm_cloud_card;
typedef struct {
    unsigned ver;          // changes with every update
    int urgent;            // a question: drawn at once (a step that only waits is drawn once it took 300 ms, so a quick sync shows no screen at all)
    char title[96];
    char line[3][200];     // the body's lines
    int progress;          // a bar: -1 none, else 0..1000
    int ncards;            // 2: two cards side by side (the conflict's choice), chosen by sel
    dm_cloud_card card[2];
    int nbtn; char btn[3][64];   // buttons under it, chosen by sel (when there are no cards)
    int sel;
    char foot[160];        // a small line at the bottom (the B key's hint)
} dm_cloud_view;
void dm_cloud_view_get(dm_cloud_view *v);   // savecloud_android.c: a copy of the current view

#ifdef __ANDROID__
// datasync_android.c: the screen (its EGL window picture), shown while the cloud's start runs: 1 when it runs and done() is called on the main thread once it ended
// (dm_cloud_screen_end); 0: no screen (no looper or thread), the caller goes on at once
int dm_cloud_screen_begin(ANativeActivity *act, ANativeWindow *w, void (*done)(void));
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
// the corner note (gfx_vk_present.c): its text while it shows (2 s after an upload went up or could not, 3 s for a trial build), else 0; id changes with each note
int dm_cloud_toast(char *text, int cap, unsigned *id);
