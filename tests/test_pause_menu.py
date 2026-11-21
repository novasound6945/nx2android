"""The pause menu's settings.txt rows and its frame rate limit, through the production code on the computer.

settings_load, settings_save, limit_apply, the limit row's press (menu_do) and dm_menu_state are cut out of front_android.c, fps_pin (which holds the
limit, with game_mode_sync which reads it) out of perf_android.c, and the menu panel's drawing (ov_menu_panel) out of gfx_vk_present.c, with the font pack the APK ships. No phone or game is needed.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from porter.upstream import android_profile


ROOT = Path(__file__).resolve().parents[1]
DM = ROOT / 'nxnative/dm'
FONT = DM / 'third_party/fusion-pixel-font/fusion-pixel-12px-proportional-zh_tw.nxpf'   # porter's UI_FONT: the APK's assets/font/ui.bin


def cut(text, start, end):
    """text from start up to (not with) the next end after it"""
    i = text.index(start)
    return text[i:text.index(end, i)]


# the Windows C runtime has no setenv or unsetenv, and its rename does not replace a file (bionic's, which the app runs on, does)
POSIX = r'''
#ifdef _WIN32
static int setenv(const char *k, const char *v, int o) { (void)o; return _putenv_s(k, v); }
static int unsetenv(const char *k) { return _putenv_s(k, ""); }
#define rename(a, b) (remove(b), rename(a, b))
#endif
'''

SETTINGS_MAIN = r'''
static char *slurp(const char *p) { static char b[4096]; FILE *f = fopen(p, "r"); assert(f); size_t n = fread(b, 1, sizeof b - 1, f); b[n] = 0; fclose(f); return b; }   // (text: \r\n on Windows)
static void put(const char *p, const char *s) { FILE *f = fopen(p, "w"); assert(f); fputs(s, f); fclose(f); }
static const char *pin(void) { const char *e = getenv("DM_FPS_PIN"); return e ? e : "(unset)"; }
static int pin_now(void) { int s = -2; for (int i = 0; i < 30; i++) { game_mode_sync(); s = fps_pin(); } return s; }   // the presenting thread's game_mode_sync reads DM_FPS_PIN every 30th frame, fps_pin gives it: 0 60 fps, 1 40, 2 30, -1 no pin
static int limit_bits(void) { int st = dm_menu_state(); assert(st >= 0); return st >> 26 & 3; }   // what the panel is drawn with (gfx_vk_present.c ov_menu_panel)
int main(int argc, char **argv) {
    assert(argc == 2); snprintf(g_root, sizeof g_root, "%s", argv[1]);
    char p[600]; snprintf(p, sizeof p, "%s/settings.txt", g_root);
    atomic_store(&g_menu, 1);   // the menu open: dm_menu_state gives the panel's state

    // a new install: the data pack's settings.txt with the profile's setting limit=40, over env.txt's DM_FPS_PIN=60
    put(p, "docked=0\nlimit=40\naspect=4:3\nui=phone,a,5000,4000,1200\n"); setenv("DM_FPS_PIN", "60", 1);
    settings_load(); limit_apply(1);
    assert(atomic_load(&g_set_limit) == 40 && !strcmp(pin(), "40") && pin_now() == 1 && limit_bits() == 2);
    // the row steps 40, 30, Auto (the env files' 60 again), 60, 40; each step is saved, every other row with it as it was
    menu_press(OVM_LIMIT); assert(atomic_load(&g_set_limit) == 30 && !strcmp(pin(), "30") && pin_now() == 2 && limit_bits() == 3);
    const char *s = slurp(p); assert(strstr(s, "limit=30\n") && strstr(s, "docked=0\n") && strstr(s, "aspect=4:3\n") && strstr(s, "ui=phone,a,5000,4000,1200,0\n"));   // (written with the hidden field, 0: settings_save)
    menu_press(OVM_LIMIT); assert(atomic_load(&g_set_limit) == 0 && !strcmp(pin(), "60") && pin_now() == 0 && limit_bits() == 0 && strstr(slurp(p), "limit=0\n"));
    menu_press(OVM_LIMIT); assert(atomic_load(&g_set_limit) == 60 && !strcmp(pin(), "60") && pin_now() == 0 && limit_bits() == 1);
    menu_press(OVM_LIMIT); assert(atomic_load(&g_set_limit) == 40 && !strcmp(pin(), "40") && pin_now() == 1 && limit_bits() == 2);
    atomic_store(&g_set_limit, 0); settings_load(); assert(atomic_load(&g_set_limit) == 40);   // read back as saved

    // an older install's settings.txt (no limit= row: the installer keeps the device's own file) and env files without DM_FPS_PIN: Auto, the adaptive rate.
    // The variable is there from the start, holding no step (a change in the menu then replaces its value and never adds the variable)
    put(p, "overlay=1\nrumble=1\ndocked=0\nfps=0\nopacity=60\ngrid=0\n"); unsetenv("DM_FPS_PIN"); atomic_store(&g_set_limit, 0);
    settings_load(); limit_apply(1);
    assert(atomic_load(&g_set_limit) == 0 && getenv("DM_FPS_PIN") && !strcmp(pin(), "auto") && pin_now() == -1 && limit_bits() == 0);
    menu_press(OVM_LIMIT); assert(!strcmp(pin(), "60") && pin_now() == 0);
    menu_press(OVM_LIMIT); menu_press(OVM_LIMIT); menu_press(OVM_LIMIT);   // 40, 30, Auto
    assert(atomic_load(&g_set_limit) == 0 && !strcmp(pin(), "auto") && pin_now() == -1 && limit_bits() == 0 && strstr(slurp(p), "limit=0\n"));

    // values the menu has not: Auto
    put(p, "limit=45\n"); settings_load(); assert(atomic_load(&g_set_limit) == 0);
    put(p, "limit=120\n"); settings_load(); assert(atomic_load(&g_set_limit) == 0);
    return 0;
}
'''

PANEL_MAIN = r'''
static const drawn *find(const char *s) { for (int i = 0; i < g_ndrawn; i++) if (!strcmp(g_drawn[i].s, s)) return &g_drawn[i]; return NULL; }
int main(int argc, char **argv) {
    assert(argc == 2); FILE *f = fopen(argv[1], "rb"); assert(f); fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    g_font = malloc((size_t)n); assert(fread(g_font, 1, (size_t)n, f) == (size_t)n); fclose(f);
    static const char *const words[OVM_LIMITS] = {"Auto", "60 fps", "40 fps", "30 fps"};
    const float screens[][2] = {{1920, 1080}, {2400, 1080}, {1280, 960}, {1216, 760}};   // the Retroid Pocket 6, a 20:9 phone, the Retroid Pocket Nova, a 16:10 tablet
    for (unsigned q = 0; q < sizeof screens / sizeof screens[0]; q++) {
        float W = screens[q][0], H = screens[q][1], m[4]; ov_menu_rect(W, H, m);
        uint32_t pw = (uint32_t)m[2] + 1, ph = (uint32_t)m[3] + 1;   // the panel's raster: a texel a window pixel (gfx_vk_present.c g_ov.pw, ph)
        uint8_t *px = calloc((size_t)pw * ph, 4); assert(px); ov_canvas cv = {px, pw, 0, 0, pw, ph};
        float p[4] = {0, 0, (float)pw, (float)ph}, r[4]; ov_menu_rowrect(p, 0, r);
        int full = 3 * ovm_ref_scale(r[3]);   // half as large again as the half-resolution raster's letters (each of its texels was two pixels)
        assert(ovm_letters(r[3], r[2]) == full && full >= 3);   // the panel (ov_menu_rect) is wide enough for them: no row made them smaller
        for (int tab = 0; tab < OVM_TABS; tab++) for (int k = 0; k < OVM_LIMITS; k++) {
            g_ndrawn = 0; ov_menu_panel(&cv, tab << 8 | 60 << 9 | k << 26);
            const drawn *res = find("Resume"); assert(res && res->scale == full);   // no row made the menu's letters smaller
            if (tab != 1) continue;
            ov_menu_rowrect(p, 3, r);   // the Game tab's third row, under Mode
            const drawn *l = find("Frame rate limit"), *v = find(words[k]), *mode = find("Mode");
            assert(l && v && mode && l->scale == full && v->scale == full && mode->cy < l->cy && fabsf(l->cy - (r[1] + r[3] / 2)) < 0.01f && fabsf(v->cy - l->cy) < 0.01f);
            assert(v->x > l->x + cv_text_w(l->s, l->scale) + r[3] * 0.3f && v->x + cv_text_w(v->s, v->scale) <= r[0] + r[2]);   // after its label, inside the row
            for (int i = 0; i < g_ndrawn; i++) { const drawn *d = &g_drawn[i]; float w = cv_text_w(d->s, d->scale);   // every word inside the panel's rows
                ov_menu_rowrect(p, 0, r); assert(d->x >= r[0] - 0.5f && d->x + w <= r[0] + r[2] + 0.5f && PF_CAP * d->scale * 4 <= r[3] * 0.85f * 3); }
            for (int j = 0; j < OVM_LIMITS; j++) assert(j == k || !find(words[j]));   // the current value only: the row steps through them
        }
        // Exit game's half: its word while held (overlay_layout.h ov_exit_hold), red filling it from the left
        exit_state = OV_EXIT_HOLDING; exit_f = 0.5f; g_ndrawn = 0; ov_menu_panel(&cv, 0); assert(find("Hold to exit") && !find("Exit game") && find("Resume"));
        ov_menu_rowrect(p, OVM_ROWS - 1, r); { const uint8_t *q = px + 4 * ((size_t)(r[1] + r[3] * 0.2f) * pw + (size_t)(r[0] + r[3] * 0.3f));   // near the left end, over the fill
            assert(q[0] > 200 && q[1] < 120 && q[2] < 120); }
        exit_state = OV_EXIT_GOING; g_ndrawn = 0; ov_menu_panel(&cv, 0); assert(find("Exiting…"));
        exit_state = OV_EXIT_IDLE; exit_f = 0; g_ndrawn = 0; ov_menu_panel(&cv, 0); assert(find("Exit game"));
        free(px);
    }
    return 0;
}
'''

HOLD_MAIN = r'''
#define S 1000000000ll
static int tick(ov_exit_hold *h, int64_t t, float *f) { return ov_exit_tick(h, t, f); }
int main(void) {
    ov_exit_hold h = {0}; float f;
    // a tap: never the end; "Hold to exit" for 1.5 s, then as it was
    ov_exit_press(&h, 10 * S); assert(tick(&h, 10 * S + S / 10, &f) == OV_EXIT_HOLDING);
    ov_exit_release(&h, 10 * S + S / 5);
    for (int64_t t = 10 * S + S / 5; t < 14 * S; t += S / 30) { int k = tick(&h, t, &f); assert(k != OV_EXIT_GOING && k != OV_EXIT_HOLDING); assert((k == OV_EXIT_HINT) == (t < 10 * S + S / 5 + OV_EXIT_HINT_NS)); }
    // held 1.5 s: the half fills, then the end, once
    ov_exit_press(&h, 20 * S); assert(tick(&h, 20 * S, &f) == OV_EXIT_HOLDING && f == 0);
    assert(tick(&h, 21 * S, &f) == OV_EXIT_HOLDING && fabsf(f - 2.0f / 3) < 0.001f);
    assert(tick(&h, 20 * S + OV_EXIT_HOLD_NS - 1, &f) == OV_EXIT_HOLDING);
    assert(tick(&h, 20 * S + OV_EXIT_HOLD_NS, &f) == OV_EXIT_GOING && f == 1);
    ov_exit_release(&h, 22 * S); ov_exit_leave(&h); assert(tick(&h, 23 * S, &f) == OV_EXIT_GOING);   // (going: nothing takes it back)
    // held, then the finger off the half: as it was, no end however long after
    ov_exit_hold g = {0}; ov_exit_press(&g, 30 * S); assert(tick(&g, 30 * S + 8 * S / 10, &f) == OV_EXIT_HOLDING);
    ov_exit_leave(&g); for (int64_t t = 30 * S + 8 * S / 10; t < 35 * S; t += S / 30) assert(tick(&g, t, &f) == OV_EXIT_IDLE);
    // let go after a second (not a tap): as it was, no hint
    ov_exit_press(&g, 40 * S); ov_exit_release(&g, 41 * S); assert(tick(&g, 41 * S, &f) == OV_EXIT_IDLE && tick(&g, 44 * S, &f) == OV_EXIT_IDLE);
    // the halves of the button row (front_android.c menu_touch): Exit game on the left, Resume on the right, no other row
    const float screens[][2] = {{1280, 960}, {1920, 1080}};
    for (int q = 0; q < 2; q++) {
        float W = screens[q][0], H = screens[q][1], p[4], r[4]; ov_menu_rect(W, H, p); ov_menu_rowrect(p, OVM_ROWS - 1, r);
        float cy = r[1] + r[3] / 2;
        assert(ov_menu_exit_at(r[0] + 4, cy, W, H) && ov_menu_exit_at(p[0] + p[2] / 2 - 1, cy, W, H));
        assert(!ov_menu_exit_at(p[0] + p[2] / 2 + 1, cy, W, H) && !ov_menu_exit_at(r[0] + r[2] - 4, cy, W, H));
        ov_menu_rowrect(p, OVM_ROWS - 2, r); assert(!ov_menu_exit_at(r[0] + 4, r[1] + r[3] / 2, W, H));
        assert(!ov_menu_exit_at(p[0] - 5, cy, W, H));
    }
    return 0;
}
'''


class PauseMenu(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cc = shutil.which('cc') or shutil.which('clang')
        if not cls.cc:
            raise unittest.SkipTest('a C compiler is needed')
        cls.front = (DM / 'android/front_android.c').read_text(encoding='utf-8')
        cls.perf = (DM / 'android/perf_android.c').read_text(encoding='utf-8')
        cls.present = (DM / 'android/gfx_vk_present.c').read_text(encoding='utf-8')

    def _run(self, source, arg):
        with tempfile.TemporaryDirectory(prefix='nx2a_menu_') as tmp:
            tmp = Path(tmp)
            (tmp / 'android_profile.h').write_text(android_profile.header(None), encoding='utf-8')
            c, exe = tmp / 'check.c', tmp / 'check'
            c.write_text(source, encoding='utf-8')
            # the cut-out code comes with neighbours the checks do not call (unused statics are no fault of theirs)
            result = subprocess.run([self.cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-Wno-unused-variable', '-Wno-unused-const-variable',
                                     f'-I{tmp}', f'-I{DM / "android"}', f'-I{DM / "host"}', str(c), '-lm', '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            data = tmp / 'files'; data.mkdir()
            result = subprocess.run([str(exe), str(arg if arg else data)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return result.stdout

    def test_settings_and_frame_rate_limit(self):
        settings = cut(self.front, "// ---- settings (the pause menu's)", 'void dm_lan_android_start(')
        state = cut(self.front, 'static _Atomic int g_menu_hi = -1, g_menu_tab;', 'int dm_overlay_fps(void)')
        press = next(line.strip() for line in self.front.splitlines() if line.strip().startswith('else if (item == OVM_LIMIT)'))
        # fps_pin gives what game_mode_sync (the presenting thread, every 30th frame) read from DM_FPS_PIN; both come with the adaptive rate's state they touch
        pin = cut(self.perf, 'static int g_step = 0;', 'static int64_t work_pct(') + cut(self.perf, 'static int fps_pin(void) {', "// The frame's replay and GPU time")
        source = r'''
#include <assert.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "overlay_layout.h"
''' + POSIX + r'''
#define LOG(...) do { printf(__VA_ARGS__); putchar('\n'); } while (0)
static int opmode;
int dm_opmode(void) { return opmode; }
void dm_set_opmode(int m) { opmode = m; }
static int pad_labels(void) { return 0; }
static _Atomic int g_tx_on, g_menu;
// the Resolution, Detail distance and Frame rate rows' neighbours (front_android.c, before the settings): a game with none of them
static char g_res_scales[3][16]; static int res_steps(void) { return 0; } static void res_apply(int step) { (void)step; }
static _Atomic int g_set_detail; static void detail_apply(int step) { (void)step; }
int dm_fps_target_get(void) { return 0; }
// dm/host/dm_pacing.c's two switches the governor's policy reads (game_mode_sync), as there
static int dm_pacing_adaptive(int *explicit_set) { const char *e = getenv("DM_ADAPTIVE_FPS"); if (explicit_set) *explicit_set = e && *e; return !(e && !atoi(e)); }
static int dm_pacing_pin(void) { const char *e = getenv("DM_FPS_PIN"); int v = e ? atoi(e) : 0; return v == 60 || v == 40 || v == 30 ? v : 0; }
''' + settings + state + pin + 'static void menu_press(int item) {\n' + press[len('else '):] + '\n}\n' + SETTINGS_MAIN   # (the line ends in a comment)
        out = self._run(source, None)
        self.assertIn('[menu] frame rate limit: 40 fps (settings.txt limit=40, over the env files\' DM_FPS_PIN 60)', out)
        self.assertIn('[menu] frame rate limit: auto (DM_FPS_PIN=60, as the env files set it)', out)
        self.assertIn('[menu] frame rate limit: auto (the adaptive rate: the env files set no DM_FPS_PIN)', out)
        self.assertIn('limit=45 is not 0 (auto), 60, 40 or 30', out)

    def test_frame_rate_limit_row_drawn(self):
        canvas = cut(self.present, 'typedef struct { uint8_t *px; uint32_t stride, x0, y0, w, h; } ov_canvas;', 'static const uint8_t C_WHITE[4]')
        colours = cut(self.present, 'static const uint8_t C_WHITE[4]', 'static void ov_menu_icon(')
        panel = cut(self.present, 'static const uint8_t C_BG[4]', "// The layout editor's toolbar and prompt")
        source = r'''
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "overlay_layout.h"
#include "pixel_font.h"
static uint8_t *g_font;
const uint8_t *dm_overlay_font(void) { return g_font; }
float dm_overlay_scale(void) { return 1; }
int dm_save_note(char *out, int cap) { snprintf(out, (size_t)cap, "-"); return OVX_IDLE; }
int dm_menu_audio_available(void) { return 0; }   // no outputs to choose: the Game tab has no audio row (overlay_layout.h ov_menu_item)
int dm_menu_rate_available(void) { return 0; } int dm_menu_res_available(void) { return 0; } int dm_menu_detail_available(void) { return 0; } int dm_menu_detail_step(void) { return 0; }   // a game without the profile's rows
static int exit_state; static float exit_f;   // front_android.c: Exit game's hold (overlay_layout.h OV_EXIT_*)
int dm_menu_exit(float *f, int *chosen) { *f = exit_f; *chosen = 0; return exit_state; }
''' + canvas + colours + r'''
typedef struct { char s[64]; float x, cy; int scale; } drawn;   // each text the panel draws
static drawn g_drawn[64]; static int g_ndrawn;
static void rec_text(const ov_canvas *cv, float x, float cy, const char *s, int scale, const uint8_t c[4]) {
    if (g_ndrawn < 64) { drawn *d = &g_drawn[g_ndrawn++]; snprintf(d->s, sizeof d->s, "%s", s); d->x = x; d->cy = cy; d->scale = scale; }
    cv_text(cv, x, cy, s, scale, c);
}
#define cv_text rec_text
''' + panel + PANEL_MAIN
        self._run(source, FONT)

    def test_exit_held(self):
        # Exit game ends the game only when held 1.5 s (overlay_layout.h ov_exit_hold, which front_android.c's touch and A run): a tap shows the hint, a finger
        # off the half lets go
        source = r'''
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "overlay_layout.h"
float dm_overlay_scale(void) { return 1; }
float dm_menu_row_need(float rh) { (void)rh; return 0; }
int dm_menu_audio_available(void) { return 0; } int dm_menu_rate_available(void) { return 0; } int dm_menu_res_available(void) { return 0; } int dm_menu_detail_available(void) { return 0; }
''' + HOLD_MAIN
        self._run(source, None)

if __name__ == '__main__':
    unittest.main()
