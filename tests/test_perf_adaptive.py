"""Exercise the Android adaptive controller's recovery using its actual C code."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import csrc  # noqa: E402


SOURCE = Path(__file__).resolve().parents[1] / 'nxnative/dm/android/perf_android.c'


def controller_source():
    src = SOURCE.read_text(encoding='utf-8')
    controller = csrc.between(src, 'static const int64_t STEPS[]', '// hle_nvn.c, the first acquire')
    hint, shown = controller.index('// The frame\'s work to the CPU hint'), controller.index('// The frames\' time on screen')
    return controller[:hint] + controller[shown:]   # without hint_report: the Android hint API (H, g_hint), called only from dm_perf_frame


HEAD = r'''
#include <assert.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define LOG(...) do { if (0) printf(__VA_ARGS__); } while (0)
#ifdef _WIN32  // MinGW (the Windows release runner) has no setenv; "NAME=" removes the variable
static int setenv(const char *k, const char *v, int o) { char s[256]; (void)o; snprintf(s, sizeof s, "%s=%s", k, v); return _putenv(s); }
static int unsetenv(const char *k) { char s[256]; snprintf(s, sizeof s, "%s=", k); return _putenv(s); }
#endif
'''


class AdaptiveFrameRate(unittest.TestCase):
    def build_and_run(self, main, runs=([],)):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            self.skipTest('a C compiler is required')
        harness = HEAD + csrc.PACING_INCLUDE + controller_source() + main
        with tempfile.TemporaryDirectory(prefix='nx2a_adaptive_') as tmp:
            c = Path(tmp) / 'check.c'
            exe = Path(tmp) / 'check'
            c.write_text(harness, encoding='utf-8'); csrc.pacing_files(tmp)   # perf_android.c asks dm_pacing.c
            result = subprocess.run([cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-I' + tmp, str(c), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            for args in runs:
                with self.subTest(policy=args or 'default'):
                    result = subprocess.run([str(exe), *args], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_downshift_veto_allows_median_recovery(self):
        self.build_and_run(r'''
static int64_t mixed_work(int i) { return i % 64 < 48 ? 15000000 : 27000000; }
int main(int argc, char **argv) {
    (void)argv; (void)bg_mark_at;
    int median = argc > 1;
    unsetenv("DM_FPS_PIN"); unsetenv("DM_ADAPTIVE_FPS");
    if (median) setenv("DM_ADAPT_UP_PCT", "50", 1); else unsetenv("DM_ADAPT_UP_PCT");
    game_mode_sync();   // presenting-frame policy initialization
    assert(dm_present_period_adapt(16666667) == 16666667);
    g_step = 1; g_last_change = 1000000000ll; g_up_hold = 20000000000ll;
    g_nwork = 64; g_nshown = 64;
    for (int i = 0; i < 64; i++) g_work_hist[i] = mixed_work(i);
    assert(work_pct(50) == 15000000 && work_pct(90) == 27000000);
    adapt_frame(mixed_work(0), 2000000000ll);  // four-second settling hold
    assert(g_step == 1 && g_ok_frames == 0);
    for (int i = 1; i <= 121; i++) adapt_frame(mixed_work(i), 6000000000ll + i * 25000000ll);
    assert(g_step == 1);  // the longer recovery hold still applies
    assert(g_held_back == 121);
    assert(g_ok_frames == (median ? 121 : 0));
    adapt_frame(mixed_work(122), 21000000000ll);
    assert(g_step == (median ? 0 : 1));  // default p90 policy stays at 40

    g_step = 0; g_last_change = g_last_up = 21000000000ll;
    g_nwork = 64; g_nshown = 64;
    for (int i = 0; i < 64; i++) { g_work_hist[i] = 19000000; g_late[i] = i < 7; }
    adapt_frame(19000000, 24000000000ll);
    assert(g_step == 0);  // no early downshift within four seconds
    adapt_frame(19000000, 26000000000ll);
    assert(g_step == 1 && g_last_change == 26000000000ll);
    assert(g_up_hold == 40000000000ll);  // a failed recovery doubles the hold
    assert(g_nwork == 0 && g_nshown == 0 && g_ok_frames == 0);
    return 0;
}
''', runs=([], ['median']))

    def test_save_and_upload_frames_left_out(self):
        # The game's save (marks at its writes and commits) and an upload (busy from its start to its end): their long frames are not judged and the rate holds
        # until BG_TAIL_NS after the last mark; a window open longer than BG_SPAN_NS is judged again, so a load that stays steps down as before
        self.build_and_run(r'''
static void heavy_history(void) {   // 10 of 64 frames over the 60 fps period (p90 over it) and 7 of 64 late on screen: a step down without a window
    g_step = 0; g_last_change = g_last_up = 0; g_nwork = 64; g_nshown = 64; g_ok_frames = 0;
    for (int i = 0; i < 64; i++) { g_work_hist[i] = i < 10 ? 40000000 : 14000000; g_late[i] = i < 7; }
}
int main(void) {
    unsetenv("DM_FPS_PIN"); unsetenv("DM_ADAPTIVE_FPS"); unsetenv("DM_ADAPT_UP_PCT");
    game_mode_sync();
    const int64_t S = 1000000000ll, T = 100 * S;
    // a save: long frames between its writes are left out, the history is unchanged
    heavy_history(); bg_mark_at(0, T);
    for (int i = 0; i < 20; i++) { adapt_frame(150000000, T + i * 50000000ll); if (i % 5 == 0) bg_mark_at(0, T + i * 50000000ll); }
    assert(g_bg_skipped == 20 && g_nwork == 64 && g_step == 0);
    // frames that fit go in, and the heavy history of before is held back while the window lasts (1.5 s after the last mark at T + 0.75 s)
    unsigned held = g_held_back;
    adapt_frame(14000000, T + 2 * S);
    assert(g_step == 0 && g_nwork == 65 && g_bg_held == 1 && g_held_back == held + 1);
    // the window over, the same history is judged as before: a load that is really there steps down
    adapt_frame(14000000, T + 2 * S + 300000000ll);
    assert(g_step == 1);

    // an upload busy for 3 s: left out from its start to 1.5 s after its end
    heavy_history(); unsigned skipped = g_bg_skipped;
    const int64_t U = 200 * S;
    bg_mark_at(1, U);
    adapt_frame(120000000, U + 3 * S); assert(g_bg_skipped == skipped + 1 && g_step == 0);
    bg_mark_at(-1, U + 3 * S);
    adapt_frame(120000000, U + 4 * S); assert(g_bg_skipped == skipped + 2 && g_step == 0);
    adapt_frame(120000000, U + 5 * S); assert(g_bg_skipped == skipped + 2 && g_step == 1);   // judged again (and its own frame recorded)
    assert(g_bg_busy == 0);
    bg_mark_at(-1, U + 6 * S); assert(g_bg_busy == 0);   // an unmatched end does not go below zero

    // a window held open past 10 s (an upload hanging on the network, a game that writes all the time) is a load like any other again
    heavy_history(); skipped = g_bg_skipped;
    const int64_t V = 300 * S;
    bg_mark_at(1, V);
    adapt_frame(120000000, V + 9 * S); assert(g_bg_skipped == skipped + 1 && g_step == 0);
    adapt_frame(120000000, V + 11 * S); assert(g_bg_skipped == skipped + 1 && g_step == 1);
    bg_mark_at(-1, V + 12 * S);

    // a frame over the period by its GPU alone is judged even in a window
    heavy_history(); skipped = g_bg_skipped;
    const int64_t W = 400 * S;
    bg_mark_at(0, W); g_frame_cost = 30000000;
    adapt_frame(40000000, W + 100000000ll); assert(g_bg_skipped == skipped && g_nwork == 65);
    g_frame_cost = 0;
    return 0;
}
''')


if __name__ == '__main__':
    unittest.main()
