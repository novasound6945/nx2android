// The pause menu's Exit game (menu_exit.h), front_android.c's part (fork_hooks_android.inc includes it; the hooks fork_menu_* are declared in fork_hooks_android.h).
// Exit game held (menu_exit.h ov_exit_hold): a press on its half starts the hold, a thread of its own redraws the filling half and ends the game when it completes:
// the cloud save's owed upload first (savecloud_android.c dm_cloud_flush, as onPause would start it: onDestroy's _exit comes too soon for it), then Activity.finish()
static pthread_mutex_t g_ex_mu = PTHREAD_MUTEX_INITIALIZER;
static ov_exit_hold g_ex; static int g_ex_src, g_ex_thread, g_ex_pick; static unsigned g_ex_ver;   // who holds (1 a finger, 2 A), the timing thread runs, a controller chose Exit game
static int g_menu_ex;   // the menu's pointer went down on Exit game's half (menu_touch)
static void menu_exit_now(void);   // (below)
static void *exit_run(void *a) {
    (void)a; int last = -1, lq = -1;
    for (;;) {
        float f; pthread_mutex_lock(&g_ex_mu); int k = ov_exit_tick(&g_ex, mono_ns(), &f), q = (int)(f * 30);
        if (k != last || q != lq) { g_ex_ver++; last = k; lq = q; }
        if (k == OV_EXIT_IDLE || k == OV_EXIT_GOING) g_ex_thread = 0;
        pthread_mutex_unlock(&g_ex_mu); dm_overlay_redraw();
        if (k == OV_EXIT_GOING) { menu_exit_now(); return NULL; }
        if (k == OV_EXIT_IDLE) return NULL;
        usleep(33000);
    }
}
static void exit_kick(void) {   // (g_ex_mu held) the timing thread, if it is not running
    g_ex_ver++;
    if (g_ex_thread) return;
    pthread_t t; g_ex_thread = !pthread_create(&t, NULL, exit_run, NULL); if (g_ex_thread) pthread_detach(t);
}
static void exit_press(int src) { pthread_mutex_lock(&g_ex_mu); if (!g_ex.going) { ov_exit_press(&g_ex, mono_ns()); g_ex_src = src; exit_kick(); } pthread_mutex_unlock(&g_ex_mu); }
static void exit_release(int src) { pthread_mutex_lock(&g_ex_mu); if (g_ex.t0 && g_ex_src == src) { ov_exit_release(&g_ex, mono_ns()); exit_kick(); } pthread_mutex_unlock(&g_ex_mu); }
static void exit_leave(void) { pthread_mutex_lock(&g_ex_mu); if (g_ex.t0 || g_ex.hint) { ov_exit_leave(&g_ex); exit_kick(); } pthread_mutex_unlock(&g_ex_mu); }
static void exit_pick(int on) { pthread_mutex_lock(&g_ex_mu); if (g_ex_pick != on) { g_ex_pick = on; g_ex_ver++; } pthread_mutex_unlock(&g_ex_mu); }
static int exit_chosen(void) { pthread_mutex_lock(&g_ex_mu); int c = g_ex_pick; pthread_mutex_unlock(&g_ex_mu); return c; }
int dm_menu_exit(float *f, int *chosen) { pthread_mutex_lock(&g_ex_mu); ov_exit_hold h = g_ex; int k = ov_exit_tick(&h, mono_ns(), f); *chosen = g_ex_pick; pthread_mutex_unlock(&g_ex_mu); return k; }
unsigned dm_menu_exit_ver(void) { pthread_mutex_lock(&g_ex_mu); unsigned v = g_ex_ver; pthread_mutex_unlock(&g_ex_mu); return v; }
// Exit game held to its end: the cloud save's owed upload goes up first (at most 8 s), then the app ends itself the way Android does (Activity.finish(), g_quit keeps it
// finished if it is resumed once more); without an activity (the adb shell dmhost) nothing ends
static void menu_exit_now(void) {
    if (!g_act) { LOG("[menu] exit game: no activity (the adb shell's dmhost): nothing ends"); pthread_mutex_lock(&g_ex_mu); g_ex.going = 0; g_ex_ver++; pthread_mutex_unlock(&g_ex_mu); return; }
    dm_haptics_android_tap(); LOG("[menu] exit game (held): the cloud save first, then Activity.finish()");
    if (dm_cloud_flush(8000) > 0) LOG("[menu] exit game: the cloud upload did not end within 8 s; the app ends anyway (what is owed goes up at the next start)");
    g_quit = 1; act_finish(g_act);
}
// ---- the calls from the menu's code (front_android.c menu_open, menu_close, menu_touch, menu_key; handle_event and input_thread for a key let go) ----
static void fork_menu_closed(void) { exit_leave(); }       // menu_close, the menu closed: nothing held
static void fork_menu_touch_cancel(void) { exit_leave(); } // menu_touch, the gesture cancelled
static void fork_menu_open(void) { exit_leave(); exit_pick(0); }   // the menu opens: Resume chosen, nothing held
static int fork_menu_stays(void) { pthread_mutex_lock(&g_ex_mu); int going = g_ex.going; pthread_mutex_unlock(&g_ex_mu); return going; }   // the game is ending: the menu stays
static void fork_menu_touch_down(int item, float x, float y, float W, float H) {   // a finger down on the button row: Exit game's half is held, not tapped
    g_menu_ex = item == OVM_RESUME && ov_menu_exit_at(x, y, W, H);
    if (item == OVM_RESUME) { exit_pick(g_menu_ex); if (g_menu_ex) exit_press(1); }
}
static void fork_menu_touch_move(int down_item, float x, float y, float W, float H) { if (down_item == OVM_RESUME && !ov_menu_exit_at(x, y, W, H)) exit_leave(); }   // off the half: let go
static int fork_menu_touch_up(void) {   // a press that began on Exit game, let go before its hold ended (a tap: the hint): never Resume. 1 when it was one
    if (!g_menu_ex) return 0;
    g_menu_ex = 0; exit_release(1); exit_pick(0); dm_overlay_redraw(); return 1;
}
static int fork_menu_key(uint64_t bit, int item) {   // a controller key in the menu: 1 when it was Exit game's (left and right on the button row, A held on Exit game)
    if (bit != DM_BTN_A) exit_leave();   // another key lets a held A go
    if ((bit == DM_BTN_LEFT || bit == DM_BTN_RIGHT) && item == OVM_RESUME) { exit_pick(bit == DM_BTN_LEFT); dm_overlay_redraw(); return 1; }   // Exit game (left) or Resume
    if (bit == DM_BTN_A && item == OVM_RESUME && exit_chosen()) { exit_press(2); return 1; }   // its end when A has been down 1.5 s
    return 0;
}
static void fork_menu_key_up(uint64_t bit) { if (bit == DM_BTN_A) exit_release(2); }   // a controller key let go while the menu is open: a held A on Exit game
