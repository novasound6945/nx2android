// The start screen's drawing (startscreen.h): the design's px are for a 1280 px wide screen and scale with the window, drawn 1.3 times as big as the mock-ups
// (s = 1.3 min(w / 1280, h / 720): 1.95 on a 1920 x 1080 Retroid Pocket 6, 1.3 on a 1280 x 960 Retroid Pocket Nova; a page too tall for that at 16:9 is drawn a little
// smaller, 1 at least). The margins and the widths follow the window where the mock-ups' did not fit. A screen narrower than 3:2 stacks the game's name over the steps; a
// wider one puts them side by side. Every line of text stays one line: one too long for its place is drawn smaller (to 80 %), then cut with an ellipsis (a game's name
// may break at its spaces, two lines at most)
#include "startscreen.h"
#include "pixel_font.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wcast-qual"
#pragma clang diagnostic ignored "-Wimplicit-fallthrough"
#endif
#include "../third_party/stb/stb_truetype.h"   // v1.28, public domain (or MIT): github.com/nothings/stb
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wcast-qual"
#pragma clang diagnostic ignored "-Wmissing-prototypes"
#endif
#include "../third_party/qrcodegen/qrcodegen.c"   // v1.8.0, MIT: github.com/nayuki/QR-Code-generator (the device link's QR code)
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

// ---- the design's colours and Noto Sans KR's vertical metrics (hhea: ascent 1.160 em, descent 0.288 em, a line of "normal" height 1.448 em) ----
#define BG 0x0F1115u
#define INK 0xE8EAEDu
#define DIM 0x9AA1ACu
#define DIM2 0x8A919Cu
#define BODY 0xC3C8D0u
#define ACC 0x5B9BFFu
#define OK_GREEN 0x6FCF97u
#define DOT 0x5A6170u
#define WARN 0xE0A458u
#define TRACK 0x252B36u
#define CARD 0x1A1E26u
#define CARD_SEL 0x1E2633u
#define EDGE 0x2A3140u
#define ASC 1.160f
#define LHN 1.448f

// ---- fonts and glyphs ----
static struct { stbtt_fontinfo f[2]; int ok[2]; const uint8_t *pf; } F;
typedef struct { uint32_t cp, fs; int used, w, h, x0, y0; float adv; uint8_t *a; } glyph;
#define GCAP 4096
static glyph *GC; static int GN;
int stv_font_set(int bold, const uint8_t *ttf, size_t n) {
    bold = !!bold; F.ok[bold] = 0;
    if (!ttf || n < 12) return -1;
    int off = stbtt_GetFontOffsetForIndex(ttf, 0);
    F.ok[bold] = off >= 0 && stbtt_InitFont(&F.f[bold], ttf, off);
    return F.ok[bold] ? 0 : -1;
}
void stv_font_fallback(const uint8_t *pack) { F.pf = pack; }
static void glyphs_flush(void) { if (GC) for (int i = 0; i < GCAP; i++) free(GC[i].a); free(GC); GC = NULL; GN = 0; }
static void bg_free(void);
void stv_release(void) { glyphs_flush(); bg_free(); memset(&F, 0, sizeof F); }
static const glyph *glyph_of(int bold, float size, uint32_t cp) {
    if (!GC || GN > GCAP * 3 / 4) { glyphs_flush(); GC = calloc(GCAP, sizeof *GC); if (!GC) return NULL; }
    uint32_t fs = (uint32_t)bold << 24 | (uint32_t)(size * 8 + 0.5f);
    uint32_t h = (cp * 2654435761u ^ fs * 40503u) & (GCAP - 1);
    while (GC[h].used) { if (GC[h].cp == cp && GC[h].fs == fs) return &GC[h]; h = (h + 1) & (GCAP - 1); }
    glyph *g = &GC[h]; g->used = 1; g->cp = cp; g->fs = fs; GN++;
    int fi = F.ok[bold] ? bold : F.ok[0] ? 0 : -1, gi = fi >= 0 ? stbtt_FindGlyphIndex(&F.f[fi], (int)cp) : 0;
    if (gi) {
        const stbtt_fontinfo *f = &F.f[fi]; float sc = stbtt_ScaleForMappingEmToPixels(f, size); int adv, lsb, x0, y0, x1, y1;
        stbtt_GetGlyphHMetrics(f, gi, &adv, &lsb); stbtt_GetGlyphBitmapBox(f, gi, sc, sc, &x0, &y0, &x1, &y1);
        g->adv = adv * sc; g->x0 = x0; g->y0 = y0; g->w = x1 - x0; g->h = y1 - y0;
        if (g->w > 0 && g->h > 0 && (g->a = malloc((size_t)g->w * g->h))) stbtt_MakeGlyphBitmap(f, g->a, g->w, g->h, g->w, sc, sc, gi);
        else g->w = g->h = 0;
        return g;
    }
    const pf_index *p = pf_find(F.pf, cp);   // what the font lacks (a game's name in Japanese): the pixel font, its capitals as tall as the font's (0.73 em)
    if (!p) { g->adv = size * 0.5f; return g; }
    uint32_t n; memcpy(&n, F.pf + 4, 4); const uint8_t *bits = F.pf + 8 + (size_t)n * sizeof(pf_index) + (p->off_w >> 8);
    int pw = p->off_w & 255, stride = (pw + 7) / 8; float k = size * 0.73f / PF_CAP;
    g->adv = (p->cp_adv & 255) * k; g->x0 = (int)lroundf(p->x * k); g->y0 = (int)lroundf(-(p->h + p->y) * k);
    g->w = (int)ceilf(pw * k); g->h = (int)ceilf(p->h * k);
    if (g->w <= 0 || g->h <= 0 || !(g->a = malloc((size_t)g->w * g->h))) { g->w = g->h = 0; return g; }
    for (int y = 0; y < g->h; y++) for (int x = 0; x < g->w; x++) { int sx = (int)(x / k), sy = (int)(y / k);
        g->a[y * g->w + x] = sx < pw && sy < p->h && (bits[sy * stride + sx / 8] >> (7 - sx % 8) & 1) ? 255 : 0; }
    return g;
}

// ---- pixels ----
static inline void blend(uint32_t *p, uint32_t rgb, int a) {   // a 0..255
    if (a <= 0) return;
    uint32_t d = *p; int r = rgb >> 16 & 255, g = rgb >> 8 & 255, b = rgb & 255, dr = d & 255, dg = d >> 8 & 255, db = d >> 16 & 255;
    if (a >= 255) { dr = r; dg = g; db = b; }
    else { dr += (r - dr) * a / 255; dg += (g - dg) * a / 255; db += (b - db) * a / 255; }
    *p = 0xff000000u | (uint32_t)db << 16 | (uint32_t)dg << 8 | (uint32_t)dr;
}
static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
// a rounded rectangle, its edges smoothed; drawn only left of xe (a held button's filled part: clipped by the button's own shape)
static void rrect_clip(stv_canvas *c, float x, float y, float w, float h, float r, uint32_t rgb, float alpha, float xe) {
    if (w <= 0 || h <= 0 || xe <= x) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    int x0 = (int)floorf(x), x1 = (int)ceilf(x + w), y0 = (int)floorf(y), y1 = (int)ceilf(y + h);
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > c->w) x1 = c->w; if (y1 > c->h) y1 = c->h; if (xe < x1) x1 = (int)ceilf(xe);
    float cx = x + w / 2, cy = y + h / 2, hw = w / 2 - r, hh = h / 2 - r;
    for (int j = y0; j < y1; j++) {
        float dy = fabsf(j + 0.5f - cy) - hh; uint32_t *row = c->px + (size_t)j * c->w;
        for (int i = x0; i < x1; i++) {
            float dx = fabsf(i + 0.5f - cx) - hw, ox = dx > 0 ? dx : 0, oy = dy > 0 ? dy : 0;
            float d = sqrtf(ox * ox + oy * oy) + (dx > dy ? (dx < 0 ? dx : 0) : (dy < 0 ? dy : 0)) - r;
            float cov = clampf(0.5f - d, 0, 1) * clampf(xe - i, 0, 1); if (cov > 0) blend(row + i, rgb, (int)(cov * alpha * 255 + 0.5f));
        }
    }
}
static void rrect(stv_canvas *c, float x, float y, float w, float h, float r, uint32_t rgb, float alpha) { rrect_clip(c, x, y, w, h, r, rgb, alpha, 1e9f); }
// a stroked polyline in a 24-unit icon box at (ox, oy), k px a unit (the design's SVG icons), round caps and joins
static void stroke(stv_canvas *c, const float *pts, int n, int closed, float ox, float oy, float k, float width, uint32_t rgb, float alpha) {
    float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f, hw = width * k / 2;
    for (int i = 0; i < n; i++) { float X = ox + pts[2 * i] * k, Y = oy + pts[2 * i + 1] * k; if (X < bx0) bx0 = X; if (X > bx1) bx1 = X; if (Y < by0) by0 = Y; if (Y > by1) by1 = Y; }
    int x0 = (int)floorf(bx0 - hw - 1), x1 = (int)ceilf(bx1 + hw + 1), y0 = (int)floorf(by0 - hw - 1), y1 = (int)ceilf(by1 + hw + 1);
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > c->w) x1 = c->w; if (y1 > c->h) y1 = c->h;
    int segs = closed ? n : n - 1;
    for (int j = y0; j < y1; j++) for (int i = x0; i < x1; i++) {
        float px = i + 0.5f, py = j + 0.5f, best = 1e9f;
        for (int s = 0; s < segs; s++) {
            int e = (s + 1) % n; float ax = ox + pts[2 * s] * k, ay = oy + pts[2 * s + 1] * k, ex = ox + pts[2 * e] * k - ax, ey = oy + pts[2 * e + 1] * k - ay;
            float l2 = ex * ex + ey * ey, t = l2 > 0 ? clampf(((px - ax) * ex + (py - ay) * ey) / l2, 0, 1) : 0, dx = px - ax - t * ex, dy = py - ay - t * ey, d = dx * dx + dy * dy;
            if (d < best) best = d;
        }
        float cov = clampf(hw - sqrtf(best) + 0.5f, 0, 1); if (cov > 0) blend(c->px + (size_t)j * c->w + i, rgb, (int)(cov * alpha * 255 + 0.5f));
    }
}
static void arc(stv_canvas *c, float cx, float cy, float r, float a0, float a1, float ox, float oy, float k, float width, uint32_t rgb) {   // in icon units
    float p[2 * 49]; int n = 49;
    for (int i = 0; i < n; i++) { float a = a0 + (a1 - a0) * i / (n - 1); p[2 * i] = cx + r * cosf(a); p[2 * i + 1] = cy + r * sinf(a); }
    stroke(c, p, n, 0, ox, oy, k, width, rgb, 1);
}
static void image(stv_canvas *c, const uint32_t *src, int sw, int sh, float x, float y, float size, float r) {   // the icon, scaled (bilinear, 2 x 2 samples a pixel) into a rounded square
    int x0 = (int)floorf(x), y0 = (int)floorf(y), n = (int)ceilf(size) + 1; float hw = size / 2 - r, cx = x + size / 2, cy = y + size / 2;
    for (int j = 0; j < n; j++) for (int i = 0; i < n; i++) {
        int X = x0 + i, Y = y0 + j; if (X < 0 || Y < 0 || X >= c->w || Y >= c->h) continue;
        float dx = fabsf(X + 0.5f - cx) - hw, dy = fabsf(Y + 0.5f - cy) - hw, ox = dx > 0 ? dx : 0, oy = dy > 0 ? dy : 0;
        float cov = clampf(0.5f - (sqrtf(ox * ox + oy * oy) + (dx > dy ? (dx < 0 ? dx : 0) : (dy < 0 ? dy : 0)) - r), 0, 1); if (cov <= 0) continue;
        float acc[4] = {0};
        for (int q = 0; q < 4; q++) {
            float u = ((X + 0.25f + 0.5f * (q & 1)) - x) / size * sw - 0.5f, v = ((Y + 0.25f + 0.5f * (q >> 1)) - y) / size * sh - 0.5f;
            int u0 = (int)floorf(u), v0 = (int)floorf(v); float fu = u - u0, fv = v - v0;
            for (int t = 0; t < 4; t++) { int uu = u0 + (t & 1), vv = v0 + (t >> 1); if (uu < 0) uu = 0; if (vv < 0) vv = 0; if (uu >= sw) uu = sw - 1; if (vv >= sh) vv = sh - 1;
                float wgt = ((t & 1) ? fu : 1 - fu) * ((t >> 1) ? fv : 1 - fv); uint32_t p = src[(size_t)vv * sw + uu];
                acc[0] += wgt * (p & 255); acc[1] += wgt * (p >> 8 & 255); acc[2] += wgt * (p >> 16 & 255); acc[3] += wgt * (p >> 24); }
        }
        uint32_t rgb = (uint32_t)(acc[0] / 4) << 16 | (uint32_t)(acc[1] / 4) << 8 | (uint32_t)(acc[2] / 4);
        blend(c->px + (size_t)Y * c->w + X, rgb, (int)(cov * acc[3] / 4 + 0.5f));
    }
}

// ---- text: x the left edge, base the baseline, in pixels; sp the letter spacing ----
static uint32_t next_cp(const char **s) { return pf_next(s); }
static float text_w(const char *s, int bold, float size, float sp) {
    float w = 0; while (*s) { const glyph *g = glyph_of(bold, size, next_cp(&s)); if (g) w += g->adv + sp; }
    return w;
}
static void text_at(stv_canvas *c, const char *s, float x, float base, int bold, float size, uint32_t rgb, float sp) {
    int b = (int)lroundf(base);
    while (*s) {
        const glyph *g = glyph_of(bold, size, next_cp(&s)); if (!g) continue;
        int gx = (int)lroundf(x) + g->x0, gy = b + g->y0;
        for (int j = 0; j < g->h; j++) { int Y = gy + j; if (Y < 0 || Y >= c->h) continue; uint32_t *row = c->px + (size_t)Y * c->w;
            for (int i = 0; i < g->w; i++) { int X = gx + i; if (X >= 0 && X < c->w) blend(row + X, rgb, g->a[j * g->w + i]); } }
        x += g->adv + sp;
    }
}
// One line made to fit maxw: smaller down to 80 %, then cut with "…"; out gets the text drawn, *sz its size; its width returned
static float fit(char *out, size_t cap, const char *s, int bold, float *sz, float sp, float maxw) {
    snprintf(out, cap, "%s", s); float size = *sz, w = text_w(out, bold, size, sp);
    if (w <= maxw || maxw <= 0) return w;
    float small = size * maxw / w; if (small >= size * 0.8f) { *sz = small; return text_w(out, bold, small, sp); }
    size *= 0.8f; *sz = size; float ell = text_w("\xe2\x80\xa6", bold, size, sp);
    size_t n = strlen(out);
    while (n) { n = pf_chop(out, n); if (text_w(out, bold, size, sp) + ell <= maxw) break; }
    while (n && out[n - 1] == ' ') out[--n] = 0;
    if (n + 4 < cap) memcpy(out + n, "\xe2\x80\xa6", 4);
    return text_w(out, bold, size, sp);
}
enum { LEFT, CENTER, RIGHT };
// a line in a line box whose top is top and height boxh (0: the normal height), anchored at x; fitted into maxw (0: as it is)
static float line(stv_canvas *c, const char *s, float x, float top, float boxh, int bold, float size, uint32_t rgb, int align, float maxw, float sp) {
    if (!s || !*s) return 0;
    char t[400]; float sz = size, w = fit(t, sizeof t, s, bold, &sz, sp, maxw);
    if (!boxh) boxh = LHN * size;
    float base = top + (boxh - LHN * sz) / 2 + ASC * sz;
    text_at(c, t, align == LEFT ? x : align == CENTER ? x - w / 2 : x - w, base, bold, sz, rgb, sp);
    return w;
}

// ---- the background: #0F1115 with the icon's colour as a big blurred square behind (CSS blur(σ) on a box: erf in x times erf in y), dithered against banding ----
static struct { uint32_t *px; int w, h; uint32_t key[6]; } BGC;
static void bg_free(void) { free(BGC.px); memset(&BGC, 0, sizeof BGC); }
static void background(stv_canvas *c, int glow, float gx, float gy, float size, float sigma, float opacity, uint32_t rgb) {
    uint32_t key[6] = {(uint32_t)glow, (uint32_t)gx, (uint32_t)gy, (uint32_t)size, (uint32_t)(opacity * 1000), rgb};
    if (BGC.px && BGC.w == c->w && BGC.h == c->h && !memcmp(key, BGC.key, sizeof key)) { memcpy(c->px, BGC.px, (size_t)c->w * c->h * 4); return; }
    uint32_t bg = 0xff000000u | (BG & 255) << 16 | (BG >> 8 & 255) << 8 | BG >> 16;
    if (!glow) { for (size_t i = 0; i < (size_t)c->w * c->h; i++) c->px[i] = bg; }
    else {
        float *ax = malloc(sizeof(float) * (size_t)(c->w + c->h)), *ay = ax ? ax + c->w : NULL; float k = 1.0f / (sigma * 1.41421356f);
        if (!ax) { for (size_t i = 0; i < (size_t)c->w * c->h; i++) c->px[i] = bg; return; }
        for (int i = 0; i < c->w; i++) ax[i] = 0.5f * (erff((i + 0.5f - (gx - size / 2)) * k) - erff((i + 0.5f - (gx + size / 2)) * k));
        for (int j = 0; j < c->h; j++) ay[j] = 0.5f * (erff((j + 0.5f - (gy - size / 2)) * k) - erff((j + 0.5f - (gy + size / 2)) * k));
        static const float bayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
        float br = BG >> 16, bgc = BG >> 8 & 255, bb = BG & 255, dr = (rgb >> 16) - br, dg = (rgb >> 8 & 255) - bgc, db = (rgb & 255) - bb;
        for (int j = 0; j < c->h; j++) for (int i = 0; i < c->w; i++) {
            float a = opacity * ax[i] * ay[j], d = (bayer[(j & 3) * 4 + (i & 3)] + 0.5f) / 16;
            int r = (int)(br + dr * a + d), g = (int)(bgc + dg * a + d), b = (int)(bb + db * a + d);
            c->px[(size_t)j * c->w + i] = 0xff000000u | (uint32_t)b << 16 | (uint32_t)g << 8 | (uint32_t)r;
        }
        free(ax);
    }
    uint32_t *keep = BGC.w == c->w && BGC.h == c->h ? BGC.px : realloc(BGC.px, (size_t)c->w * c->h * 4);
    if (keep) { BGC.px = keep; BGC.w = c->w; BGC.h = c->h; memcpy(BGC.key, key, sizeof key); memcpy(BGC.px, c->px, (size_t)c->w * c->h * 4); }
}

// ---- icons (the design's SVGs, in a 24-unit box) ----
static const float CHECK[] = {5, 12, 10, 17, 19, 7};
static const float CLOUD[] = {7.00f, 18.00f, 17.00f, 18.00f, 18.17f, 17.83f, 19.24f, 17.32f, 20.11f, 16.52f, 20.72f, 15.50f, 20.99f, 14.35f, 20.92f, 13.17f, 20.51f, 12.06f,
    19.79f, 11.12f, 18.82f, 10.43f, 17.70f, 10.06f, 17.43f, 9.13f, 17.01f, 8.26f, 16.46f, 7.47f, 15.78f, 6.77f, 15.01f, 6.20f, 14.15f, 5.75f, 13.23f, 5.45f, 12.27f, 5.30f,
    11.31f, 5.31f, 10.35f, 5.47f, 9.44f, 5.78f, 8.59f, 6.24f, 7.82f, 6.82f, 7.15f, 7.52f, 6.61f, 8.32f, 6.20f, 9.20f, 5.25f, 9.60f, 4.41f, 10.20f, 3.73f, 10.98f, 3.25f, 11.90f,
    2.99f, 12.90f, 2.97f, 13.93f, 3.18f, 14.94f, 3.62f, 15.88f, 4.25f, 16.69f, 5.06f, 17.33f, 5.99f, 17.78f};   // M7 18h10a4 4 0 0 0 .7-7.94A6 6 0 0 0 6.2 9.2 4.5 4.5 0 0 0 7 18z
static void icon_warn(stv_canvas *c, float x, float y, float px, float width) {   // a circle with "!"
    float k = px / 24; static const float bar[] = {12, 8, 12, 13}, dot[] = {12, 16.5f, 12, 17};
    arc(c, 12, 12, 9, 0, 6.2832f, x, y, k, width, WARN); stroke(c, bar, 2, 0, x, y, k, width, WARN, 1); stroke(c, dot, 2, 0, x, y, k, width, WARN, 1);
}
static void step_icon(stv_canvas *c, int state, float x, float y, float px, double t) {
    float k = px / 24;
    if (state == STV_DONE) stroke(c, CHECK, 3, 0, x, y, k, 2.5f, OK_GREEN, 1);
    else if (state == STV_RUN) { float a = (float)fmod(t * 6.2832, 6.2832); arc(c, 12, 12, 9, a - 1.5708f, a + 3.1416f, x, y, k, 2.5f, ACC); }   // M12 3a9 9 0 1 0 9 9, turning
    else if (state == STV_FAIL) icon_warn(c, x, y, px, 2);
    else if (state == STV_SKIP) { static const float dash[] = {8, 12, 16, 12}; stroke(c, dash, 2, 0, x, y, k, 2.5f, DOT, 1); }
    else rrect(c, x + 9 * k, y + 9 * k, 6 * k, 6 * k, 3 * k, DOT, 1);
}

// ---- the steps ----
typedef struct { float s, W, H; } box;   // s: px a design px; W, H: the canvas in design px
#define ZOOM 1.3f   // the design's px against the mock-ups'
static float s_of(int w, int h) { float s = fminf(w / 1280.0f, h / 720.0f); return s > 0 ? s : 1; }
static box box_k(int w, int h, float k) { box b; b.s = s_of(w, h) * k; b.W = w / b.s; b.H = h / b.s; return b; }
static box box_of(int w, int h) { return box_k(w, h, ZOOM); }
// a page that needs need_w x need_h design px: ZOOM when it fits, else as big as fits (1 at least)
static box box_need(int w, int h, float need_w, float need_h) {
    float s0 = s_of(w, h), k = fminf(ZOOM, fminf(w / (s0 * need_w), h / (s0 * need_h)));
    return box_k(w, h, k < 1 ? 1 : k);
}
// a game's name in two lines at most (maxw pixels, bold, letter spacing sp pixels at *sz): one line, made smaller to 80 % when that is enough; else broken at the last
// space that lets the first line fit, both lines at the size the longer needs (80 % at least; line() cuts what is still too long). Returns the lines, *sz their size
static int name_lines(const char *name, float *sz, float sp, float maxw, char *l1, char *l2, size_t cap) {
    float size = *sz, w = text_w(name, 1, size, sp); l2[0] = 0; snprintf(l1, cap, "%s", name);
    if (w <= maxw || maxw <= 0) return 1;
    if (w * 0.8f <= maxw) { *sz = size * maxw / w; return 1; }
    const char *cut = NULL;
    for (int pass = 0; pass < 2 && !cut; pass++) { float k = pass ? 0.8f : 1;   // the last space whose line fits at the full size, else at 80 %
        for (const char *q = name; *q; q++) if (*q == ' ') { size_t n = (size_t)(q - name); if (n >= cap) break; char t[400]; if (n >= sizeof t) break;
            memcpy(t, name, n); t[n] = 0; if (text_w(t, 1, size * k, sp * k) <= maxw) cut = q; else break; } }
    if (!cut) return 1;   // no space to break at: one line, made to fit
    size_t n = (size_t)(cut - name); memcpy(l1, name, n); l1[n] = 0; snprintf(l2, cap, "%s", cut + 1);
    float wm = fmaxf(text_w(l1, 1, size, sp), text_w(l2, 1, size, sp));
    if (wm > maxw) *sz = size * fmaxf(0.8f, maxw / wm);
    return 2;
}
// a card of steps at (x, y) design px, w wide; returns its height
typedef struct { float pad_y, pad_x, size, icon, gap; } card_style;
static float card_h(card_style k) { float rh = fmaxf(k.icon, LHN * k.size); return 2 * k.pad_y + STV_NSTEPS * rh + (STV_NSTEPS - 1) * k.gap; }
static void steps_card(stv_canvas *c, const stv_view *v, box b, float x, float y, float w, card_style k, double t) {
    float s = b.s, rh = fmaxf(k.icon, LHN * k.size);
    rrect(c, x * s, y * s, w * s, card_h(k) * s, 16 * s, CARD, 0.85f);
    for (int i = 0; i < STV_NSTEPS; i++) {
        const stv_step *st = &v->step[i]; float top = y + k.pad_y + i * (rh + k.gap), lx = x + k.pad_x, rx = x + w - k.pad_x;
        int dim = st->state == STV_WAIT || st->state == STV_SKIP;
        uint32_t name = dim ? DIM2 : INK, note = dim ? DIM2 : st->state == STV_FAIL ? WARN : st->state == STV_RUN && v->bar < 0 ? ACC : DIM;
        step_icon(c, st->state, lx * s, (top + (rh - k.icon) / 2) * s, k.icon * s, t);
        float nx = lx + k.icon + 14, nw = line(c, st->name, nx * s, top * s, rh * s, 0, k.size * s, name, LEFT, (rx - nx) * 0.55f * s, 0) / s;
        line(c, st->note, rx * s, top * s, rh * s, 0, 15 * s, note, RIGHT, (rx - nx - nw - 16) * s, 0);
    }
}
// the progress block (w wide at (x, y)): a title and the percentage, the bar, two small lines; gap: between its rows. Returns its height
static float bar_h(float gap) { return (ASC + 0.288f) * 34 + gap + 12 + gap + LHN * 15; }
static void bar_block(stv_canvas *c, const stv_view *v, box b, float x, float y, float w, float gap) {
    float s = b.s, row = (ASC + 0.288f) * 34, base = (y + ASC * 34) * s; char pct[16]; int p = v->bar > 1000 ? 1000 : v->bar < 0 ? 0 : v->bar;
    snprintf(pct, sizeof pct, "%d%%", p / 10);
    float pw = text_w(pct, 1, 34 * s, 0); text_at(c, pct, (x + w) * s - pw, base, 1, 34 * s, ACC, 0);
    char t[400]; float sz = 18 * s; float tw = fit(t, sizeof t, v->bar_title, 0, &sz, 0, w * s - pw - 24 * s); (void)tw; text_at(c, t, x * s, base, 0, sz, INK, 0);
    float by = y + row + gap; rrect(c, x * s, by * s, w * s, 12 * s, 6 * s, TRACK, 1);
    if (p > 0) rrect(c, x * s, by * s, fmaxf(w * p / 1000.0f, 12) * s, 12 * s, 6 * s, ACC, 1);
    float ly = by + 12 + gap, lw = line(c, v->bar_left, x * s, ly * s, 0, 0, 15 * s, DIM, LEFT, w * 0.5f * s, 0) / s;
    line(c, v->bar_right, (x + w) * s, ly * s, 0, 0, 15 * s, DIM, RIGHT, (w - lw - 24) * s, 0);
}
static void hints(stv_canvas *c, const stv_view *v, float cx, float top, float size, float s, float maxw, int align) {
    line(c, v->hint, cx * s, top * s, 0, 0, size * s, v->hint_warn ? WARN : size < 15 ? DIM2 : DIM, align, maxw * s, 0);   // (15 px: #9AA1AC; the smaller hints #8A919C)
    if (v->hint2[0]) line(c, v->hint2, cx * s, (top + LHN * size + 6) * s, 0, 0, 13 * s, DIM2, align, maxw * s, 0);
}
static float hints_h(const stv_view *v, float size) { return !v->hint[0] ? 0 : LHN * size + (v->hint2[0] ? 6 + LHN * 13 : 0); }
static void foot(stv_canvas *c, const char *t, box b) { line(c, t, b.W / 2 * b.s, (b.H - 36 - LHN * 14) * b.s, 0, 0, 14 * b.s, DIM2, CENTER, (b.W - 64) * b.s, 0); }
static int wide_of(int w, int h) { return h > 0 && w * 2 >= h * 3; }   // 3:2 and wider: side by side

static void head_line(stv_canvas *c, const stv_view *v, box b, float y, int empty);
static void draw_steps(stv_canvas *c, const stv_view *v, double t) {
    box b = box_of(c->w, c->h); float s = b.s; int bar = v->bar >= 0, icon = v->icon && v->icon_w > 0 && v->icon_h > 0;
    if (!wide_of(c->w, c->h)) {   // Main.dc.html / Download.dc.html: the name over the steps, all centred
        background(c, 1, c->w / 2.0f, c->h / 2.0f - 60 * s, 600 * s, 110 * s, 0.18f, v->glow);
        float gap = bar ? 36 : 40, cw = bar ? 720 : 584; card_style k = bar ? (card_style){24, 32, 17, 20, 12} : (card_style){28, 32, 18, 22, 14};
        float hs = bar ? 14 : 15, hh = hints_h(v, hs);
        float total = 72 + gap + (bar ? bar_h(14) + gap : 0) + card_h(k) + (hh ? gap + hh : 0), y = (b.H - total) / 2, cx = b.W / 2;
        head_line(c, v, b, y, 0); y += 72 + gap;   // the head: the icon (72, radius 16), 20 px, the name (30 bold, -0.5 spacing) over the version (15)
        if (bar) { bar_block(c, v, b, cx - 360, y, 720, 14); y += bar_h(14) + gap; }
        steps_card(c, v, b, cx - cw / 2, y, cw, k, t); y += card_h(k) + gap;
        if (hh) hints(c, v, cx, y, hs, s, b.W - 64, CENTER);
    } else {   // Wide.dc.html: the icon, name and version in a column of 340 at the left, the progress and the steps at the right
        background(c, 1, 320 * s, c->h / 2.0f, 480 * s, 100 * s, 0.16f, v->glow);
        float lx = fminf(80, fmaxf(48, (b.W - 340 - 56 - 560) / 2)), lw = 340, rx = lx + lw + 56, rw = b.W - lx - rx; card_style k = {22, 28, 17, 20, 12};   // (the right side 560 wide at least while the margins can give)
        // the name: two lines at most, broken at a space only
        char l1[400], l2[400]; float sz = 30 * s; int lines = name_lines(v->name, &sz, -0.5f * s, lw * s, l1, l2, sizeof l1);
        float nl = 1.25f * 30, lh = 96 + 18 + lines * nl + 18 + LHN * 15, ly = (b.H - lh) / 2;
        if (icon) image(c, v->icon, v->icon_w, v->icon_h, lx * s, ly * s, 96 * s, 22 * s);
        line(c, l1, lx * s, (ly + 114) * s, nl * s, 1, sz, INK, LEFT, lw * s, -0.5f * sz / 30);
        if (lines > 1) line(c, l2, lx * s, (ly + 114 + nl) * s, nl * s, 1, sz, INK, LEFT, lw * s, -0.5f * sz / 30);
        line(c, v->version, lx * s, (ly + 114 + lines * nl + 18) * s, 0, 0, 15 * s, DIM, LEFT, lw * s, 0);
        float hh = hints_h(v, 13), rh = (bar ? bar_h(12) + 28 : 0) + card_h(k) + (hh ? 28 + hh : 0), y = (b.H - rh) / 2;
        if (bar) { bar_block(c, v, b, rx, y, rw, 12); y += bar_h(12) + 28; }
        steps_card(c, v, b, rx, y, rw, k, t); y += card_h(k) + 28;
        if (hh) hints(c, v, rx, y, 13, s, rw, LEFT);
    }
    if (v->foot[0]) foot(c, v->foot, b);
}

// ---- a question of the cloud's start (Conflict / Offline / OtherDevice.dc.html): an icon, a title and body, two cards or a row of buttons, a countdown, hints ----
typedef struct { float y_icon, y_title, y_body, y_main, y_cbar, y_count, y_foot, y_card, card_w; int body_b, nbody; float card_hgt, head_h[2]; float r[3][4]; int n; } qlay;
// the top of a column total high, centred, kept off the top (24) and off what the bottom keeps (bottom)
static float q_top(box b, float total, float bottom) { float y = (b.H - total) / 2; if (y + total > b.H - bottom) y = b.H - bottom - total; return fmaxf(24, y); }
static float card_head(const dm_cloud_card *k) { return k->newer ? LHN * 13 + 8 : LHN * 15; }
static float card_inner(const dm_cloud_card *k) { return card_head(k) + 10 + LHN * 40 + 10 + LHN * 16 + (k->size[0] ? 10 + 6 + 1 + 14 + LHN * 15 : 0); }
static qlay q_layout(const dm_cloud_view *q, box b) {   // in design px (the rectangles too); no font needed (a touch is tested on the main thread)
    qlay L; memset(&L, 0, sizeof L);
    if (q->style == DM_CLOUD_STYLE_LIST || q->style == DM_CLOUD_STYLE_PAIR) {   // AfterTest / ManualPending.dc.html: the title and its line, the card of rows, the buttons, a line; 36 apart
        int list = q->style == DM_CLOUD_STYLE_LIST, n = list ? (q->nbtn > 3 ? 3 : q->nbtn) : 2, nr = q->nrow > 2 ? 2 : q->nrow;
        float th = LHN * 30 + (q->line[0][0] ? 10 + LHN * 16 : 0), ch = nr ? 40 + nr * LHN * 15 + (nr - 1) * 12 : 0, bh = list ? (n ? 68 + (n - 1) * 70 : 0) : 64, fh = q->foot[0] ? LHN * 14 : 0;
        float bottom = q->foot2[0] ? 36 + LHN * 14 + 12 : 24, fixed = th + ch + bh + fh, ng = (ch ? 1 : 0) + 1 + (fh ? 1 : 0), gap = 36;   // (foot2: the bottom line)
        if (fixed + ng * gap > b.H - bottom - 24) gap = fmaxf(16, (b.H - bottom - 24 - fixed) / ng);   // 36 apart, closer on a short screen
        float rw = fminf(600, b.W - 120), y = q_top(b, fixed + ng * gap, bottom);
        L.y_title = y; L.y_body = y + LHN * 30 + 10; y += th + gap;
        if (ch) { L.y_card = y; L.card_hgt = ch; y += ch + gap; }
        L.card_w = rw + 56; L.y_main = y; L.n = n;   // (the card: 600 plus its padding, CSS's content box)
        for (int i = 0; i < n; i++) {
            if (list) { L.r[i][0] = b.W / 2 - rw / 2; L.r[i][1] = y + (i ? 68 + 10 + (i - 1) * 70 : 0); L.r[i][2] = rw; L.r[i][3] = i ? 60 : 68; }
            else { float w = (rw - 16) / 2; L.r[i][0] = b.W / 2 - rw / 2 + i * (w + 16); L.r[i][1] = y; L.r[i][2] = w; L.r[i][3] = 64; }
        }
        L.y_foot = y + bh + gap;
        return L;
    }
    L.nbody = !!q->line[0][0] + !!q->line[1][0]; L.body_b = L.nbody == 2;
    float gap = q->ncards ? 40 : 36, hs[8]; int nb = 0; float *ys[8];
    if (q->icon) { hs[nb] = q->icon == DM_CLOUD_ICON_WARN ? 64 : 56; ys[nb++] = &L.y_icon; }
    hs[nb] = LHN * 30 + (L.nbody ? (L.body_b ? 12 + 2 * 1.6f * 17 : 10 + LHN * 16) : 0); ys[nb++] = &L.y_title;
    if (q->ncards) { L.head_h[0] = card_head(&q->card[0]); L.head_h[1] = card_head(&q->card[1]);
        L.card_hgt = 4 + 56 + fmaxf(card_inner(&q->card[0]), card_inner(&q->card[1])); hs[nb] = L.card_hgt; ys[nb++] = &L.y_main; }
    else if (q->nbtn) { hs[nb] = 60; ys[nb++] = &L.y_main; }
    if (q->count[0] && q->nbtn && !q->ncards) { hs[nb] = 4; ys[nb++] = &L.y_cbar; }
    int nf = !!q->foot[0] + !!q->foot2[0]; if (nf) { hs[nb] = nf == 2 ? LHN * 15 + 8 + LHN * 14 : LHN * 14; ys[nb++] = &L.y_foot; }
    float fixed = 0; for (int i = 0; i < nb; i++) fixed += hs[i];
    float cnt = q->count[0] && q->nbtn && !q->ncards ? 12 + LHN * 14 : 0; fixed += cnt;
    if (nb > 1 && fixed + (nb - 1) * gap > b.H - 48) gap = fmaxf(16, (b.H - 48 - fixed) / (nb - 1));   // closer on a short screen
    float y = q_top(b, fixed + (nb - 1) * gap, 24);
    for (int i = 0; i < nb; i++) { *ys[i] = y; y += hs[i] + gap; if (ys[i] == &L.y_cbar) { L.y_count = y - gap + 12; y += cnt; } }
    L.y_body = L.y_title + LHN * 30 + (L.body_b ? 12 : 10);
    if (q->ncards) { float cw = fminf(912, b.W - 64), w = (cw - 32) / 2; L.n = 2;   // two cards of 440, 32 apart (narrower to keep 32 at the sides)
        for (int i = 0; i < 2; i++) { L.r[i][0] = b.W / 2 - cw / 2 + i * (w + 32); L.r[i][1] = L.y_main; L.r[i][2] = w; L.r[i][3] = L.card_hgt; } }
    else { L.n = q->nbtn > 3 ? 3 : q->nbtn; float tw = L.n * 260 + (L.n - 1) * 20;
        for (int i = 0; i < L.n; i++) { L.r[i][0] = b.W / 2 - tw / 2 + i * 280; L.r[i][1] = L.y_main; L.r[i][2] = 260; L.r[i][3] = 60; } }
    return L;
}
static void choice_frame(stv_canvas *c, float s, const float *r, float rad, int sel) {   // a card or a button: the chosen one with the accent's edge and a soft ring 6 px out
    if (sel) rrect(c, (r[0] - 6) * s, (r[1] - 6) * s, (r[2] + 12) * s, (r[3] + 12) * s, (rad + 6) * s, ACC, 0.15f);
    rrect(c, r[0] * s, r[1] * s, r[2] * s, r[3] * s, rad * s, sel ? ACC : EDGE, 1);
    rrect(c, (r[0] + 2) * s, (r[1] + 2) * s, (r[2] - 4) * s, (r[3] - 4) * s, (rad - 2) * s, sel ? CARD_SEL : CARD, 1);
}
static void badge_a(stv_canvas *c, float s, float cx, float cy);
static void kv_card(stv_canvas *c, float s, float x, float y, float w, float h, int n, const char (*key)[32], const char (*val)[96], const int *col, float pad);
static void halo(stv_canvas *c, float s, const float *r, float rad, uint32_t rgb, float a);
static void draw_pick(stv_canvas *c, const dm_cloud_view *q, box b, qlay L) {   // AfterTest / ManualPending.dc.html
    float s = b.s, cx = b.W / 2; int list = q->style == DM_CLOUD_STYLE_LIST;
    line(c, q->title, cx * s, L.y_title * s, 0, 1, 30 * s, INK, CENTER, (b.W - 64) * s, 0);
    line(c, q->line[0], cx * s, L.y_body * s, 0, 0, 16 * s, DIM, CENTER, (b.W - 64) * s, 0);
    if (L.card_hgt > 0) {   // (the rows' names are 48 bytes here: copied into the card's 32-byte columns would cut them, so drawn the same way by hand)
        float w = L.card_w, x = cx - w / 2; rrect(c, x * s, L.y_card * s, w * s, L.card_hgt * s, 16 * s, CARD, 1);
        for (int i = 0; i < q->nrow && i < 2; i++) { float top = L.y_card + 20 + i * (LHN * 15 + 12);
            float kw = line(c, q->row_k[i], (x + 28) * s, top * s, 0, 0, 15 * s, DIM, LEFT, (w - 56) * 0.45f * s, 0) / s;
            line(c, q->row_v[i], (x + w - 28) * s, top * s, 0, 0, 15 * s, INK, RIGHT, (w - 56 - kw - 16) * s, 0); }
    }
    for (int i = 0; i < L.n; i++) {
        const float *r = L.r[i]; int sel = list ? q->sel == i : i == 0;
        if (i == 0) {   // the accent's: its name (dark, bold), A at its right in a list; the soft ring when chosen
            if (sel) halo(c, s, r, 16, ACC, 0.18f);
            rrect(c, r[0] * s, r[1] * s, r[2] * s, r[3] * s, 16 * s, ACC, 1);
            if (list) { line(c, q->btn[0], (r[0] + 28) * s, r[1] * s, r[3] * s, 1, 20 * s, BG, LEFT, (r[2] - 56 - 44) * s, 0); badge_a(c, s, r[0] + r[2] - 28 - 14, r[1] + r[3] / 2); }
            else line(c, q->btn[0], (r[0] + r[2] / 2) * s, r[1] * s, r[3] * s, 1, 19 * s, BG, CENTER, (r[2] - 24) * s, 0);
        } else if (list) {
            if (sel) choice_frame(c, s, r, 14, 1); else rrect(c, r[0] * s, r[1] * s, r[2] * s, r[3] * s, 14 * s, CARD, 0.85f);
            float lw = line(c, q->btn[i], (r[0] + 28) * s, r[1] * s, r[3] * s, 0, 17 * s, INK, LEFT, (r[2] - 56) * 0.5f * s, 0) / s;
            line(c, q->bnote[i], (r[0] + r[2] - 28) * s, r[1] * s, r[3] * s, 0, 13 * s, DIM, RIGHT, (r[2] - 56 - lw - 16) * s, 0);
        } else {
            rrect(c, r[0] * s, r[1] * s, r[2] * s, r[3] * s, 16 * s, 0x232935u, 1);
            line(c, q->btn[i], (r[0] + r[2] / 2) * s, r[1] * s, r[3] * s, 0, 19 * s, INK, CENTER, (r[2] - 24) * s, 0);
        }
    }
    if (q->foot[0]) line(c, q->foot, cx * s, L.y_foot * s, 0, 0, 14 * s, DIM2, CENTER, (b.W - 64) * s, 0);
    if (q->foot2[0]) foot(c, q->foot2, b);
}
static void draw_ask(stv_canvas *c, const dm_cloud_view *q) {
    box b = box_of(c->w, c->h); float s = b.s, cx = b.W / 2; qlay L = q_layout(q, b);
    background(c, 0, 0, 0, 0, 0, 0, 0);
    if (q->style == DM_CLOUD_STYLE_LIST || q->style == DM_CLOUD_STYLE_PAIR) { draw_pick(c, q, b, L); return; }
    if (q->icon == DM_CLOUD_ICON_OFFLINE) { static const float slash[] = {4, 4, 20, 20}; float x = (cx - 28) * s, y = L.y_icon * s, k = 56 * s / 24;
        stroke(c, CLOUD, (int)(sizeof CLOUD / sizeof *CLOUD / 2), 1, x, y, k, 1.6f, DIM, 1); stroke(c, slash, 2, 0, x, y, k, 1.6f, DIM, 1); }
    else if (q->icon == DM_CLOUD_ICON_WARN) { rrect(c, (cx - 32) * s, L.y_icon * s, 64 * s, 64 * s, 32 * s, WARN, 0.16f); icon_warn(c, (cx - 16) * s, (L.y_icon + 16) * s, 32 * s, 2); }
    line(c, q->title, cx * s, L.y_title * s, 0, 1, 30 * s, INK, CENTER, (b.W - 64) * s, 0);
    for (int i = 0, k = 0; i < 2; i++) if (q->line[i][0]) {
        if (L.body_b) line(c, q->line[i], cx * s, (L.y_body + k * 1.6f * 17) * s, 1.6f * 17 * s, 0, 17 * s, BODY, CENTER, fminf(1100, b.W - 64) * s, 0);
        else line(c, q->line[i], cx * s, L.y_body * s, 0, 0, 16 * s, DIM, CENTER, (b.W - 64) * s, 0);
        k++;
    }
    for (int i = 0; i < L.n; i++) {
        const float *r = L.r[i]; int sel = i == q->sel;
        choice_frame(c, s, r, q->ncards ? 18 : 14, sel);
        if (q->ncards) {   // padding 28 / 32 inside the 2 px edge
            const dm_cloud_card *k = &q->card[i]; float x = r[0] + 34, w = r[2] - 68, y = r[1] + 30, hh = L.head_h[i], bw = 0;
            if (k->newer) { const char *nb = q->badge[0] ? q->badge : "newer"; bw = text_w(nb, 1, 13 * s, 0) / s + 20;
                rrect(c, (x + w - bw) * s, y * s, bw * s, hh * s, hh / 2 * s, ACC, 1); line(c, nb, (x + w - bw / 2) * s, y * s, hh * s, 1, 13 * s, BG, CENTER, 0, 0); }
            line(c, k->head, x * s, y * s, hh * s, 0, 15 * s, DIM, LEFT, (w - bw - (bw ? 8 : 0)) * s, 0); y += hh + 10;
            line(c, k->ago, x * s, y * s, 0, 1, 40 * s, INK, LEFT, w * s, 0); y += LHN * 40 + 10;
            line(c, k->date, x * s, y * s, 0, 0, 16 * s, BODY, LEFT, w * s, 0); y += LHN * 16 + 10 + 6;
            if (k->size[0]) { rrect(c, x * s, y * s, w * s, fmaxf(1, s), 0, sel ? 0x2E3747u : EDGE, 1); y += 1 + 14; line(c, k->size, x * s, y * s, 0, 0, 15 * s, DIM, LEFT, w * s, 0); }
        } else line(c, q->btn[i], (r[0] + r[2] / 2) * s, r[1] * s, r[3] * s, 0, 18 * s, INK, CENTER, (r[2] - 24) * s, 0);
    }
    if (L.y_cbar > 0 && L.n) {   // the time left before Play anyway starts by itself: a bar under the first button, the seconds under it
        float x = L.r[0][0], p = q->count_permille < 0 ? 0 : q->count_permille > 1000 ? 1000 : q->count_permille;
        rrect(c, x * s, L.y_cbar * s, 260 * s, 4 * s, 2 * s, TRACK, 1); if (p > 0) rrect(c, x * s, L.y_cbar * s, 260 * p / 1000 * s, 4 * s, 2 * s, ACC, 1);
        line(c, q->count, cx * s, L.y_count * s, 0, 0, 14 * s, DIM2, CENTER, (b.W - 64) * s, 0);
    }
    if (q->foot[0] && q->foot2[0]) { line(c, q->foot, cx * s, L.y_foot * s, 0, 0, 15 * s, DIM, CENTER, (b.W - 64) * s, 0);
        line(c, q->foot2, cx * s, (L.y_foot + LHN * 15 + 8) * s, 0, 0, 14 * s, DIM2, CENTER, (b.W - 64) * s, 0); }
    else if (q->foot[0] || q->foot2[0]) line(c, q->foot[0] ? q->foot : q->foot2, cx * s, L.y_foot * s, 0, 0, 14 * s, DIM2, CENTER, (b.W - 64) * s, 0);
}

// ---- the launcher and its pages (Launcher / LauncherWide / CloudSettings / AppSettings.dc.html): the rectangles in design px from fixed sizes only (a touch is tested
// with them on the main thread, without a font) ----
#define BTN 0x232935u      // a plain button
#define RED_BG 0x231B1Du   // Back to the cloud save: its fill, edge, words and the held part
#define RED_EDGE 0x5A3A3Au
#define RED_INK 0xF2B8B5u
#define RED_DIM 0xC99A97u
#define RED_HOLD 0xE57373u
#define TRACK_OFF 0x3A4150u
#define RADIO_OFF 0x5A6270u
static uint32_t col_of(int k) { return k == STV_COL_GOOD ? OK_GREEN : k == STV_COL_WARN ? WARN : k == STV_COL_DIM ? DIM : INK; }
static void edged(stv_canvas *c, float s, const float *r, float rad, float edge_w, uint32_t edge, uint32_t fill) {   // a box with an edge inside its rectangle
    rrect(c, r[0] * s, r[1] * s, r[2] * s, r[3] * s, rad * s, edge, 1);
    rrect(c, (r[0] + edge_w) * s, (r[1] + edge_w) * s, (r[2] - 2 * edge_w) * s, (r[3] - 2 * edge_w) * s, (rad - edge_w) * s, fill, 1);
}
static void halo(stv_canvas *c, float s, const float *r, float rad, uint32_t rgb, float a) { rrect(c, (r[0] - 6) * s, (r[1] - 6) * s, (r[2] + 12) * s, (r[3] + 12) * s, (rad + 6) * s, rgb, a); }   // box-shadow 0 0 0 6px
static void disc(stv_canvas *c, float cx, float cy, float r, uint32_t rgb, float a) { rrect(c, cx - r, cy - r, 2 * r, 2 * r, r, rgb, a); }
static void ring(stv_canvas *c, float cx, float cy, float r, float w, uint32_t rgb) {   // a circle's outline, w wide, centred on radius r (pixels)
    int x0 = (int)floorf(cx - r - w), x1 = (int)ceilf(cx + r + w), y0 = (int)floorf(cy - r - w), y1 = (int)ceilf(cy + r + w);
    for (int j = y0 < 0 ? 0 : y0; j < y1 && j < c->h; j++) for (int i = x0 < 0 ? 0 : x0; i < x1 && i < c->w; i++) {
        float d = hypotf(i + 0.5f - cx, j + 0.5f - cy), cov = clampf(w / 2 - fabsf(d - r) + 0.5f, 0, 1); if (cov > 0) blend(c->px + (size_t)j * c->w + i, rgb, (int)(cov * 255 + 0.5f));
    }
}
static void tri(stv_canvas *c, const float *p, uint32_t rgb) {   // a filled triangle (pixels), its edges smoothed: the play icon (M7 4.5v15l13-7.5z)
    float x0 = fminf(p[0], fminf(p[2], p[4])), x1 = fmaxf(p[0], fmaxf(p[2], p[4])), y0 = fminf(p[1], fminf(p[3], p[5])), y1 = fmaxf(p[1], fmaxf(p[3], p[5]));
    float area = (p[2] - p[0]) * (p[5] - p[1]) - (p[4] - p[0]) * (p[3] - p[1]), sg = area < 0 ? -1.0f : 1.0f;
    for (int j = (int)floorf(y0) < 0 ? 0 : (int)floorf(y0); j <= (int)ceilf(y1) && j < c->h; j++) for (int i = (int)floorf(x0) < 0 ? 0 : (int)floorf(x0); i <= (int)ceilf(x1) && i < c->w; i++) {
        float px = i + 0.5f, py = j + 0.5f, d = 1e9f;
        for (int k = 0; k < 3; k++) { const float *a = p + 2 * k, *b = p + 2 * ((k + 1) % 3); float ex = b[0] - a[0], ey = b[1] - a[1], l = hypotf(ex, ey);
            if (l > 0) d = fminf(d, sg * ((px - a[0]) * ey - (py - a[1]) * ex) / -l); }
        float cov = clampf(d + 0.5f, 0, 1); if (cov > 0) blend(c->px + (size_t)j * c->w + i, rgb, (int)(cov * 255 + 0.5f));
    }
}
// lines of at most maxw (pixels), broken at the spaces, each lh high from top; returns the lines drawn (at most cap)
static int para(stv_canvas *c, const char *s, float x, float top, float maxw, float size, float lh, uint32_t rgb, int align, int cap) {
    char ln[400]; int n = 0; const char *p = s;
    while (*p && n < cap) {
        const char *best = NULL, *q = p;
        for (;;) { const char *e = strchr(q, ' '); size_t k = (size_t)((e ? e : q + strlen(q)) - p); if (k >= sizeof ln) break;
            memcpy(ln, p, k); ln[k] = 0; if (text_w(ln, 0, size, 0) > maxw && best) break;
            best = e ? e : q + strlen(q); if (!e) break; q = e + 1; }
        if (!best) best = p + strlen(p);
        size_t k = (size_t)(best - p); if (k >= sizeof ln) k = sizeof ln - 1; memcpy(ln, p, k); ln[k] = 0;
        if (n == cap - 1 && *best) { snprintf(ln, sizeof ln, "%s", p); }   // the last line takes the rest (made to fit)
        line(c, ln, x, top + n * lh, lh, 0, size, rgb, align, maxw, 0);
        n++; p = *best ? best + 1 : best;
    }
    return n;
}
static void head_line(stv_canvas *c, const stv_view *v, box b, float y, int empty) {   // the narrow screens' head (Main.dc.html's): the icon 72 (empty: an empty square
    // without one), the name (two lines at most) over the version, centred against the icon
    float s = b.s, cx = b.W / 2; int icon = v->icon && v->icon_w > 0 && v->icon_h > 0;
    char l1[400], l2[400]; float nsz = 30 * s, maxw = fminf(b.W - 128, 1000) - 92; int lines = name_lines(v->name, &nsz, -0.5f * s, maxw * s, l1, l2, sizeof l1);
    float sp = -0.5f * nsz / 30, nw = fminf(maxw, fmaxf(text_w(l1, 1, nsz, sp), lines > 1 ? text_w(l2, 1, nsz, sp) : 0) / s), nl = lines > 1 ? 1.25f * 30 : LHN * 30;
    float vw = text_w(v->version, 0, 15 * s, 0) / s, tw = fminf(maxw, fmaxf(nw, vw)), x0 = cx - (92 + tw) / 2, ty = y + (72 - (lines * nl + 4 + LHN * 15)) / 2;
    if (icon) image(c, v->icon, v->icon_w, v->icon_h, x0 * s, y * s, 72 * s, 16 * s); else if (empty) rrect(c, x0 * s, y * s, 72 * s, 72 * s, 16 * s, EDGE, 1);
    for (int i = 0; i < lines; i++) line(c, i ? l2 : l1, (x0 + 92) * s, (ty + i * nl) * s, nl * s, 1, nsz, INK, LEFT, maxw * s, sp);
    line(c, v->version, (x0 + 92) * s, (ty + lines * nl + 4) * s, 0, 0, 15 * s, DIM, LEFT, tw * s, 0);
}
static void foot_at(stv_canvas *c, const char *t, box b, float bottom, float size) { line(c, t, b.W / 2 * b.s, (b.H - bottom - LHN * size) * b.s, 0, 0, size * b.s, DIM2, CENTER, (b.W - 64) * b.s, 0); }

typedef struct { float r[4][4], head_y, status_y, colx; } llay;   // the items (0..2) and Start (3); the narrow head's and statuses' tops; the wide left column's x
static llay ln_layout(box b, int wide) {
    llay L; memset(&L, 0, sizeof L);
    float ih = wide ? 60 : 64, mt = wide ? 24 : 32, list = 3 * ih + 3 * 10 + mt + 72, x, y;
    if (!wide) { float total = 72 + 36 + LHN * 14 + 36 + list; L.head_y = (b.H - total) / 2; L.status_y = L.head_y + 72 + 36; y = L.status_y + LHN * 14 + 36; x = b.W / 2 - 260; }
    else { float gap = fminf(56, fmaxf(32, b.W - 96 - 860)); L.colx = (b.W - 860 - gap) / 2; x = L.colx + 340 + gap; y = (b.H - list) / 2; }   // (the columns 56 apart, less to keep 48 at the sides)
    for (int i = 0; i < 3; i++) { L.r[i][0] = x; L.r[i][1] = y + i * (ih + 10); L.r[i][2] = 520; L.r[i][3] = ih; }
    L.r[3][0] = x; L.r[3][1] = y + 3 * (ih + 10) + mt; L.r[3][2] = 520; L.r[3][3] = 72;
    return L;
}
static void status_at(stv_canvas *c, const stv_status *st, float x, float top, float s, float gap, float maxw, double t) {   // the icon (16) and the words (14), top: the
    // line's; maxw: the words' room (0: as they are)
    step_icon(c, st->state, x * s, (top + (LHN * 14 - 16) / 2) * s, 16 * s, t);
    line(c, st->text, (x + 16 + gap) * s, top * s, 0, 0, 14 * s, st->state == STV_RUN ? INK : st->state == STV_FAIL ? WARN : DIM, LEFT, maxw * s, 0);
}
static void badge_a(stv_canvas *c, float s, float cx, float cy) { disc(c, cx * s, cy * s, 14 * s, BG, 0.18f); line(c, "A", cx * s, (cy - LHN * 14 / 2) * s, 0, 1, 14 * s, BG, CENTER, 0, 0); }
static void draw_launcher(stv_canvas *c, const stv_view *v, double t) {
    box b = box_of(c->w, c->h); float s = b.s; int wide = wide_of(c->w, c->h); const stv_launcher *n = &v->ln; llay L = ln_layout(b, wide);
    if (!wide) {   // Launcher.dc.html: the head, the three statuses in a row, the items
        background(c, 1, c->w / 2.0f, (b.H / 2 - 100) * s, 600 * s, 110 * s, 0.16f, v->glow);
        head_line(c, v, b, L.head_y, 1);
        float w[STV_NSTEPS], tot = 0;
        for (int i = 0; i < STV_NSTEPS; i++) { w[i] = 16 + 6 + text_w(n->status[i].text, 0, 14 * s, 0) / s; tot += w[i] + (i ? 22 : 0); }
        float x = b.W / 2 - tot / 2;
        for (int i = 0; i < STV_NSTEPS; i++) { status_at(c, &n->status[i], x, L.status_y, s, 6, 0, t); x += w[i] + 22; }
    } else {   // LauncherWide.dc.html: the icon (88), the name (two lines at most), the version and the statuses in a column of 340 at the left, the items at the right
        background(c, 1, (L.colx + 108) * s, (b.H / 2 - 10) * s, 460 * s, 100 * s, 0.16f, v->glow);
        float lw = 340, nl = 1.25f * 28; char l1[400], l2[400]; float sz = 28 * s; int lines = name_lines(v->name, &sz, -0.5f * s, lw * s, l1, l2, sizeof l1);
        float h = 88 + 22 + lines * nl + 6 + LHN * 14 + 22 + 3 * LHN * 14 + 2 * 10, y = (b.H - h) / 2, x = L.colx;
        if (v->icon && v->icon_w > 0 && v->icon_h > 0) image(c, v->icon, v->icon_w, v->icon_h, x * s, y * s, 88 * s, 20 * s); else rrect(c, x * s, y * s, 88 * s, 88 * s, 20 * s, EDGE, 1);
        y += 88 + 22;
        line(c, l1, x * s, y * s, nl * s, 1, sz, INK, LEFT, lw * s, -0.5f * sz / 28); if (lines > 1) line(c, l2, x * s, (y + nl) * s, nl * s, 1, sz, INK, LEFT, lw * s, -0.5f * sz / 28);
        y += lines * nl + 6; line(c, v->version, x * s, y * s, 0, 0, 14 * s, DIM, LEFT, lw * s, 0); y += LHN * 14 + 22;
        for (int i = 0; i < STV_NSTEPS; i++) { status_at(c, &n->status[i], x, y, s, 8, lw - 24, t); y += LHN * 14 + 10; }
    }
    float ls = wide ? 17 : 18, ns = wide ? 13 : 14;
    for (int i = 0; i < 3; i++) {   // an item that is off: its words dimmed, its ring still shown when it is chosen
        const float *r = L.r[i]; int sel = n->sel == i, off = n->item_off[i];
        if (sel) choice_frame(c, s, r, 14, 1); else rrect(c, r[0] * s, r[1] * s, r[2] * s, r[3] * s, 14 * s, CARD, 0.85f);
        float lw = line(c, n->item[i], (r[0] + 28) * s, r[1] * s, r[3] * s, 0, ls * s, off ? DIM2 : INK, LEFT, (r[2] - 56) * 0.55f * s, 0) / s;
        line(c, n->note[i], (r[0] + r[2] - 28) * s, r[1] * s, r[3] * s, 0, ns * s, off ? DIM2 : DIM, RIGHT, (r[2] - 56 - lw - 16) * s, 0);
    }
    const float *r = L.r[3]; float k = 22 * s / 24, ix = (r[0] + 28) * s, iy = (r[1] + (r[3] - 22) / 2) * s;
    if (!n->start_off) {   // Start: the accent's fill, the play icon, the name, A; chosen: the soft ring around it
        if (n->sel == 3) halo(c, s, r, 16, ACC, 0.18f);
        rrect(c, r[0] * s, r[1] * s, r[2] * s, r[3] * s, 16 * s, ACC, 1);
        float p[6] = {ix + 7 * k, iy + 4.5f * k, ix + 7 * k, iy + 19.5f * k, ix + 20 * k, iy + 12 * k}; tri(c, p, BG);
        line(c, n->start, (r[0] + 28 + 22 + 16) * s, r[1] * s, r[3] * s, 1, 22 * s, BG, LEFT, (r[2] - 28 - 22 - 16 - 28 - 28 - 16) * s, 0);
        badge_a(c, s, r[0] + r[2] - 28 - 14, r[1] + r[3] / 2);
    } else {   // the start not ready: a plain button (chosen: the accent's edge) that its progress fills from the left in the accent, faint; a spinner, the name dimmed
        // over what the start is doing; no A (it does nothing until the start is ready)
        if (n->sel == 3) edged(c, s, r, 16, 2, ACC, CARD_SEL); else rrect(c, r[0] * s, r[1] * s, r[2] * s, r[3] * s, 16 * s, BTN, 1);
        if (n->start_permille > 0) { float fw = r[2] * (n->start_permille > 1000 ? 1000 : n->start_permille) / 1000.0f;
            rrect_clip(c, r[0] * s, r[1] * s, r[2] * s, r[3] * s, 16 * s, ACC, 0.16f, (r[0] + fw) * s); }
        step_icon(c, STV_RUN, ix, iy, 22 * s, t);
        float bh = LHN * 20 + 2 + LHN * 13, ty = r[1] + (r[3] - bh) / 2, tx = r[0] + 28 + 22 + 16;
        line(c, n->start, tx * s, ty * s, 0, 1, 20 * s, DIM, LEFT, (r[2] - tx + r[0] - 28) * s, 0);
        line(c, n->start_sub, tx * s, (ty + LHN * 20 + 2) * s, 0, 0, 13 * s, BODY, LEFT, (r[2] - tx + r[0] - 28) * s, 0);
    }
    if (n->keys[0]) foot_at(c, n->keys, b, wide ? 24 : 36, wide ? 13 : 14);
}

// the two pages: the title (30 bold) and the room under it; the keys' line at the bottom (14, 28 up) and the room above it; the sides kept 32 wide
#define PG_TITLE (LHN * 30 + 28)
#define PG_FOOT (16 + LHN * 14 + 28)
static float pg_top(box b, float content) { return fmaxf(32, (b.H - PG_FOOT - content) / 2); }   // centred above the keys' line
typedef struct { float x0, cw, lw, y0, top, mh, note_y, rx, rw, card_y, card_h, r[6][4]; int note_n; } clay;   // r: the modes (0..2), Upload now (3), Back to the cloud
// save (4), the link's button (5; a rectangle 0 wide when there is none); cw the page's width, lw the modes' (the mock-up's 1040 and 520: narrower where the window is, the modes giving first); note_n: the note's lines
static float cp_mh(void) { return 44 + LHN * 19 + 4 + LHN * 14; }
static float cp_content(int note_n) { return PG_TITLE + LHN * 14 + 14 + 3 * cp_mh() + 2 * 12 + 16 + note_n * 1.5f * 13; }   // the left column, the taller
static box cp_box(int w, int h) { return box_need(w, h, 880 + 64, 32 + cp_content(2) + PG_FOOT); }
static clay cp_layout(const stv_cloudpage *p, box b) {
    clay L; memset(&L, 0, sizeof L);
    L.cw = fminf(1040, b.W - 64); L.rw = fminf(480, L.cw - 460); L.lw = L.cw - 40 - L.rw; L.x0 = (b.W - L.cw) / 2;
    L.note_n = b.H - PG_FOOT - 32 >= cp_content(3) ? 3 : 2;
    L.y0 = pg_top(b, cp_content(L.note_n)); L.top = L.y0 + PG_TITLE;
    float y = L.top + LHN * 14 + 14; L.mh = cp_mh();
    for (int i = 0; i < 3; i++) { L.r[i][0] = L.x0; L.r[i][1] = y + i * (L.mh + 12); L.r[i][2] = L.lw; L.r[i][3] = L.mh; }
    L.note_y = y + 3 * L.mh + 2 * 12 + 16;
    L.rx = L.x0 + L.lw + 40; L.card_y = y; int n = p->nrow < 0 ? 0 : p->nrow > 4 ? 4 : p->nrow;
    L.card_h = 40 + n * LHN * 15 + (n > 1 ? (n - 1) * 12 : 0);
    if (p->link == 1) {   // not linked: Link this device above the card, nothing under it
        L.r[5][0] = L.rx; L.r[5][1] = y; L.r[5][2] = L.rw; L.r[5][3] = 64; L.card_y = y + 64 + 12;
        return L;
    }
    float uy = L.card_y + L.card_h + 12;
    if (p->link == 2) { L.r[5][0] = L.rx; L.r[5][1] = uy; L.r[5][2] = L.rw; L.r[5][3] = 48; uy += 48 + 12; }   // linked: Unlink this app under the card (its account row)
    L.r[3][0] = L.rx; L.r[3][1] = uy; L.r[3][2] = L.rw; L.r[3][3] = 56;
    L.r[4][0] = L.rx; L.r[4][1] = uy + 56 + 12; L.r[4][2] = L.rw; L.r[4][3] = 56;
    return L;
}
static void page_title(stv_canvas *c, const char *title, const char *game, float x0, float w, float y0, float s) {   // the title (30 bold) and the game's name at the right, on one baseline
    float tw = line(c, title, x0 * s, y0 * s, 0, 1, 30 * s, INK, LEFT, w * 0.6f * s, 0) / s;
    line(c, game, (x0 + w) * s, (y0 + ASC * (30 - 15)) * s, 0, 0, 15 * s, DIM, RIGHT, (w - tw - 24) * s, 0);
}
static void kv_card(stv_canvas *c, float s, float x, float y, float w, float h, int n, const char (*key)[32], const char (*val)[96], const int *col, float pad) {   // a card of rows
    rrect(c, x * s, y * s, w * s, h * s, 16 * s, CARD, 1);
    for (int i = 0; i < n; i++) { float top = y + 20 + i * (LHN * 15 + 12);
        float kw = line(c, key[i], (x + pad) * s, top * s, 0, 0, 15 * s, DIM, LEFT, (w - 2 * pad) * 0.45f * s, 0) / s;
        line(c, val[i], (x + w - pad) * s, top * s, 0, 0, 15 * s, col_of(col[i]), RIGHT, (w - 2 * pad - kw - 16) * s, 0); }
}
static void draw_cloudpage(stv_canvas *c, const stv_view *v) {
    box b = cp_box(c->w, c->h); float s = b.s; const stv_cloudpage *p = &v->cp; clay L = cp_layout(p, b);
    background(c, 0, 0, 0, 0, 0, 0, 0);
    page_title(c, p->title, v->name, L.x0, L.cw, L.y0, s);
    line(c, p->label[0], L.x0 * s, L.top * s, 0, 0, 14 * s, DIM, LEFT, L.lw * s, 0);
    for (int i = 0; i < 3; i++) {   // the modes: a radio, the name (19 bold) over what it does (14); the one chosen with the accent's edge
        const float *r = L.r[i]; int sel = p->sel == i && !p->modes_off, on = p->checked == i;
        if (sel) edged(c, s, r, 16, 2, ACC, CARD_SEL); else edged(c, s, r, 16, 2, EDGE, CARD);
        float cx = r[0] + 26 + 11, cy = r[1] + 22 + 2 + 11;
        ring(c, cx * s, cy * s, 10 * s, 2 * s, on ? ACC : RADIO_OFF); if (on) disc(c, cx * s, cy * s, 5 * s, ACC, 1);
        float tx = r[0] + 26 + 22 + 16, tw = r[2] - (tx - r[0]) - 26;
        line(c, p->mode[i], tx * s, (r[1] + 22) * s, 0, 1, 19 * s, p->modes_off && !on ? DIM2 : INK, LEFT, tw * s, 0);
        line(c, p->desc[i], tx * s, (r[1] + 22 + LHN * 19 + 4) * s, 0, 0, 14 * s, p->modes_off && !on ? DIM2 : BODY, LEFT, tw * s, 0);
    }
    if (p->note[0]) para(c, p->note, L.x0 * s, L.note_y * s, L.lw * s, 13 * s, 1.5f * 13 * s, DIM2, LEFT, L.note_n);
    line(c, p->label[1], L.rx * s, L.top * s, 0, 0, 14 * s, DIM, LEFT, L.rw * s, 0);
    kv_card(c, s, L.rx, L.card_y, L.rw, L.card_h, p->nrow > 4 ? 4 : p->nrow, p->key, p->val, p->col, 24);
    const float *lb = L.r[5]; int lsel = p->sel == 5;
    if (p->link == 1) {   // Link this device: the accent's fill (as Start), its name dark and bold, the words at its right; chosen: the soft ring
        if (lsel) halo(c, s, lb, 14, ACC, 0.18f);
        rrect(c, lb[0] * s, lb[1] * s, lb[2] * s, lb[3] * s, 14 * s, ACC, 1);
        float w = line(c, p->lbtn, (lb[0] + 24) * s, lb[1] * s, lb[3] * s, 1, 17 * s, BG, LEFT, (lb[2] - 48) * 0.6f * s, 0) / s;
        line(c, p->lnote, (lb[0] + lb[2] - 24) * s, lb[1] * s, lb[3] * s, 0, 13 * s, 0x1E2633u, RIGHT, (lb[2] - 48 - w - 16) * s, 0);
        if (p->keys[0]) foot_at(c, p->keys, b, 28, 14);
        return;
    }
    if (p->link == 2) {   // Unlink this app: a plain button; pressed once, red (the next press unlinks)
        if (p->larm) edged(c, s, lb, 14, 2, RED_HOLD, RED_BG); else if (lsel) edged(c, s, lb, 14, 2, ACC, CARD_SEL); else rrect(c, lb[0] * s, lb[1] * s, lb[2] * s, lb[3] * s, 14 * s, BTN, 1);
        float w = line(c, p->lbtn, (lb[0] + 24) * s, lb[1] * s, lb[3] * s, 0, 16 * s, p->larm ? RED_INK : INK, LEFT, (lb[2] - 48) * 0.45f * s, 0) / s;
        line(c, p->lnote, (lb[0] + lb[2] - 24) * s, lb[1] * s, lb[3] * s, 0, 13 * s, p->larm ? RED_DIM : DIM, RIGHT, (lb[2] - 48 - w - 16) * s, 0);
    }
    const float *u = L.r[3];   // Upload now
    if (p->sel == 3) edged(c, s, u, 14, 2, ACC, CARD_SEL); else rrect(c, u[0] * s, u[1] * s, u[2] * s, u[3] * s, 14 * s, BTN, 1);
    float uw = line(c, p->up, (u[0] + 24) * s, u[1] * s, u[3] * s, 0, 16 * s, p->up_off ? DIM2 : INK, LEFT, (u[2] - 48) * 0.6f * s, 0) / s;
    line(c, p->up_note, (u[0] + u[2] - 24) * s, u[1] * s, u[3] * s, 0, 13 * s, DIM, RIGHT, (u[2] - 48 - uw - 16) * s, 0);
    const float *k = L.r[4];   // Back to the cloud save: red, its held part filling from the left
    int ks = p->sel == 4;
    if (p->back_off) rrect(c, k[0] * s, k[1] * s, k[2] * s, k[3] * s, 14 * s, BTN, 1);
    else edged(c, s, k, 14, ks ? 2 : 1, ks ? RED_HOLD : RED_EDGE, RED_BG);
    if (!p->back_off && p->hold > 0) { float hw = k[2] * (p->hold > 1000 ? 1000 : p->hold) / 1000.0f; rrect_clip(c, k[0] * s, k[1] * s, k[2] * s, k[3] * s, 14 * s, RED_HOLD, 0.22f, (k[0] + hw) * s); }
    float bw = line(c, p->back, (k[0] + 24) * s, k[1] * s, k[3] * s, 0, 16 * s, p->back_off ? DIM2 : RED_INK, LEFT, (k[2] - 48) * 0.55f * s, 0) / s;
    line(c, p->back_note, (k[0] + k[2] - 24) * s, k[1] * s, k[3] * s, 0, 13 * s, p->back_off ? DIM2 : RED_DIM, RIGHT, (k[2] - 48 - bw - 16) * s, 0);
    if (p->keys[0]) foot_at(c, p->keys, b, 28, 14);
}

// the app's page: one column of 720 (AppSettings.dc.html), or where that is too tall for the window, two (the switch at the left, the data's card, its buttons and the
// note at the right; each half of the page's width)
typedef struct { float x0, w, y0, lab1, lab2, lab2x, lab3, labw, note_x, note_y, note_w, sw[4], loc[4], card[4], btn[2][4], game[3][4]; } alay;   // loc: the data's place, under the switch;
                                                                                                                                                // game: the game's rows, under it (lab3 their title)
static float ap_sh(void) { return 40 + LHN * 18 + 4 + 2 * 1.5f * 14; }
static float ap_data(void) { return 40 + 3 * LHN * 15 + 2 * 12 + 10 + 56 + 10 + 2 * 1.5f * 13; }   // the data's card, the buttons, the note's two lines
#define AP_GROW 60   // a game's row: its height, 10 apart
static float ap_game(int n) { return n > 0 ? 28 + LHN * 14 + 10 + n * AP_GROW + (n - 1) * 10 : 0; }   // the game's rows and their title, under the data's place
static float ap_one(int n) { return PG_TITLE + LHN * 14 + 10 + 2 * ap_sh() + 12 + ap_game(n) + 28 + LHN * 14 + 10 + ap_data(); }
static float ap_two(int n) { return PG_TITLE + LHN * 14 + 10 + fmaxf(2 * ap_sh() + 12 + ap_game(n), ap_data()); }
static box ap_box(int w, int h, int n) {   // n: the game's rows
    box b = box_of(w, h);
    return b.H >= 32 + ap_one(n) + PG_FOOT ? b : box_need(w, h, 880 + 64, 32 + ap_two(n) + PG_FOOT);
}
static alay ap_layout(box b, int n) {
    alay L; memset(&L, 0, sizeof L); n = n < 0 ? 0 : n > 3 ? 3 : n;
    int two = b.H < 32 + ap_one(n) + PG_FOOT; float sh = ap_sh(), ch = 40 + 3 * LHN * 15 + 2 * 12, y, cw, cx;
    if (!two) { L.w = cw = 720; L.x0 = cx = (b.W - 720) / 2; L.y0 = pg_top(b, ap_one(n)); L.labw = 720; }
    else { L.w = fminf(1040, b.W - 64); L.x0 = (b.W - L.w) / 2; cw = L.labw = (L.w - 40) / 2; cx = L.x0 + cw + 40; L.y0 = pg_top(b, ap_two(n)); }
    y = L.y0 + PG_TITLE; L.lab1 = y; y += LHN * 14 + 10;
    L.sw[0] = L.x0; L.sw[1] = y; L.sw[2] = cw; L.sw[3] = sh;
    L.loc[0] = L.x0; L.loc[1] = y + sh + 12; L.loc[2] = cw; L.loc[3] = sh;
    { float gy = y + 2 * sh + 12 + 28; L.lab3 = gy; gy += LHN * 14 + 10;   // the game's rows, in the left column (under the data's place)
      for (int i = 0; i < n; i++) { L.game[i][0] = L.x0; L.game[i][1] = gy + i * (AP_GROW + 10); L.game[i][2] = cw; L.game[i][3] = AP_GROW; } }
    if (!two) { y += 2 * sh + 12 + ap_game(n) + 28; L.lab2 = y; y += LHN * 14 + 10; } else L.lab2 = L.lab1;
    L.lab2x = cx;
    L.card[0] = cx; L.card[1] = y; L.card[2] = cw; L.card[3] = ch; y += ch + 10;
    float bw = (cw - 10) / 2;   // (the mock-up's 355, 10 apart)
    for (int i = 0; i < 2; i++) { L.btn[i][0] = cx + i * (bw + 10); L.btn[i][1] = y; L.btn[i][2] = bw; L.btn[i][3] = 56; }
    L.note_x = cx; L.note_y = y + 56 + 10; L.note_w = cw;
    return L;
}
static void draw_apppage(stv_canvas *c, const stv_view *v) {
    const stv_apppage *p = &v->ap; box b = ap_box(c->w, c->h, p->ngame); float s = b.s; alay L = ap_layout(b, p->ngame);
    background(c, 0, 0, 0, 0, 0, 0, 0);
    page_title(c, p->title, v->name, L.x0, L.w, L.y0, s);
    line(c, p->label[0], L.x0 * s, L.lab1 * s, 0, 0, 14 * s, DIM, LEFT, L.labw * s, 0);
    const float *r = L.sw;   // the switch's card: the name (18), two lines (14, 1.5), the switch (56 x 32) at the right
    if (p->sel == 0) edged(c, s, r, 16, 2, ACC, CARD); else rrect(c, r[0] * s, r[1] * s, r[2] * s, r[3] * s, 16 * s, CARD, 1);
    line(c, p->quick, (r[0] + 24) * s, (r[1] + 20) * s, 0, 0, 18 * s, INK, LEFT, (r[2] - 48 - 56 - 20) * s, 0);
    for (int i = 0; i < 2; i++) line(c, p->quick_desc[i], (r[0] + 24) * s, (r[1] + 20 + LHN * 18 + 4 + i * 21) * s, 21 * s, 0, 14 * s, DIM, LEFT, (r[2] - 48 - 56 - 20) * s, 0);
    float tx = r[0] + r[2] - 24 - 56, ty = r[1] + (r[3] - 32) / 2;
    rrect(c, tx * s, ty * s, 56 * s, 32 * s, 16 * s, p->quick_on ? ACC : TRACK_OFF, 1);
    disc(c, (tx + (p->quick_on ? 28 : 4) + 12) * s, (ty + 4 + 12) * s, 12 * s, p->quick_on ? 0xFFFFFFu : BODY, 1);
    const float *q = L.loc;   // the data's place: the name (18), its two lines, where it is now at the right (bold, the accent; dimmed when it cannot be chosen)
    if (p->sel == 3) edged(c, s, q, 16, 2, ACC, CARD); else rrect(c, q[0] * s, q[1] * s, q[2] * s, q[3] * s, 16 * s, CARD, 1);
    float vw = p->loc_val[0] ? line(c, p->loc_val, (q[0] + q[2] - 24) * s, q[1] * s, q[3] * s, 1, 17 * s, p->loc_off ? DIM2 : ACC, RIGHT, q[2] * 0.4f * s, 0) / s : 0;
    line(c, p->loc, (q[0] + 24) * s, (q[1] + 20) * s, 0, 0, 18 * s, p->loc_off ? DIM2 : INK, LEFT, (q[2] - 48 - vw - 20) * s, 0);
    for (int i = 0; i < 2; i++) line(c, p->loc_desc[i], (q[0] + 24) * s, (q[1] + 20 + LHN * 18 + 4 + i * 21) * s, 21 * s, 0, 14 * s, DIM, LEFT, (q[2] - 48 - vw - 20) * s, 0);
    if (p->ngame > 0) line(c, p->glabel, L.x0 * s, L.lab3 * s, 0, 0, 14 * s, DIM, LEFT, L.labw * s, 0);
    for (int i = 0; i < p->ngame && i < 3; i++) {   // a game's row: its name (17), its value at the right (bold, the accent), as the data's place shows where it is
        const float *g = L.game[i];
        if (p->sel == 4 + i) edged(c, s, g, 16, 2, ACC, CARD); else rrect(c, g[0] * s, g[1] * s, g[2] * s, g[3] * s, 16 * s, CARD, 1);
        float vw = line(c, p->gval[i], (g[0] + g[2] - 24) * s, g[1] * s, g[3] * s, 1, 17 * s, ACC, RIGHT, g[2] * 0.45f * s, 0) / s;
        line(c, p->gname[i], (g[0] + 24) * s, g[1] * s, g[3] * s, 0, 17 * s, INK, LEFT, (g[2] - 48 - vw - 20) * s, 0);
    }
    line(c, p->label[1], L.lab2x * s, L.lab2 * s, 0, 0, 14 * s, DIM, LEFT, L.labw * s, 0);
    kv_card(c, s, L.card[0], L.card[1], L.card[2], L.card[3], p->nrow > 3 ? 3 : p->nrow, p->key, p->val, p->col, 24);
    for (int i = 0; i < 2; i++) {
        const float *k = L.btn[i]; int off = i ? p->clear_off : p->verify_off;
        if (p->sel == i + 1) edged(c, s, k, 14, 2, ACC, CARD_SEL); else rrect(c, k[0] * s, k[1] * s, k[2] * s, k[3] * s, 14 * s, BTN, 1);
        line(c, i ? p->clear : p->verify, (k[0] + k[2] / 2) * s, k[1] * s, k[3] * s, 0, 16 * s, off ? DIM2 : INK, CENTER, (k[2] - 32) * s, 0);
    }
    if (p->note[0]) para(c, p->note, L.note_x * s, L.note_y * s, L.note_w * s, 13 * s, 1.5f * 13 * s, DIM2, LEFT, 2);
    if (p->keys[0]) foot_at(c, p->keys, b, 28, 14);
}
// ---- the device link's page (LinkCode / LinkDone.dc.html): the rectangles in design px from fixed sizes only, as the other pages ----
typedef struct { float y_title, y_main, y_btn, qx, rx, rw, y_disc, y_card, card_h, y_foot; } klay;
static float lk_card_h(const stv_linkpage *p) { int n = p->nrow > 3 ? 3 : p->nrow; return n ? 48 + n * LHN * 16 + (n - 1) * 14 : 0; }
static klay lk_layout(const stv_linkpage *p, box b) {
    klay L; memset(&L, 0, sizeof L);
    if (p->state == STV_LINK_CODE || p->state == STV_LINK_EXPIRED) {   // the title, 40, the QR code (260) and its column 64 apart, 40, the buttons (56)
        float total = LHN * 30 + 40 + 260 + 40 + 56, y = q_top(b, total, 28 + LHN * 14 + 12);
        L.rw = fminf(520, b.W - 64 - 260 - 64); L.qx = (b.W - 260 - 64 - L.rw) / 2; L.rx = L.qx + 260 + 64;
        L.y_title = y; L.y_main = y + LHN * 30 + 40; L.y_btn = L.y_main + 260 + 40;
    } else if (p->state == STV_LINK_DONE) {   // the check (80), 32, the title and its line, 32, the card (560), 32, the line, 32, Continue (60)
        L.card_h = lk_card_h(p); float th = LHN * 30 + 12 + LHN * 17, fh = p->foot[0] ? LHN * 14 + 32 : 0;
        float total = 80 + 32 + th + 32 + (L.card_h ? L.card_h + 32 : 0) + fh + 60, y = q_top(b, total, 28 + LHN * 14 + 12);
        L.y_disc = y; y += 80 + 32; L.y_title = y; y += th + 32; L.y_card = y; if (L.card_h) y += L.card_h + 32; L.y_foot = y; y += fh; L.y_btn = y;
    } else {   // BUSY, FAIL: the icon (56), 32, the title and its line, 40, the buttons
        float th = LHN * 30 + (p->line[0] ? 12 + LHN * 16 : 0), total = 56 + 32 + th + 40 + 56, y = q_top(b, total, 28 + LHN * 14 + 12);
        L.y_disc = y; L.y_title = y + 56 + 32; L.y_btn = L.y_title + th + 40;
    }
    return L;
}
static int lk_buttons(const stv_linkpage *p, box b, float r[2][4]) {   // the buttons (260 wide, 20 apart, centred): their count
    klay L = lk_layout(p, b); int n = p->nbtn < 0 ? 0 : p->nbtn > 2 ? 2 : p->nbtn; float h = p->state == STV_LINK_DONE ? 60 : 56, tw = n * 260 + (n > 1 ? 20 : 0);
    for (int i = 0; i < n; i++) { r[i][0] = b.W / 2 - tw / 2 + i * 280; r[i][1] = L.y_btn; r[i][2] = 260; r[i][3] = h; }
    return n;
}
static void fill_px(stv_canvas *c, int x, int y, int w, int h, uint32_t rgb, float alpha) {   // whole pixels (the QR code's modules: sharp, no smoothing)
    int a = (int)(alpha * 255 + 0.5f);
    for (int j = y < 0 ? 0 : y; j < y + h && j < c->h; j++) for (int i = x < 0 ? 0 : x; i < x + w && i < c->w; i++) blend(c->px + (size_t)j * c->w + i, rgb, a);
}
// the QR code of text in a white square (x, y, size pixels, radius rad): black modules a whole number of pixels each, 4 modules of white around them at least; faded:
// the modules light grey (a code run out). 0 when the text does not fit a version 10 code
static int qr_draw(stv_canvas *c, const char *text, float x, float y, float size, float rad, int faded) {
    static uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(10)], qr[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
    rrect(c, x, y, size, size, rad, 0xFFFFFFu, 1);
    if (!text[0] || !qrcodegen_encodeText(text, tmp, qr, qrcodegen_Ecc_MEDIUM, 1, 10, qrcodegen_Mask_AUTO, true)) return 0;
    int n = qrcodegen_getSize(qr), m = (int)floorf((size - 0.6f * rad) / (n + 8)); if (m < 1) m = 1;   // (the white's round corners keep off the 4 modules' square)
    int x0 = (int)lroundf(x + (size - n * m) / 2), y0 = (int)lroundf(y + (size - n * m) / 2);
    for (int j = 0; j < n; j++) for (int i = 0; i < n; i++) if (qrcodegen_getModule(qr, i, j)) fill_px(c, x0 + i * m, y0 + j * m, m, m, 0x000000u, faded ? 0.18f : 1);
    return n;
}
static void draw_linkpage(stv_canvas *c, const stv_view *v, double t) {
    box b = box_of(c->w, c->h); float s = b.s, cx = b.W / 2; const stv_linkpage *p = &v->lk; klay L = lk_layout(p, b); float br[2][4]; int nb = lk_buttons(p, b, br);
    background(c, 0, 0, 0, 0, 0, 0, 0);
    line(c, p->title, cx * s, L.y_title * s, 0, 1, 30 * s, INK, CENTER, (b.W - 64) * s, 0);
    if (p->state == STV_LINK_CODE || p->state == STV_LINK_EXPIRED) {
        int ex = p->state == STV_LINK_EXPIRED; float qs = 260 * s;
        qr_draw(c, p->qr, L.qx * s, L.y_main * s, qs, 16 * s, ex);
        // the column: the address's step, the code's step (22 apart), the wait's line; centred against the QR code
        float h1 = LHN * 15 + 8 + LHN * 22, h2 = LHN * 15 + 8 + LHN * 56, h3 = LHN * 15, y = L.y_main + (260 - (h1 + 22 + h2 + 22 + h3)) / 2, rw = L.rw;
        line(c, p->step1, L.rx * s, y * s, 0, 0, 15 * s, DIM, LEFT, rw * s, 0); y += LHN * 15 + 8;
        line(c, p->host, L.rx * s, y * s, 0, 0, 22 * s, ex ? DIM2 : ACC, LEFT, rw * s, 0); y += LHN * 22 + 22;
        line(c, p->step2, L.rx * s, y * s, 0, 0, 15 * s, DIM, LEFT, rw * s, 0); y += LHN * 15 + 8;
        line(c, p->code, L.rx * s, y * s, 0, 1, 56 * s, ex ? DIM2 : INK, LEFT, rw * s, 6 * s); y += LHN * 56 + 22;
        if (ex) { icon_warn(c, L.rx * s, (y + (LHN * 15 - 18) / 2) * s, 18 * s, 2.5f); line(c, p->status, (L.rx + 28) * s, y * s, 0, 0, 15 * s, WARN, LEFT, (rw - 28) * s, 0); }
        else { step_icon(c, STV_RUN, L.rx * s, (y + (LHN * 15 - 18) / 2) * s, 18 * s, t); line(c, p->status, (L.rx + 28) * s, y * s, 0, 0, 15 * s, DIM, LEFT, (rw - 28) * s, 0); }
    } else if (p->state == STV_LINK_DONE) {
        disc(c, cx * s, (L.y_disc + 40) * s, 40 * s, OK_GREEN, 0.16f); stroke(c, CHECK, 3, 0, (cx - 20) * s, (L.y_disc + 20) * s, 40 * s / 24, 2.5f, OK_GREEN, 1);
        line(c, p->line, cx * s, (L.y_title + LHN * 30 + 12) * s, 0, 0, 17 * s, BODY, CENTER, (b.W - 64) * s, 0);
        if (L.card_h) { float w = fminf(560, b.W - 64), x = cx - w / 2; rrect(c, x * s, L.y_card * s, w * s, L.card_h * s, 16 * s, CARD, 1);
            for (int i = 0; i < p->nrow && i < 3; i++) { float top = L.y_card + 24 + i * (LHN * 16 + 14);
                float kw = line(c, p->key[i], (x + 32) * s, top * s, 0, 0, 16 * s, DIM, LEFT, (w - 64) * 0.4f * s, 0) / s;
                line(c, p->val[i], (x + w - 32) * s, top * s, 0, 0, 16 * s, INK, RIGHT, (w - 64 - kw - 16) * s, 0); } }
        if (p->foot[0]) line(c, p->foot, cx * s, L.y_foot * s, 0, 0, 14 * s, DIM2, CENTER, (b.W - 64) * s, 0);
    } else {
        if (p->state == STV_LINK_BUSY) step_icon(c, STV_RUN, (cx - 28) * s, L.y_disc * s, 56 * s, t);
        else { static const float slash[] = {4, 4, 20, 20}; float x = (cx - 28) * s, y = L.y_disc * s, k = 56 * s / 24;
            stroke(c, CLOUD, (int)(sizeof CLOUD / sizeof *CLOUD / 2), 1, x, y, k, 1.6f, DIM, 1); stroke(c, slash, 2, 0, x, y, k, 1.6f, DIM, 1); }
        line(c, p->line, cx * s, (L.y_title + LHN * 30 + 12) * s, 0, 0, 16 * s, DIM, CENTER, (b.W - 64) * s, 0);
    }
    for (int i = 0; i < nb; i++) {   // the chosen one with the accent's edge and its ring; Continue (LinkDone) bold
        const float *r = br[i]; choice_frame(c, s, r, 14, p->sel == i);
        line(c, p->btn[i], (r[0] + r[2] / 2) * s, r[1] * s, r[3] * s, p->state == STV_LINK_DONE, (p->state == STV_LINK_DONE ? 18 : 17) * s, INK, CENTER, (r[2] - 24) * s, 0);
    }
    if (p->keys[0]) foot_at(c, p->keys, b, 28, 14);
}
int stv_page_hit(const stv_view *v, int w, int h, float x, float y) {
    box b = v->screen == STV_SCREEN_CLOUD ? cp_box(w, h) : v->screen == STV_SCREEN_APP ? ap_box(w, h, v->ap.ngame) : box_of(w, h);   // (each page's, as it is drawn)
    if (v->screen == STV_SCREEN_LINK) b = box_of(w, h);
    float X = x / b.s, Y = y / b.s; const float *r[8]; int n = 0;
    llay ll; clay cl; alay al; float lr[2][4];
    if (v->screen == STV_SCREEN_LAUNCHER) { ll = ln_layout(b, wide_of(w, h)); for (int i = 0; i < 4; i++) r[n++] = ll.r[i]; }
    else if (v->screen == STV_SCREEN_CLOUD) { cl = cp_layout(&v->cp, b); for (int i = 0; i < 6; i++) r[n++] = cl.r[i]; }
    else if (v->screen == STV_SCREEN_LINK) { int k = lk_buttons(&v->lk, b, lr); for (int i = 0; i < k; i++) r[n++] = lr[i]; }
    else if (v->screen == STV_SCREEN_APP) { al = ap_layout(b, v->ap.ngame); r[n++] = al.sw; r[n++] = al.btn[0]; r[n++] = al.btn[1]; r[n++] = al.loc; for (int i = 0; i < v->ap.ngame && i < 3; i++) r[n++] = al.game[i]; }
    for (int i = 0; i < n; i++) if (X >= r[i][0] && X < r[i][0] + r[i][2] && Y >= r[i][1] && Y < r[i][1] + r[i][3]) return i;
    return -1;
}

void stv_draw(stv_canvas *c, const stv_view *v, double t) {
    if (!c || !c->px || c->w <= 0 || c->h <= 0) return;
    if (v->ask && v->ask->ask) draw_ask(c, v->ask);
    else if (v->screen == STV_SCREEN_LAUNCHER) draw_launcher(c, v, t);
    else if (v->screen == STV_SCREEN_CLOUD) draw_cloudpage(c, v);
    else if (v->screen == STV_SCREEN_APP) draw_apppage(c, v);
    else if (v->screen == STV_SCREEN_LINK) draw_linkpage(c, v, t);
    else draw_steps(c, v, t);
}
int stv_animated(const stv_view *v) {
    if (v->ask && v->ask->ask) return 0;
    if (v->screen != STV_SCREEN_STEPS) return v->anim;
    for (int i = 0; i < STV_NSTEPS; i++) if (v->step[i].state == STV_RUN) return 1;
    return 0;
}
int stv_hit(const dm_cloud_view *q, int w, int h, float x, float y, int *kind) {
    box b = box_of(w, h); float X = x / b.s, Y = y / b.s;
    if (!q->ask) {   // the steps: the bottom line (B's hint) is the waiting step's one button
        if (q->nbtn && Y >= b.H - 36 - LHN * 14 - 16 && Y <= b.H - 16) { *kind = 2; return 0; }
        return -1;
    }
    qlay L = q_layout(q, b);
    for (int i = 0; i < L.n; i++) if (X >= L.r[i][0] && X < L.r[i][0] + L.r[i][2] && Y >= L.r[i][1] && Y < L.r[i][1] + L.r[i][3]) { *kind = q->ncards && q->style == DM_CLOUD_STYLE_ASK ? 1 : 2; return i; }
    return -1;
}

uint32_t stv_icon_colour(const uint32_t *px, int w, int h) {   // the saturated pixels' mean, as HSL with its lightness and saturation set for a glow on #0F1115
    double r = 0, g = 0, b = 0, sum = 0;
    if (!px || w <= 0 || h <= 0) return ACC;
    for (int j = 0; j < h; j += 2) for (int i = 0; i < w; i += 2) {
        uint32_t p = px[(size_t)j * w + i]; double R = (p & 255) / 255.0, G = (p >> 8 & 255) / 255.0, B = (p >> 16 & 255) / 255.0, A = (p >> 24) / 255.0;
        double mx = fmax(R, fmax(G, B)), mn = fmin(R, fmin(G, B)), sat = mx > 0 ? (mx - mn) / mx : 0, wgt = sat * sat * mx * A;
        r += R * wgt; g += G * wgt; b += B * wgt; sum += wgt;
    }
    if (sum < 0.02 * (w / 2) * (h / 2) * 0.05) return ACC;   // a grey icon: the accent
    r /= sum; g /= sum; b /= sum;
    double mx = fmax(r, fmax(g, b)), mn = fmin(r, fmin(g, b)), d = mx - mn, hue = 0;
    if (d < 1e-6) return ACC;
    if (mx == r) hue = fmod((g - b) / d, 6); else if (mx == g) hue = (b - r) / d + 2; else hue = (r - g) / d + 4;
    hue *= 60; if (hue < 0) hue += 360;
    double L = 0.62, S = 0.85, C = (1 - fabs(2 * L - 1)) * S, X = C * (1 - fabs(fmod(hue / 60, 2) - 1)), m = L - C / 2, o[3];
    int sx = (int)(hue / 60) % 6;
    double tab[6][3] = {{C, X, 0}, {X, C, 0}, {0, C, X}, {0, X, C}, {X, 0, C}, {C, 0, X}};
    for (int i = 0; i < 3; i++) o[i] = (tab[sx][i] + m) * 255 + 0.5;
    return (uint32_t)o[0] << 16 | (uint32_t)o[1] << 8 | (uint32_t)o[2];
}

// ---- speed and time left ----
void stv_rate_reset(stv_rate *r) { memset(r, 0, sizeof *r); r->eta = -1; }
void stv_rate_feed(stv_rate *r, int64_t t, double done, double left) {
    if (r->n && t - r->t[r->n - 1] < 250) return;   // a sample every 250 ms at most
    if (r->n && done < r->b[r->n - 1]) stv_rate_reset(r);   // a new count (the next phase)
    if (r->n == 48) { memmove(r->t, r->t + 1, 47 * sizeof *r->t); memmove(r->b, r->b + 1, 47 * sizeof *r->b); r->n--; }
    r->t[r->n] = t; r->b[r->n] = done; r->n++;
    int i0 = 0; while (i0 < r->n - 1 && t - r->t[i0] > 5000) i0++;
    double dt = (double)(t - r->t[i0]), step = r->last ? (double)(t - r->last) : 0; r->last = t;
    if (dt < 1000) return;   // under a second of samples: not known yet
    double raw = (done - r->b[i0]) * 1000.0 / dt;
    r->speed = r->speed > 0 ? r->speed + (raw - r->speed) * (1 - exp(-step / 2000.0)) : raw;   // smoothed over about 2 s
    if (r->speed <= 0 || left < 0) return;
    double e = left / r->speed;
    if (r->eta >= 0) { r->eta -= step / 1000.0; if (r->eta < 0) r->eta = 0; r->eta += (e - r->eta) * (1 - exp(-step / 3000.0)); }   // counting down, drawn to the estimate over 3 s
    else r->eta = e;
}
void stv_fmt_eta(char *o, size_t n, double s, int ko) {
    if (s < 0) { if (n) o[0] = 0; return; }
    if (s < 60) { int v = ((int)ceil(s) + 4) / 5 * 5; if (v < 5) v = 5; snprintf(o, n, ko ? "약 %d초 남음" : "about %d s left", v); }
    else if (s < 3600) snprintf(o, n, ko ? "약 %d분 남음" : "about %d min left", (int)ceil(s / 60));
    else { int m = (int)ceil(s / 60); snprintf(o, n, ko ? "약 %d시간 %d분 남음" : "about %d h %d min left", m / 60, m % 60); }
}
void stv_fmt_speed(char *o, size_t n, double bps) {
    double m = bps / 1048576.0;
    if (m >= 10) snprintf(o, n, "%.0f MB/s", m); else if (m >= 1) snprintf(o, n, "%.1f MB/s", m); else snprintf(o, n, "%.0f KB/s", bps / 1024.0);
}
void stv_fmt_bytes(char *o, size_t n, uint64_t done, uint64_t total) {
    if (total >= 1024ull << 20) snprintf(o, n, "%.1f / %.1f GB", done / 1073741824.0, total / 1073741824.0);
    else snprintf(o, n, "%.0f / %.0f MB", done / 1048576.0, total / 1048576.0);
}
void stv_fmt_int(char *o, size_t n, long long v) {
    char t[32]; int k = snprintf(t, sizeof t, "%lld", v < 0 ? -v : v), j = 0; char r[48];
    if (v < 0) r[j++] = '-';
    for (int i = 0; i < k; i++) { r[j++] = t[i]; if ((k - i - 1) % 3 == 0 && i < k - 1) r[j++] = ','; }
    r[j] = 0; snprintf(o, n, "%s", r);
}
