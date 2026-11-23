// This fork's drawing in gfx_vk_present.c (upstream's file), included there once, before ov_menu_panel: the pause menu's Exit game beside Resume (menu_exit.h,
// front_android.c's hold) and the cloud sync's corner marks (savecloud_android.c dm_cloud_mark). Upstream's code calls fork_* in one line where these ran before
#include "menu_exit.h"
#include "savecloud.h"
__attribute__((weak)) int dm_menu_exit(float *f, int *chosen) { *f = 0; *chosen = 0; return OV_EXIT_IDLE; }   // front_android.c: Exit game's hold
__attribute__((weak)) unsigned dm_menu_exit_ver(void) { return 0; }
__attribute__((weak)) int dm_cloud_mark(int64_t *age_ms) { (void)age_ms; return 0; }   // savecloud_android.c: the corner note (savecloud.h)
static uint32_t ov_tex_rows(void);   // (gfx_vk_present.c, below)
static void ov_rect(VkCommandBuffer cb, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, float alpha);

#include "menu_exit_draw.inc"   // the button row: Exit game | Resume

// The cloud sync's corner mark (savecloud_android.c dm_cloud_mark), icons only: rasterized once with the texture into the rows under the counter's characters, at
// the size they are drawn (OV_MARK_H of the window's height, one texel a window pixel): OV_SPIN frames of a turning arc and the cloud struck through and the orange
// "!", each on a dark disc, and the check: a white V on a green disc (the disc itself is its background). Drawn in the top right corner, OV_MARK_M of the height from
// both edges, while a mark shows (not under the menu); with none, nothing is drawn and the overlay pass is not even begun for it
#define OV_MARK_H 0.045f
#define OV_MARK_M 0.03f
#define OV_SPIN 12
enum { MK_CHECK = OV_SPIN, MK_OFFLINE, MK_WARN, MK_N };
static int g_mark; static int64_t g_mark_age;
static void cv_seg(const ov_canvas *cv, float ax, float ay, float bx, float by, float w, const uint8_t c[4]) {   // a stroke with round ends
    float ex = bx - ax, ey = by - ay, l2 = ex * ex + ey * ey, r = w / 2;
    for (int y = (int)(fminf(ay, by) - r) - 1; y <= (int)(fmaxf(ay, by) + r) + 1; y++) for (int x = (int)(fminf(ax, bx) - r) - 1; x <= (int)(fmaxf(ax, bx) + r) + 1; x++) {
        float px = x + 0.5f - ax, py = y + 0.5f - ay, t = l2 > 0 ? fminf(1, fmaxf(0, (px * ex + py * ey) / l2)) : 0, dx = px - t * ex, dy = py - t * ey;
        cv_blend(cv, x, y, c, 0.5f - (sqrtf(dx * dx + dy * dy) - r));
    }
}
static void cv_poly(const ov_canvas *cv, const float *p, int n, float ox, float oy, float k, float w, const uint8_t c[4]) {   // a polyline in a 24-unit box (the start screen's SVG icons)
    // (drawn as one coverage: each texel takes the nearest segment, so the joints are not drawn twice)
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, r = w * k / 2;
    for (int i = 0; i < n; i++) { float X = ox + p[2 * i] * k, Y = oy + p[2 * i + 1] * k; x0 = fminf(x0, X); x1 = fmaxf(x1, X); y0 = fminf(y0, Y); y1 = fmaxf(y1, Y); }
    for (int y = (int)(y0 - r) - 1; y <= (int)(y1 + r) + 1; y++) for (int x = (int)(x0 - r) - 1; x <= (int)(x1 + r) + 1; x++) {
        float best = 1e9f;
        for (int i = 0; i + 1 < n; i++) {
            float ax = ox + p[2 * i] * k, ay = oy + p[2 * i + 1] * k, ex = ox + p[2 * i + 2] * k - ax, ey = oy + p[2 * i + 3] * k - ay, l2 = ex * ex + ey * ey;
            float px = x + 0.5f - ax, py = y + 0.5f - ay, t = l2 > 0 ? fminf(1, fmaxf(0, (px * ex + py * ey) / l2)) : 0, dx = px - t * ex, dy = py - t * ey; best = fminf(best, dx * dx + dy * dy);
        }
        cv_blend(cv, x, y, c, 0.5f - (sqrtf(best) - r));
    }
}
static void ov_mark_cell(int i, float *u, float *v) { *u = (float)(i % g_ov.mcols * g_ov.mg); *v = (float)(g_ov.h + g_ov.sh + g_ov.ph + g_ov.gh + i / g_ov.mcols * g_ov.mg); }
static void ov_mark_glyphs(void) {
    if (!g_ov.mcols) return;   // (a texture too narrow: no marks)
    float g = (float)g_ov.mg; ov_canvas cv = {g_ov.px, g_ov.w, 0, 0, g_ov.w, ov_tex_rows()};
    static const uint8_t disc[4] = {15, 17, 21, 150}, ink[4] = {232, 234, 237, 255}, green[4] = {52, 199, 89, 255}, white[4] = {255, 255, 255, 255}, grey[4] = {195, 200, 208, 255},
                         orange[4] = {224, 164, 88, 255};
    static const float check[] = {5.6f, 12.4f, 10, 16.8f, 18.6f, 7.6f}, slash[] = {4, 4, 20, 20}, bar[] = {12, 7.5f, 12, 13}, dot[] = {12, 16.5f, 12, 16.6f};
    static const float cloud[] = {7, 18, 17, 18, 18.17f, 17.83f, 19.24f, 17.32f, 20.11f, 16.52f, 20.72f, 15.5f, 20.99f, 14.35f, 20.92f, 13.17f, 20.51f, 12.06f, 19.79f, 11.12f, 18.82f, 10.43f,
        17.7f, 10.06f, 17.43f, 9.13f, 17.01f, 8.26f, 16.46f, 7.47f, 15.78f, 6.77f, 15.01f, 6.2f, 14.15f, 5.75f, 13.23f, 5.45f, 12.27f, 5.3f, 11.31f, 5.31f, 10.35f, 5.47f, 9.44f, 5.78f,
        8.59f, 6.24f, 7.82f, 6.82f, 7.15f, 7.52f, 6.61f, 8.32f, 6.2f, 9.2f, 5.25f, 9.6f, 4.41f, 10.2f, 3.73f, 10.98f, 3.25f, 11.9f, 2.99f, 12.9f, 2.97f, 13.93f, 3.18f, 14.94f, 3.62f, 15.88f,
        4.25f, 16.69f, 5.06f, 17.33f, 5.99f, 17.78f, 7, 18};   // M7 18h10a4 4 0 0 0 .7-7.94A6 6 0 0 0 6.2 9.2 4.5 4.5 0 0 0 7 18z
    for (int i = 0; i < MK_N; i++) {
        float x0, y0; ov_mark_cell(i, &x0, &y0); float k = g * 0.62f / 24, ox = x0 + g * 0.19f, oy = y0 + g * 0.19f;   // the icon in the middle 62 % of the disc
        cv_disc(&cv, x0 + g / 2, y0 + g / 2, g / 2 - 1, i == MK_CHECK ? green : disc);
        if (i < OV_SPIN) { float p[2 * 25], a0 = 6.2832f * i / OV_SPIN;   // M12 3a9 9 0 1 0 9 9: three quarters of a circle, turned a step each frame
            for (int j = 0; j < 25; j++) { float a = a0 - 1.5708f + 4.7124f * j / 24; p[2 * j] = 12 + 8.5f * cosf(a); p[2 * j + 1] = 12 + 8.5f * sinf(a); }
            cv_poly(&cv, p, 25, ox, oy, k, 2.6f, ink); }
        else if (i == MK_CHECK) cv_poly(&cv, check, 3, ox, oy, k, 3.2f, white);
        else if (i == MK_OFFLINE) { cv_poly(&cv, cloud, (int)(sizeof cloud / sizeof *cloud / 2), ox, oy, k, 1.9f, grey); cv_poly(&cv, slash, 2, ox, oy, k, 1.9f, grey); }
        else { cv_ring(&cv, ox + 12 * k, oy + 12 * k, 9.5f * k, 2.2f * k, orange); cv_poly(&cv, bar, 2, ox, oy, k, 2.4f, orange); cv_poly(&cv, dot, 2, ox, oy, k, 2.6f, orange); }
    }
}
static void ov_mark(VkCommandBuffer cb, int mark, int64_t age) {
    if (!g_ov.mcols) return;
    float H = (float)g_sext.height, sz = H * OV_MARK_H, m = H * OV_MARK_M, cx = (float)g_sext.width - m - sz / 2, cy = m + sz / 2, a = 1, k = 1; int cell;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    if (mark == DM_CLOUD_MARK_BUSY) cell = (int)((ts.tv_sec * 1000 + ts.tv_nsec / 1000000) / 83 % OV_SPIN);   // a turn a second
    else {
        int life = mark == DM_CLOUD_MARK_SAVED ? DM_CLOUD_MARK_SAVED_MS : mark == DM_CLOUD_MARK_OFFLINE ? DM_CLOUD_MARK_OFFLINE_MS : DM_CLOUD_MARK_CONFLICT_MS;
        cell = mark == DM_CLOUD_MARK_SAVED ? MK_CHECK : mark == DM_CLOUD_MARK_OFFLINE ? MK_OFFLINE : MK_WARN;
        if (age < 200) k = 1 + 0.22f * sinf(3.1416f * age / 200.0f);   // the pop: a little bigger and back, 0.2 s
        if (age > life - 300) a = fmaxf(0, (life - age) / 300.0f);       // the fade, its last 0.3 s
    }
    float g = (float)g_ov.mg, u0, v0, r = g * k / 2; ov_mark_cell(cell, &u0, &v0);   // (the cell's size: sz, a texel a pixel)
    ov_rect(cb, cx - r, cy - r, cx + r, cy + r, u0, v0, u0 + g, v0 + g, a);
}
static void fork_marks_size(uint32_t w) {   // overlay_ready, the texture made again for a window: the marks' cells (a texture too narrow: none)
    g_ov.mg = (uint32_t)ceilf((float)g_sext.height * OV_MARK_H); g_ov.mcols = g_ov.mg ? w / g_ov.mg : 0; if (g_ov.mcols > MK_N) g_ov.mcols = MK_N;
    g_ov.mrows = g_ov.mcols ? (MK_N + g_ov.mcols - 1) / g_ov.mcols : 0;
}
static void fork_mark_poll(int external) { g_mark = external ? 0 : dm_cloud_mark(&g_mark_age); }   // present_window: the mark this present shows (none: nothing of it is drawn)
static void fork_draw_marks(VkCommandBuffer cb, int menu) { if (g_mark && menu < 0) ov_mark(cb, g_mark, g_mark_age); }   // ov_render, last: not under the menu
