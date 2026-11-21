// bgdl.h: the background download's decisions (datasync_android.c), as pure functions the computer tests (tests/test_bgdl.py, tests/bgdl_fixture.c).
// The download itself stays datasync.h's (the list, the resumed parts, the hashes); the Android side keeps the process alive while it runs, through a foreground
// service of the APK's own (java_service/org/nx2a/DownloadService.java, in every APK: mkapk.py, porter apk-update) whose notification shows the progress and takes
// Pause / Resume / Cancel. This header decides:
//   - after a failed try, what the sync thread waits for (bg_plan): Resume (paused), the device's network (none: no timed retry, ConnectivityManager's callback wakes
//     it), or a timer (the server not reached on a network, a full disk, ...): 2, 4, 8 ... 60 s, 30 s for what only the player or an install can fix
//   - the notification's words and bar (bg_note), and when it is posted again (bg_due: on a change of state, else at most once a second)
//   - what the end of the download does when the app is not on the screen (bg_finish), and what Cancel does (bg_cancel)
// The numbers are DownloadService.java's (ST_*, CMD_*): keep them the same
#ifndef DM_BGDL_H
#define DM_BGDL_H
#include "datasync.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { BG_ST_NONE = 0, BG_ST_RUN, BG_ST_PAUSED, BG_ST_WAIT_NET, BG_ST_WAIT, BG_ST_DONE };                     // the notification's state (DownloadService.ST_*)
enum { BG_CMD_PAUSE = 1, BG_CMD_RESUME, BG_CMD_CANCEL, BG_CMD_NET_UP, BG_CMD_NET_DOWN, BG_CMD_STOPPED };   // from the service (DownloadService.CMD_*)
enum { BG_NEXT_TIMER, BG_NEXT_NET, BG_NEXT_RESUME, BG_NEXT_CANCEL };
typedef struct { int next, wait_s, delay; } bg_plan_t;   // what to wait for; TIMER: the seconds; delay: the timer of the try after this one

// After a try that failed with rc (datasync.h DS_ERR_*): net is the device's network (1 connected, 0 none, -1 not known), delay the timer this try was given, progressed
// whether it downloaded something (the next break waits from the start again)
static bg_plan_t bg_plan(int rc, int net, int paused, int cancelled, int delay, int progressed) {
    bg_plan_t p = {BG_NEXT_TIMER, 0, 2};
    if (delay < 2) delay = 2;
    if (cancelled) { p.next = BG_NEXT_CANCEL; return p; }
    if (paused) { p.next = BG_NEXT_RESUME; p.delay = 2; return p; }
    if (rc == DS_ERR_NET && net == 0) { p.next = BG_NEXT_NET; p.delay = 2; return p; }   // nothing to try until the network is back: no retries for nothing
    p.wait_s = progressed ? 2 : delay;
    p.delay = rc == DS_ERR_PACKAGE || rc == DS_ERR_SPACE || rc == DS_ERR_LOCAL ? 30 : p.wait_s * 2 > 60 ? 60 : p.wait_s * 2;   // (DS_ERR_LOCAL: until an install pushes what is missing)
    return p;
}

// The download's end with the app not on the screen: GO (it is: the start goes on), WAIT (in the background, the activity alive: "Download complete · tap to start", the
// start goes on once it is back), EXIT (its activity gone, the process kept only for the download: the notification stays, the process ends, the next start is a new one)
enum { BG_FIN_GO, BG_FIN_WAIT, BG_FIN_EXIT };
static int bg_finish(int visible, int detached) { return detached ? BG_FIN_EXIT : visible ? BG_FIN_GO : BG_FIN_WAIT; }
// Cancel in the notification: the app on the screen stays, paused (its screen offers Resume); in the background the process ends (the parts downloaded stay: the next start
// resumes them)
enum { BG_CANCEL_PAUSE, BG_CANCEL_EXIT };
static int bg_cancel(int visible, int detached) { return visible && !detached ? BG_CANCEL_PAUSE : BG_CANCEL_EXIT; }
// The activity destroyed (Back, swiped from Recents) while the data downloads: the process stays for the service (1), else it ends as before (front_android.c on_destroy)
static int bg_keep_alive(int downloading, int service_on) { return downloading && service_on; }

static void bg_fmt_bytes(char *o, size_t n, uint64_t done, uint64_t total) {   // as startscreen.c stv_fmt_bytes
    if (total >= 1024ull << 20) snprintf(o, n, "%.1f / %.1f GB", done / 1073741824.0, total / 1073741824.0);
    else snprintf(o, n, "%.0f / %.0f MB", done / 1048576.0, total / 1048576.0);
}
static void bg_fmt_eta(char *o, size_t n, double s, int ko) {   // as startscreen.c stv_fmt_eta; "" when not known
    if (s < 0 || s != s) { if (n) o[0] = 0; return; }
    if (s < 60) { int v = ((int)ceil(s) + 4) / 5 * 5; if (v < 5) v = 5; snprintf(o, n, ko ? "약 %d초 남음" : "about %d s left", v); }
    else if (s < 3600) snprintf(o, n, ko ? "약 %d분 남음" : "about %d min left", (int)ceil(s / 60));
    else { int m = (int)ceil(s / 60); snprintf(o, n, ko ? "약 %d시간 %d분 남음" : "about %d h %d min left", m / 60, m % 60); }
}

typedef struct { int state, permille; char title[160], text[200], b_pause[32], b_cancel[32]; } bg_note_t;   // permille -1: no bar (or one that moves)
// The notification of state for the game named game: done of total bytes, eta seconds left (-1: not known), wait_s the seconds of a timer (BG_ST_WAIT). phase
// (datasync.h DS_PH_*): DS_PH_CHECK while it runs is the check of the files the folder has (a try after a break hashes them again: tens of seconds for a pack of
// GBs), "Checking the files · 1.2 / 3.1 GB" with its bar and no time left; any other phase is the download's
static void bg_note_phase(bg_note_t *n, int ko, const char *game, int state, int phase, uint64_t done, uint64_t total, double eta, int wait_s) {
    memset(n, 0, sizeof *n); n->state = state; n->permille = -1;
    snprintf(n->title, sizeof n->title, "%s", game && game[0] ? game : ko ? "게임 데이터" : "Game data");
    char b[64] = "", e[64] = "";
    if (total) { bg_fmt_bytes(b, sizeof b, done < total ? done : total, total); n->permille = (int)((done < total ? done : total) * 1000 / total); }
    if (state == BG_ST_RUN && phase == DS_PH_CHECK) snprintf(n->text, sizeof n->text, "%s%s%s", ko ? "파일 확인 중" : "Checking the files", b[0] ? " · " : "", b);
    else if (state == BG_ST_RUN) {
        bg_fmt_eta(e, sizeof e, eta, ko);
        snprintf(n->text, sizeof n->text, "%s%s%s%s%s", ko ? "게임 데이터 받는 중" : "Downloading the game data", b[0] ? " · " : "", b, e[0] ? " · " : "", e);
    } else if (state == BG_ST_PAUSED) snprintf(n->text, sizeof n->text, "%s%s%s", ko ? "일시정지됨" : "Paused", b[0] ? " · " : "", b);
    else if (state == BG_ST_WAIT_NET) snprintf(n->text, sizeof n->text, "%s", ko ? "연결을 기다리는 중 · 연결되면 이어서 받아요" : "Waiting for a connection · it goes on once there is one");
    else if (state == BG_ST_WAIT) snprintf(n->text, sizeof n->text, ko ? "%d초 뒤 다시 시도해요" : "Trying again in %d s", wait_s);
    else if (state == BG_ST_DONE) { snprintf(n->text, sizeof n->text, "%s", ko ? "다운로드 완료 · 눌러서 시작" : "Download complete · tap to start"); n->permille = -1; }
    if (state == BG_ST_RUN || state == BG_ST_WAIT_NET || state == BG_ST_WAIT) snprintf(n->b_pause, sizeof n->b_pause, "%s", ko ? "일시정지" : "Pause");
    else if (state == BG_ST_PAUSED) snprintf(n->b_pause, sizeof n->b_pause, "%s", ko ? "이어 받기" : "Resume");
    if (state != BG_ST_DONE && state != BG_ST_NONE) snprintf(n->b_cancel, sizeof n->b_cancel, "%s", ko ? "취소" : "Cancel");
    if (state == BG_ST_WAIT_NET || state == BG_ST_WAIT) n->permille = -1;   // (a stopped bar would look like one moving: none while it waits)
}
static void bg_note(bg_note_t *n, int ko, const char *game, int state, uint64_t done, uint64_t total, double eta, int wait_s) { bg_note_phase(n, ko, game, state, DS_PH_DOWNLOAD, done, total, eta, wait_s); }
// Whether the notification is posted again now (ms clocks): a new state at once, the same one at most once a second and only when its bar or words changed (the time
// left: every 10 s)
static int bg_due(int64_t now, int64_t last, int state, int last_state, int permille, int last_permille) {
    if (state != last_state || !last) return 1;
    if (now - last < 1000) return 0;
    return permille != last_permille || now - last >= 10000;
}
#endif
