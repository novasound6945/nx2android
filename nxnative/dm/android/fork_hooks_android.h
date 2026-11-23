// This fork's connections in front_android.c (upstream's file), declared before its code: the headers of the fork's parts and the hooks front_android.c calls.
// Their bodies are in fork_hooks_android.inc, which front_android.c includes once, before ANativeActivity_onCreate (where all of upstream's statics they use exist).
// The fork's parts: the start screen and launcher (datasync_android.c, startscreen.c), the game data's download and its place (datasync_android.c, storage_android.c),
// the save's cloud sync (savecloud_android.c), the pause menu's Exit game (menu_exit.h) and the game's settings the launcher sets (limit=, rate=, res=, aspect=).
// Every hook is fork_*; upstream's code calls each in one line, in the place the fork's code ran before (the same order, thread and locks)
#pragma once
#include <dirent.h>
#include "menu_exit.h"
#include "env_path.h"
#include "storage_android.h"
#include "savecloud.h"
#include "rewrite_abi.h"

// datasync_android.c: the data from a server (porter --data-url), the shader preparation and a notice on the same screen, the launcher's gate, the background download
void dm_sync_window(ANativeWindow *w); int dm_sync_begin(ANativeActivity *act, const char *root, void (*done)(void));
int dm_prep_begin(ANativeActivity *act, ANativeWindow *w, void (*done)(void));   // (gfx_vk_gpl.inc vk_gpl_prepare)
int dm_notice_begin(ANativeActivity *act, ANativeWindow *w, const char *const ko[3], const char *const en[3], const char *detail);   // a notice the start stops at
int dm_launcher_gate(void (*go)(void));   // 1 when the launcher holds the game until its Start (go() then, on this thread), 0 to start it now
int dm_sync_detach(ANativeActivity *act); int dm_sync_reattach(ANativeActivity *act); void dm_sync_visible(int on);
int dm_storage_ask(ANativeActivity *act, void (*done)(const char *root));   // the card that holds the data is not in: insert it, or internal storage
void dm_overlay_redraw(void);   // gfx_vk_present.c (front_android.c's weak default)

// ---- the activity's callbacks (front_android.c) ----
static void fork_window_created(ANativeWindow *w);   // on_window_created, after the window is kept
static void fork_window_destroyed(void);             // on_window_destroyed, after the window is let go, before the backend's swapchain goes
static void fork_on_pause(void);                     // on_pause, last
static void fork_on_resume(void);                    // on_resume, last
static int fork_on_destroy(ANativeActivity *a);      // on_destroy, first: 1 when the process stays (the data downloads in its service)
static void fork_callbacks(ANativeActivity *act);    // onCreate, with the callbacks: onStart, onStop
static void fork_on_create_again(ANativeActivity *act);   // a second onCreate
static void fork_on_create(ANativeActivity *act, int sdk);   // onCreate's end, the log open: the start (the data's place, the start screen, the download...)
static int fork_queue_created(AInputQueue *q);       // on_queue_created, the queue kept: 1 when the start screen takes the input (before the game)
// ---- the game's start (front_android.c game_start, game_run: onCreate's tail once the start screen's steps let it) ----
static void game_start(ANativeActivity *act, int late); static void game_run(ANativeActivity *act);   // (front_android.c)
static void fork_start_ended(ANativeActivity *act, int late);   // the start ends without the game (g_quit set): the start screen closed, the activity finished if late
static int fork_start_gate(ANativeActivity *act, int late);     // before game_run: the data's contract, the cloud's start, the launcher; 1 when the game does not run now
static void fork_game_run(void);                                // game_run, first: the start screen closed, the cloud's uploads from here
static void fork_settings_loaded(ANativeActivity *act);         // game_run, after settings_load: the 4:3 variant, the frame rate limit
static void fork_mods(char *mods, size_t cap);                  // host_thread: the modules folder (mods/ or mods-4x3/)
// ---- env.txt and settings.txt ----
static const char *fork_env_value(const char *key, const char *value);   // env_file: the value to set (a path into the data's folder rewritten for where it is)
static void fork_env_file_done(void);                                    // env_file, the file read
static int fork_settings_parse(const char *line);   // settings_load: 1 when the line was the fork's (limit=, rate=, res=, aspect=)
static void fork_settings_write(FILE *f);           // settings_save: the fork's lines
// ---- the pause menu's Exit game (menu_exit.h) ----
static void fork_menu_open(void); static int fork_menu_stays(void); static void fork_menu_closed(void);
static void fork_menu_touch_down(int item, float x, float y, float W, float H); static void fork_menu_touch_move(int down_item, float x, float y, float W, float H);
static int fork_menu_touch_up(void); static void fork_menu_touch_cancel(void); static int fork_menu_key(uint64_t bit, int item); static void fork_menu_key_up(uint64_t bit);
