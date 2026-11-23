// This fork's connections in front_android.c (upstream's file): the bodies of the hooks fork_hooks_android.h declares, and the fork's start (the data's place, the start
// screen and launcher, the download, the shader preparation, the cloud's start) that runs before upstream's game start (front_android.c game_start, game_run). Included
// once, before ANativeActivity_onCreate: everything of upstream's it uses exists there. Nothing here changes what ran or in which order; it only moved out of upstream's lines
#include "game_opts_android.inc"   // settings.txt limit=, rate=, res= (the launcher's App settings)
#include "menu_exit_android.inc"   // the pause menu's Exit game

// ---- the activity's callbacks ----
// A window made again (back from Home or the lock screen) while the pause menu (or the editor, a text entry, the save picker) holds the game: the game sends no frame,
// so the new surface stayed black (a few frames a second at most, the menu not drawn) until the menu's state changed (B closed it). The last frame and the menu are
// presented again at once (and every vsync after while the menu stays: gfx_vk_present.c present_thread)
static void fork_window_created(ANativeWindow *w) { dm_sync_window(w); if (dm_menu_state() >= 0) dm_overlay_redraw(); }   // (-1 while nothing holds the game, and before it runs)
static void fork_window_destroyed(void) { dm_sync_window(NULL); }
static void fork_on_pause(void) { dm_cloud_pause(); }     // (savecloud_android.c: the owed upload on a thread of its own)
static void fork_on_resume(void) { dm_cloud_resume(); }
static int fork_on_destroy(ANativeActivity *a) { return dm_sync_detach(a); }   // while the data downloads in its service (datasync_android.c), the process stays for it
static void on_start(ANativeActivity *a) { (void)a; dm_sync_visible(1); }   // (the background download: the start goes on once the app is back)
static void on_stop(ANativeActivity *a) { (void)a; dm_sync_visible(0); }
static void fork_callbacks(ANativeActivity *act) { act->callbacks->onStart = on_start; act->callbacks->onStop = on_stop; }
// a second onCreate (configuration change) keeps the running game; after the activity was destroyed while the data downloaded (the process kept by its service),
// the new one takes the download's screen (g_act, set before)
static void fork_on_create_again(ANativeActivity *act) { dm_sync_reattach(act); }
// datasync_android.c, once the game data moved to another volume (storage_android.c): the app ends (the next start is a new process, which reads the new root); any thread
void dm_front_quit(void) {
    static _Atomic int once; if (atomic_exchange(&once, 1)) return;
    LOG("storage: the data moved: the app ends"); g_quit = 1; act_finish(g_act);
}

// ---- the input before the game: the start screen's ----
static _Atomic int g_syncing;   // the data download runs (dm_sync_begin): the game, its input thread with it, starts when it ends (game_start)
static int on_input_early(int fd, int events, void *data) {   // the main thread's looper: events before the game, none of them handled but the start screen's (the cloud's questions, Retry)
    (void)fd; (void)events; (void)data; AInputEvent *e;
    while (g_queue && AInputQueue_getEvent(g_queue, &e) >= 0) { if (AInputQueue_preDispatchEvent(g_queue, e)) continue; AInputQueue_finishEvent(g_queue, e, dm_start_input(e) || dm_cloud_input(e)); }   // (the launcher's first: it hands a question's input to the cloud)
    return 1;
}
static int fork_queue_created(AInputQueue *q) {
    if (atomic_load(&g_syncing)) { AInputQueue_attachLooper(q, ALooper_forThread(), 2, on_input_early, NULL); return 1; }
    return 0;
}

// ---- env.txt: a path into the data's folder, for where the data is ----
static int g_env_said, g_env_moved;   // (env.txt is read at the shader preparation and again at the game's start: the rewrites are logged once)
static char g_env_own[1200];
static const char *fork_env_value(const char *key, const char *value) {
    if (strcmp(key, "DM_CTL") && (dm_env_own_path(value, g_root, g_env_own, sizeof g_env_own) || dm_env_moved_path(value, g_root, g_env_own, sizeof g_env_own))) {   // (DM_CTL: devtools' file, in the internal files folder)
        // env_path.h: a pack ported under another package (an APK renamed by porter apk-update --package), or its internal files folder's path while the data is on an
        // SD card (storage_android.c), or the reverse
        if (!g_env_said) LOG("env: %s=%s is not the data's folder: read as %s", key, value, g_env_own);
        g_env_moved = 1; return g_env_own;
    }
    return value;
}
static void fork_env_file_done(void) { if (g_env_moved) g_env_said = 1; g_env_moved = 0; }

// ---- settings.txt's keys of the fork's: limit=, rate=, res= (game_opts_android.inc) and aspect= (the 4:3 variant, below) ----
static char g_set_aspect[16];   // settings.txt's aspect= (auto, 4:3, 16:9): which rewrite of the game runs (aspect_variant); kept as it is when the menu saves
static int fork_settings_parse(const char *line) {
    if (game_opt_parse(line)) return 1;
    if (sscanf(line, "aspect=%15s", g_set_aspect) == 1) { if (strcmp(g_set_aspect, "auto")) setenv("DM_ASPECT", g_set_aspect, 1); else unsetenv("DM_ASPECT"); return 1; }   // aspect_variant
    return 0;
}
static void fork_settings_write(FILE *f) { game_opt_write(f); if (g_set_aspect[0]) fprintf(f, "aspect=%s\n", g_set_aspect); }

// ---- which rewrite of the game's code: mods/, or mods-4x3/ (a data pack with the variant, android.tsv variant 4x3: the game rendering 4:3) on a screen narrower than
// 3:2 (Retroid Pocket Nova, 1280x960). DM_ASPECT (env.txt, or settings.txt's aspect=) = 4:3 or 16:9 chooses by hand; auto or none goes by the screen. The screen's size is the
// display's real size (the app is landscape, so the long side is its width whatever the rotation at launch); env-4x3.txt, the variant's own env rows, comes after env.txt ----
static char g_mods[600];   // the modules folder the host runs (host_thread)
static void fork_mods(char *mods, size_t cap) { snprintf(mods, cap, "%s", g_mods); }
static float screen_aspect(ANativeActivity *act, int *w, int *h) {   // long side / short side of the display, 0 when not known
    JNIEnv *E = act->env; *w = *h = 0;
    jclass cA = (*E)->GetObjectClass(E, act->clazz); jmethodID mWm = cA ? (*E)->GetMethodID(E, cA, "getWindowManager", "()Landroid/view/WindowManager;") : NULL;
    jobject wm = mWm ? (*E)->CallObjectMethod(E, act->clazz, mWm) : NULL; if (jx(E) || !wm) return 0;
    jclass cWm = (*E)->FindClass(E, "android/view/WindowManager"), cD = (*E)->FindClass(E, "android/view/Display"), cM = (*E)->FindClass(E, "android/util/DisplayMetrics"); if (jx(E) || !cWm || !cD || !cM) return 0;
    jmethodID mDisp = (*E)->GetMethodID(E, cWm, "getDefaultDisplay", "()Landroid/view/Display;"), mReal = (*E)->GetMethodID(E, cD, "getRealMetrics", "(Landroid/util/DisplayMetrics;)V"),
              mNew = (*E)->GetMethodID(E, cM, "<init>", "()V"); if (jx(E) || !mDisp || !mReal || !mNew) return 0;
    jobject d = (*E)->CallObjectMethod(E, wm, mDisp), m = (*E)->NewObject(E, cM, mNew); if (jx(E) || !d || !m) return 0;
    (*E)->CallVoidMethod(E, d, mReal, m); if (jx(E)) return 0;
    jfieldID fw = (*E)->GetFieldID(E, cM, "widthPixels", "I"), fh = (*E)->GetFieldID(E, cM, "heightPixels", "I"); if (jx(E) || !fw || !fh) return 0;
    *w = (*E)->GetIntField(E, m, fw); *h = (*E)->GetIntField(E, m, fh);
    int lo = *w < *h ? *w : *h, hi = *w < *h ? *h : *w;
    return lo > 0 ? (float)hi / (float)lo : 0;
}
static void aspect_variant(ANativeActivity *act) {
    char v[600]; struct stat st; snprintf(g_mods, sizeof g_mods, "%s/mods", g_root); snprintf(v, sizeof v, "%s/mods-4x3", g_root);
    if (stat(v, &st) || !S_ISDIR(st.st_mode)) return;   // a pack without variants: as before
    const char *want = getenv("DM_ASPECT"); int w, h; float r = screen_aspect(act, &w, &h);
    int four = want && !strcmp(want, "4:3") ? 1 : want && !strcmp(want, "16:9") ? 0 : r > 0 && r < 1.5f;
    if (four) { snprintf(g_mods, sizeof g_mods, "%s", v); snprintf(v, sizeof v, "%s/env-4x3.txt", g_root); env_file(v); }
    LOG("modules: %s (screen %dx%d, %.3f:1; DM_ASPECT=%s: %s)", four ? "mods-4x3, the 4:3 variant" : "mods, the game's own 16:9", w, h, r, want ? want : "auto",
        want && (!strcmp(want, "4:3") || !strcmp(want, "16:9")) ? "chosen by hand" : "by the screen, 4:3 below 1.5:1");
}

// ---- the rewritten modules' contract with this host (dm/host/rewrite_abi.h): every <module>.fixups.tsv of mods/ and mods-<variant>/ says the REWRITE_ABI it was written
// for. A pack of another number would crash somewhere in the game's code (an APK whose host was updated alone, porter apk-update, over data that needs a port again): the app
// says so on the screen and in the log, and the game is not started. A table without the number (a pack from before it) is run as before; loader.c logs it ----
static int rewrite_abi_fits(char *why, size_t cap) {   // 0: a module of another contract (why: which, for the screen)
    DIR *top = opendir(g_root); struct dirent *e; int ok = 1;
    if (!top) return 1;
    while (ok && (e = readdir(top))) {
        if (strcmp(e->d_name, "mods") && strncmp(e->d_name, "mods-", 5)) continue;
        char d[900]; snprintf(d, sizeof d, "%s/%s", g_root, e->d_name);
        DIR *m = opendir(d); struct dirent *f; if (!m) continue;
        while (ok && (f = readdir(m))) {
            size_t n = strlen(f->d_name); if (n < 11 || strcmp(f->d_name + n - 11, ".fixups.tsv")) continue;
            char p[1200], line[256]; snprintf(p, sizeof p, "%s/%s", d, f->d_name); FILE *t = fopen(p, "r"); if (!t) continue;
            while (fgets(line, sizeof line, t) && line[0] == '#')
                if (!strncmp(line, "# REWRITE_ABI", 13) && atoi(line + 13) != DM_REWRITE_ABI) {
                    snprintf(why, cap, "%s/%s: REWRITE_ABI %d, this app's host %d", e->d_name, f->d_name, atoi(line + 13), DM_REWRITE_ABI); ok = 0; break;
                }
            fclose(t);
        }
        closedir(m);
    }
    closedir(top);
    return ok;
}

// ---- the start: from onCreate to game_start, through the data's place, the download, the shader preparation; then the cloud's start and the launcher to game_run ----
static int g_sdk;
static char g_home[512];   // the internal files folder (DM_HOME): the log, saves/ and sync.txt, whatever volume holds the game data (DM_ROOT: storage_android.c, the player's choice)
static void game_start_late(void); static int prep_start(void); static void prep_late(void); static void cloud_done(int quit); static void launch_go(void);
// the data root (storage_android.c's choice) for everything of the start: the paths under it, as the environment names them
static void root_apply(const char *root) {
    char p[700]; snprintf(g_root, sizeof g_root, "%s", root);
    setenv("DM_ROOT", g_root, 1);
    snprintf(p, sizeof p, "%s/RomFS", g_root); setenv("DM_ROMFS", p, 0);
    snprintf(p, sizeof p, "%s/shaders.dmsv", g_root); setenv("DM_VK_SHADERS", p, 0);
    snprintf(g_mods, sizeof g_mods, "%s/mods", g_root);
}
static void start_flow(ANativeActivity *act, int late);
static void storage_found(const char *root) {   // the main thread, the start's question answered (the card is in again, or internal storage chosen): the start as from onCreate
    root_apply(root); LOG("storage: the start goes on with %s", g_root);
    if (g_queue) AInputQueue_detachLooper(g_queue);   // (start_flow attaches it again as onCreate's input would be)
    start_flow(g_act, 1);
    if (atomic_load(&g_syncing)) { if (g_queue) AInputQueue_attachLooper(g_queue, ALooper_forThread(), 2, on_input_early, NULL); return; }
    if (g_queue && !g_quit) { while (!g_ilooper) usleep(1000); AInputQueue_attachLooper(g_queue, g_ilooper, 1, on_input, NULL); }
}
// onCreate's end (g_root: the internal files folder, the log open in it): the data's place, then the start screen (datasync_android.c), one for the data download, the
// shader preparation and the cloud's start: it shows only once they took 300 ms, and closes (its thread, EGL, fonts and icon gone) before the game takes the window (game_run).
// An APK ported with --data-url downloads its data first, on threads of its own; the game starts on this thread when that ends
static void fork_on_create(ANativeActivity *act, int sdk) {
    snprintf(g_home, sizeof g_home, "%s", g_root);
    setenv("DM_HOME", g_home, 1);
    if (act->internalDataPath) setenv("DM_INTERNAL", act->internalDataPath, 1);   // the app's own storage, where code may be loaded from (gfx_vk_core.c drv_slot: a test driver)
    g_sdk = sdk;
    char root[512]; int missing = dm_storage_start(act, root, sizeof root);
    dm_cloud_attach(act);   // (savecloud_android.c: the device link's key, model and the app's names, before the launcher can ask for a link)
    if (missing) {   // the card that holds the data is not in: the start screen asks (insert it, or internal storage), then the start goes on from storage_found
        dm_start_screen_open(act, g_home); atomic_store(&g_syncing, 1);   // (nothing reads the data before the answer: DM_ROOT is not set yet; the input is the screen's)
        if (dm_storage_ask(act, storage_found)) return;
        LOG("[RED] storage: the start screen cannot ask: internal storage is used"); dm_storage_use_internal(root, sizeof root);
        root_apply(root); start_flow(act, 0); return;
    }
    root_apply(root);
    dm_start_screen_open(act, g_root);
    start_flow(act, 0);
}
static void start_flow(ANativeActivity *act, int late) {   // onCreate's start once the data root is known (late: after onCreate returned, the card's question answered)
    dm_cloud_precheck(act);   // (savecloud_android.c: the cloud's early check beside the data's step: the link, the server's meta; its questions wait for game_start)
    atomic_store(&g_syncing, 1);
    if (dm_sync_begin(act, g_root, prep_late)) return;
    if (prep_start()) return;
    atomic_store(&g_syncing, 0);
    game_start(act, late);
}
// The shader preparation (datasync_android.c dm_prep_begin) between the download and the game: with env.txt's switches read (the Vulkan driver, DM_VK_GPL), as game_start reads them again
static int prep_start(void) { char p[700]; snprintf(p, sizeof p, "%s/env.txt", g_root); env_file(p); return dm_prep_begin(g_act, phone_window(), game_start_late); }
static void prep_late(void) { if (!prep_start()) game_start_late(); }   // the main thread, once the data download ended
static void game_start_late(void) {   // the main thread, once the data download ended: its input queue goes to the input thread, as at a start without the download
    if (g_queue) AInputQueue_detachLooper(g_queue);
    atomic_store(&g_syncing, 0);
    game_start(g_act, 1);
    if (atomic_load(&g_syncing)) { if (g_queue) AInputQueue_attachLooper(g_queue, ALooper_forThread(), 2, on_input_early, NULL); return; }   // the cloud sync's start screen: still before the game
    if (g_queue && !g_quit) { while (!g_ilooper) usleep(1000); AInputQueue_attachLooper(g_queue, g_ilooper, 1, on_input, NULL); }   // (no input thread when the game was not started)
}
// game_start (late: after onCreate returned, the data download) ended without the game: the start screen goes, and the activity is finished at once when late
static void fork_start_ended(ANativeActivity *act, int late) { dm_start_screen_close(); if (late) act_finish(act); }
static int fork_start_gate(ANativeActivity *act, int late) {
    char why[300];
    if (!rewrite_abi_fits(why, sizeof why)) {   // the start screen shows the notice until the player leaves the app; its input goes to on_input_early (g_syncing), which hands the keys back
        LOG("[RED] the game's rewritten code does not fit this APK's host (%s): the game is not started. Install the APK that came with the data, or port the game again "
            "from its files (porter apk-update refuses such a pack)", why);
        static const char *const ko[3] = {"게임을 시작할 수 없어요", "이 앱과 기기에 있는 게임 데이터가 서로 맞지 않아요", "데이터와 함께 배포된 앱을 설치하거나 배포한 곳에 알려 주세요"};
        static const char *const en[3] = {"The game cannot start", "This app and the game data on the device do not fit together", "Install the app that came with the data, or tell where you got it"};
        atomic_store(&g_syncing, 1);
        if (!dm_notice_begin(act, phone_window(), ko, en, why)) { atomic_store(&g_syncing, 0); g_quit = 1; dm_start_screen_close(); if (late) act_finish(act); }
        return 1;
    }
    // the save's cloud sync (savecloud_android.c, sync.txt), still before the host mounts the save: its start screen runs on threads of its own and the game starts from
    // cloud_done on this thread; without sync.txt (or in a trial build) the game starts at once, as before
    atomic_store(&g_syncing, 1);
    if (dm_cloud_begin(act, phone_window(), cloud_done)) return 1;
    if (dm_launcher_gate(launch_go)) return 1;   // the launcher: the game starts with its Start (launch_go); the start screen keeps the input (g_syncing)
    atomic_store(&g_syncing, 0);
    return 0;
}
static void cloud_done(int quit) {   // the main thread, once the cloud sync's start ended: the game starts (its input queue to the input thread), or the player chose to end the app
    if (g_queue) AInputQueue_detachLooper(g_queue);
    atomic_store(&g_syncing, 0);
    if (quit) { LOG("cloud: the player cancelled the start: the app ends"); g_quit = 1; dm_start_screen_close(); act_finish(g_act); return; }
    if (dm_launcher_gate(launch_go)) { atomic_store(&g_syncing, 1); if (g_queue) AInputQueue_attachLooper(g_queue, ALooper_forThread(), 2, on_input_early, NULL); return; }
    game_run(g_act);
    if (g_queue) { while (!g_ilooper) usleep(1000); AInputQueue_attachLooper(g_queue, g_ilooper, 1, on_input, NULL); }
}
static void launch_go(void) {   // the main thread, the launcher's Start once the start was ready: the game starts (its input queue to the input thread)
    if (g_queue) AInputQueue_detachLooper(g_queue);
    atomic_store(&g_syncing, 0);
    game_run(g_act);
    if (g_queue) { while (!g_ilooper) usleep(1000); AInputQueue_attachLooper(g_queue, g_ilooper, 1, on_input, NULL); }
}
static void fork_game_run(void) {
    dm_start_screen_close();   // the window is the game's from here: the start screen's thread, EGL and everything it loaded are gone before the host starts
    dm_cloud_game_start();     // (the save's uploads, when this run syncs: from the game's start, not the check's, so the launcher's choices come first)
}
static void fork_settings_loaded(ANativeActivity *act) { aspect_variant(act); limit_apply(1); }   // (after the variant's env-4x3.txt, which comes after env.txt: Auto gives back what both set)
