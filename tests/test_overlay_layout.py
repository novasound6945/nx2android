"""Check the Android layout editor's geometry without a device."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from porter.upstream import android_profile


DM = Path(__file__).resolve().parents[1] / 'nxnative/dm'


class OverlayLayout(unittest.TestCase):
    def test_grid_and_toolbar(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            self.skipTest('a C compiler is needed for the layout geometry check')
        harness = r'''
#include <assert.h>
#include "overlay_layout.h"
static int custom_faces, custom_a, custom_x;
float dm_overlay_scale(void) { return 1; }
int dm_overlay_custom(int layout, int ctl, float *x, float *y, float *s) {
    *s = 1;
    if (layout == 1 && custom_faces && (ctl == custom_a || ctl == custom_x)) {
        *s = custom_faces == 2 ? 0.5f : 1.2f;
        *x = ctl == custom_a ? 0.70f : (custom_faces == 3 ? 0.40f : custom_faces == 2 ? 0.675f : 0.65f);
        *y = 0.45f; return 1;
    }
    if (layout != 1 || ctl != 0) return 0;
    *x = 0.1125f; *y = 0.55f; *s = 0.705f; return 1;
}
int main(void) {
    assert(ov_layout_for_size(2400, 1080) == 1);
    assert(ov_layout_for_size(1920, 1080) == 0);
    assert(ov_layout_for_size(0, 0) == 0);
    const ov_ctl *phone_layout = OV_LAYOUTS[1];
    int ia = -1, ix = -1, ib = -1, iy = -1;
    for (int i = 0; i < OV_N; i++) {
        if (phone_layout[i].btn == DM_BTN_A) ia = i;
        if (phone_layout[i].btn == DM_BTN_X) ix = i;
        if (phone_layout[i].btn == DM_BTN_B) ib = i;
        if (phone_layout[i].btn == DM_BTN_Y) iy = i;
    }
    assert(ia >= 0 && ix >= 0 && ib >= 0 && iy >= 0);
    float a[4], x[4], b[4], y[4];
    ov_box(&phone_layout[ia], 2400, 1080, a); ov_box(&phone_layout[ix], 2400, 1080, x);
    ov_box(&phone_layout[ib], 2400, 1080, b); ov_box(&phone_layout[iy], 2400, 1080, y);
    assert(!ov_face_combo(phone_layout, a[0], a[1], 2400, 1080));
    assert(!ov_face_combo(phone_layout, x[0], x[1], 2400, 1080));
    assert(ov_face_combo(phone_layout, (a[0] + x[0]) / 2, (a[1] + x[1]) / 2, 2400, 1080) == (DM_BTN_A | DM_BTN_X));
    assert(ov_button_bits(phone_layout, &phone_layout[ix], x[0], x[1], 2400, 1080) == DM_BTN_X);
    assert(ov_button_bits(phone_layout, &phone_layout[ix], (a[0] + x[0]) / 2, (a[1] + x[1]) / 2, 2400, 1080) == (DM_BTN_A | DM_BTN_X));
    assert(ov_button_bits(phone_layout, &phone_layout[ib], (a[0] + x[0]) / 2, (a[1] + x[1]) / 2, 2400, 1080) == DM_BTN_B);
    assert(ov_face_combo(phone_layout, (a[0] + b[0]) / 2, (a[1] + b[1]) / 2, 2400, 1080) == (DM_BTN_A | DM_BTN_B));
    assert(!ov_face_combo(phone_layout, (a[0] + y[0]) / 2, (a[1] + y[1]) / 2, 2400, 1080));
    custom_a = ia; custom_x = ix;
    for (custom_faces = 1; custom_faces <= 3; custom_faces++) {
        ov_box(&phone_layout[ia], 2400, 1080, a); ov_box(&phone_layout[ix], 2400, 1080, x);
        uint64_t pair = ov_face_combo(phone_layout, (a[0] + x[0]) / 2, (a[1] + x[1]) / 2, 2400, 1080);
        assert(pair == (custom_faces == 3 ? 0 : (DM_BTN_A | DM_BTN_X)));
        assert(!ov_face_combo(phone_layout, a[0], a[1], 2400, 1080));
        assert(!ov_face_combo(phone_layout, x[0], x[1], 2400, 1080));
    }
    custom_faces = 0;
    // The saved phone layout must be evaluated against the phone's dimensions,
    // even if a 16:9 external screen is attached.
    float phone[4], external[4];
    ov_box(&OV_LAYOUTS[1][0], 2400, 1080, phone);
    ov_box(&OV_LAYOUTS[1][0], 1920, 1080, external);
    assert(fabsf(phone[0] - 270) < 0.01f && fabsf(phone[1] - 594) < 0.01f);
    assert(fabsf(external[0] - 216) < 0.01f);
    assert(fabsf(phone[2] - OV_LAYOUTS[1][0].w * 1080 * 0.705f / 2) < 0.01f);
    assert(ov_drag_pos(126, 30, 1000, 50) == 150);
    assert(ov_drag_pos(124, 30, 1000, 50) == 100);
    assert(ov_drag_pos(-100, 65, 1000, 50) == 100);
    assert(ov_drag_pos(1100, 65, 1000, 50) == 900);
    assert(ov_drag_pos(126, 30, 1000, 0) == 126);
    assert(ov_drag_pos(1100, 65, 1000, 0) == 935);
    assert(ov_drag_pos(1100, 600, 1013, 50) == 506.5f);
    assert(ov_drag_pos(1100, 505, 1013, 50) == 508);

    // the Gamepad tab's last row is the face buttons' (front_android.c pad_labels); the Game tab's: the frame rate counter, the mode, the frame rate limit under
    // it (front_android.c rate_apply), the save's export and import
    assert(ov_menu_item(0, OVM_ROWS - 2) == OVM_PAD && ov_menu_item(1, OVM_ROWS - 1) == OVM_RESUME);
    assert(ov_menu_item(1, 1) == OVM_FPS && ov_menu_item(1, 2) == OVM_MODE && ov_menu_item(1, 3) == OVM_RATE && ov_menu_item(1, OVM_ROWS - 2) == OVM_IMPORT);
    {   // every item has one row in one tab, before Resume (gfx_vk_present.c ov_menu_panel sizes the letters by the items before OVM_RESUME)
        int seen[OVM_N] = {0};
        for (int t = 0; t < OVM_TABS; t++) for (int row = 1; row < OVM_ROWS - 1; row++) { int it = ov_menu_item(t, row); if (it >= 0) { assert(it < OVM_RESUME); seen[it]++; } }
        for (int it = 0; it < OVM_RESUME; it++) assert(seen[it] == 1);
    }
    assert(OVM_RATES == 4 && ((OVM_RATES - 1) << 29) > 0 && !(((OVM_RATES - 1) << 29) & (1 << 28)));   // dm_menu_state's bits 29..30: clear of bit 28 and of the sign
    const float screens[][2] = {{915, 412}, {1216, 760}, {2400, 1080}, {1080, 2400}};
    for (unsigned s = 0; s < sizeof screens / sizeof screens[0]; s++) {
        float W = screens[s][0], H = screens[s][1], step = H / OVE_GRID_ROWS, p[4];
        ov_menu_rect(W, H, p);
        for (int row = 0; row < OVM_ROWS; row++) {
            float r[4]; ov_menu_rowrect(p, row, r);
            assert(ov_menu_row_at(r[0] + r[2] / 2, r[1] + r[3] / 2, W, H) == row);
        }
        for (int axis = 0; axis < 2; axis++) for (int i = -10; i <= 110; i++) {
            float extent = screens[s][axis], half = H * 0.12f;
            float pos = ov_drag_pos(extent * i / 100, half, extent, step);
            assert(pos >= half - 0.001f && pos <= extent - half + 0.001f);
            assert(fabsf(pos / step - roundf(pos / step)) < 0.001f);
        }
        ov_ed_rect(W, H, p);
        for (int i = 0; i < OVE_ALL; i++) {
            if (i == OVE_DISCARD) ov_ed_prompt_rect(W, H, p);
            float b[4]; ov_ed_button(i, p, b);
            assert(b[0] >= p[0] && b[0] + b[2] <= p[0] + p[2]);
            assert(b[1] >= p[1] && b[1] + b[3] <= p[1] + p[3]);
            int first = i < OVE_N ? 0 : OVE_DISCARD, n = i < OVE_N ? OVE_N : 2;
            assert(ov_ed_button_at(b[0] + b[2] / 2, b[1] + b[3] / 2, p, first, n) == i);
        }
    }
    return 0;
}
'''
        self._run(cc, harness)

    def test_text_entry_keyboard(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            self.skipTest('a C compiler is needed for the keyboard geometry check')
        harness = r'''
#include <assert.h>
#include "overlay_layout.h"
float dm_overlay_scale(void) { return 1; }
int dm_overlay_custom(int layout, int ctl, float *x, float *y, float *s) { (void)layout; (void)ctl; (void)x; (void)y; *s = 1; return 0; }
int main(void) {
    assert(OVK_N == 45 && OVK_SHIFT == 40 && OVK_LANG == 41 && OVK_SPACE == 42 && OVK_DEL == 43 && OVK_OK == 44);
    const float panels[][4] = {{0, 0, 915, 412}, {100, 50, 1216, 760}, {0, 0, 2400, 1080}, {30, 400, 1020, 900}};
    for (unsigned q = 0; q < sizeof panels / sizeof panels[0]; q++) {
        const float *p = panels[q];
        for (int i = 0; i < OVK_N; i++) {   // inside the panel's key area, no overlaps, and the touch test finds each key at its centre
            float r[4]; ov_kb_key(i, p, r);
            assert(r[2] > 0 && r[3] > 0 && r[0] >= p[0] && r[1] >= p[1] + p[3] * 0.30f && r[0] + r[2] <= p[0] + p[2] && r[1] + r[3] <= p[1] + p[3] * 0.90f + 0.01f);
            assert(ov_kb_at(r[0] + r[2] / 2, r[1] + r[3] / 2, p) == i);
            for (int j = 0; j < i; j++) { float o[4]; ov_kb_key(j, p, o); assert(r[0] >= o[0] + o[2] || o[0] >= r[0] + r[2] || r[1] >= o[1] + o[3] || o[1] >= r[1] + r[3]); }
        }
        assert(ov_kb_at(p[0] - 1, p[1] - 1, p) == -1);
    }
    for (int i = 0; i < OVK_N; i++) {   // the d-pad: a key from every key in every direction; a row's width of moves left or right comes back, and five up or down
        int row = i < 40 ? 10 : 5;   // from a wide key (a character key's column can change on the wide row: two columns share a wide key)
        for (int d = -1; d <= 1; d += 2) {
            int k = ov_kb_move(i, d, 0), m = ov_kb_move(i, 0, d); assert(k >= 0 && k < OVK_N && m >= 0 && m < OVK_N);
            assert((k >= 40) == (i >= 40));
            k = i; for (int s = 0; s < row; s++) k = ov_kb_move(k, d, 0); assert(k == i);
            k = i; for (int s = 0; s < 5; s++) k = ov_kb_move(k, 0, d); assert(i < 40 || k == i);
            if (i < 40 && (i % 10 == 0 || i % 10 == 2 || i % 10 == 4 || i % 10 == 7 || i % 10 == 9)) assert(k == i);
            assert(ov_kb_move(ov_kb_move(i, d, 0), -d, 0) == i);
        }
    }
    assert(ov_kb_move(30, 0, 1) == OVK_SHIFT && ov_kb_move(32, 0, 1) == OVK_LANG && ov_kb_move(35, 0, 1) == OVK_SPACE && ov_kb_move(37, 0, 1) == OVK_DEL && ov_kb_move(39, 0, 1) == OVK_OK);
    assert(ov_kb_move(OVK_OK, 1, 0) == OVK_SHIFT && ov_kb_move(OVK_SHIFT, -1, 0) == OVK_OK && ov_kb_move(OVK_SHIFT, 0, -1) == 30 && ov_kb_move(OVK_LANG, 0, 1) == 2);
    assert(ov_kb_move(-1, 1, 0) == 0);
    // the characters: English as before, the Hangul layout and its shift
    assert(ov_kb_char(10, 0, 0) == 'q' && ov_kb_char(11, 1, 0) == 'W' && ov_kb_char(0, 1, 0) == '1' && ov_kb_char(29, 1, 0) == '\'' && ov_kb_char(OVK_SPACE, 0, 0) == 0 && ov_kb_char(-1, 0, 1) == 0);
    assert(ov_kb_char(0, 0, 1) == '1' && ov_kb_char(9, 1, 1) == '0');
    static const uint32_t ko[30] = {0x3142, 0x3148, 0x3137, 0x3131, 0x3145, 0x315B, 0x3155, 0x3151, 0x3150, 0x3154, 0x3141, 0x3134, 0x3147, 0x3139, 0x314E, 0x3157, 0x3153, 0x314F, 0x3163, '\'',
                                    0x314B, 0x314C, 0x314A, 0x314D, 0x3160, 0x315C, 0x3161, '-', '.', '_'};
    static const uint32_t ko_shift[30] = {0x3143, 0x3149, 0x3138, 0x3132, 0x3146, 0x315B, 0x3155, 0x3151, 0x3152, 0x3156, 0x3141, 0x3134, 0x3147, 0x3139, 0x314E, 0x3157, 0x3153, 0x314F, 0x3163, '\'',
                                          0x314B, 0x314C, 0x314A, 0x314D, 0x3160, 0x315C, 0x3161, '-', '.', '_'};
    for (int i = 0; i < 30; i++) { assert(ov_kb_char(10 + i, 0, 1) == ko[i]); assert(ov_kb_char(10 + i, 1, 1) == ko_shift[i]); }
    for (int i = 40; i < OVK_N; i++) assert(ov_kb_char(i, 1, 1) == 0);
    assert(ov_kb_find('q') == 10 && ov_kb_find('m') == 36 && ov_kb_find('1') == 0 && ov_kb_find('!') == -1 && ov_kb_char(ov_kb_find('r'), 1, 1) == 0x3132);   // a physical r with shift: kk
    return 0;
}
'''
        self._run(cc, harness)

    def _run(self, cc, harness):
        with tempfile.TemporaryDirectory(prefix='nx2a_overlay_') as tmp:
            tmp = Path(tmp)
            (tmp / 'android_profile.h').write_text(android_profile.header(None), encoding='utf-8')
            c, exe = tmp / 'check.c', tmp / 'check'
            c.write_text(harness, encoding='utf-8')
            result = subprocess.run([cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', f'-I{tmp}',
                                     f'-I{DM / "android"}', f'-I{DM / "host"}', str(c), '-lm', '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
