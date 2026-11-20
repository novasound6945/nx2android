// The start screen's drawing (startscreen.h): the design's px are for a 1280 px wide screen and scale with the window (s = min(w / 1280, h / 720): 1.5 on a 1920 x 1080
// Retroid Pocket 6, 1 on a 1280 x 960 Retroid Pocket Nova). A screen narrower than 3:2 stacks the game's name over the steps; a wider one puts them side by side.
// Every line of text stays one line: one too long for its place is drawn smaller (to 80 %), then cut with an ellipsis (a name may break at its spaces, two lines at most)
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
static box box_of(int w, int h) { box b; b.s = fminf(w / 1280.0f, h / 720.0f); if (b.s <= 0) b.s = 1; b.W = w / b.s; b.H = h / b.s; return b; }
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

static void draw_steps(stv_canvas *c, const stv_view *v, double t) {
    box b = box_of(c->w, c->h); float s = b.s; int bar = v->bar >= 0, icon = v->icon && v->icon_w > 0 && v->icon_h > 0;
    if (!wide_of(c->w, c->h)) {   // Main.dc.html / Download.dc.html: the name over the steps, all centred
        background(c, 1, c->w / 2.0f, c->h / 2.0f - 60 * s, 600 * s, 110 * s, 0.18f, v->glow);
        float gap = bar ? 36 : 40, cw = bar ? 720 : 584; card_style k = bar ? (card_style){24, 32, 17, 20, 12} : (card_style){28, 32, 18, 22, 14};
        float hs = bar ? 14 : 15, hh = hints_h(v, hs);
        float total = 72 + gap + (bar ? bar_h(14) + gap : 0) + card_h(k) + (hh ? gap + hh : 0), y = (b.H - total) / 2, cx = b.W / 2;
        // the head: the icon (72, radius 16), 20 px, the name (30 bold, -0.5 spacing) over the version (15)
        char nm[400]; float nsz = 30 * s, nw = fit(nm, sizeof nm, v->name, 1, &nsz, -0.5f * s, (fminf(b.W - 128, 1000) - 92) * s) / s;
        float vw = text_w(v->version, 0, 15 * s, 0) / s, tw = fmaxf(nw, vw), x0 = cx - (92 + tw) / 2, ty = y + (72 - (LHN * 30 + 4 + LHN * 15)) / 2;
        if (icon) image(c, v->icon, v->icon_w, v->icon_h, x0 * s, y * s, 72 * s, 16 * s);
        text_at(c, nm, (x0 + 92) * s, (ty + (LHN * 30 - LHN * nsz / s) / 2 + ASC * nsz / s) * s, 1, nsz, INK, -0.5f * s);
        line(c, v->version, (x0 + 92) * s, (ty + LHN * 30 + 4) * s, 0, 0, 15 * s, DIM, LEFT, tw * s, 0);
        y += 72 + gap;
        if (bar) { bar_block(c, v, b, cx - 360, y, 720, 14); y += bar_h(14) + gap; }
        steps_card(c, v, b, cx - cw / 2, y, cw, k, t); y += card_h(k) + gap;
        if (hh) hints(c, v, cx, y, hs, s, b.W - 64, CENTER);
    } else {   // Wide.dc.html: the icon, name and version in a column of 340 at the left, the progress and the steps at the right
        background(c, 1, 320 * s, c->h / 2.0f, 480 * s, 100 * s, 0.16f, v->glow);
        float lx = 80, lw = 340, rx = lx + lw + 56, rw = b.W - 80 - rx; card_style k = {22, 28, 17, 20, 12};
        // the name: two lines at most, broken at a space only (a line too long for 340 is made to fit)
        char l1[400] = "", l2[400] = ""; float sz = 30 * s; const char *sp = NULL;
        if (text_w(v->name, 1, sz, -0.5f * s) > lw * s) for (const char *q = v->name; *q; q++) if (*q == ' ') { char tmp[400]; size_t n = (size_t)(q - v->name); if (n >= sizeof tmp) break;
            memcpy(tmp, v->name, n); tmp[n] = 0; if (text_w(tmp, 1, sz, -0.5f * s) <= lw * s) sp = q; else break; }
        if (sp) { size_t n = (size_t)(sp - v->name); memcpy(l1, v->name, n); l1[n] = 0; snprintf(l2, sizeof l2, "%s", sp + 1); } else snprintf(l1, sizeof l1, "%s", v->name);
        int lines = l2[0] ? 2 : 1; float nl = 1.25f * 30, lh = 96 + 18 + lines * nl + 18 + LHN * 15, ly = (b.H - lh) / 2;
        if (icon) image(c, v->icon, v->icon_w, v->icon_h, lx * s, ly * s, 96 * s, 22 * s);
        line(c, l1, lx * s, (ly + 114) * s, nl * s, 1, 30 * s, INK, LEFT, lw * s, -0.5f * s);
        if (lines > 1) line(c, l2, lx * s, (ly + 114 + nl) * s, nl * s, 1, 30 * s, INK, LEFT, lw * s, -0.5f * s);
        line(c, v->version, lx * s, (ly + 114 + lines * nl + 18) * s, 0, 0, 15 * s, DIM, LEFT, lw * s, 0);
        float hh = hints_h(v, 13), rh = (bar ? bar_h(12) + 28 : 0) + card_h(k) + (hh ? 28 + hh : 0), y = (b.H - rh) / 2;
        if (bar) { bar_block(c, v, b, rx, y, rw, 12); y += bar_h(12) + 28; }
        steps_card(c, v, b, rx, y, rw, k, t); y += card_h(k) + 28;
        if (hh) hints(c, v, rx, y, 13, s, rw, LEFT);
    }
    if (v->foot[0]) foot(c, v->foot, b);
}

// ---- a question of the cloud's start (Conflict / Offline / OtherDevice.dc.html): an icon, a title and body, two cards or a row of buttons, a countdown, hints ----
typedef struct { float y_icon, y_title, y_body, y_main, y_cbar, y_count, y_foot, y_card; int body_b, nbody; float card_hgt, head_h[2]; float r[3][4]; int n; } qlay;
static float card_head(const dm_cloud_card *k) { return k->newer ? LHN * 13 + 8 : LHN * 15; }
static float card_inner(const dm_cloud_card *k) { return card_head(k) + 10 + LHN * 40 + 10 + LHN * 16 + (k->size[0] ? 10 + 6 + 1 + 14 + LHN * 15 : 0); }
static qlay q_layout(const dm_cloud_view *q, box b) {   // in design px (the rectangles too); no font needed (a touch is tested on the main thread)
    qlay L; memset(&L, 0, sizeof L);
    if (q->style == DM_CLOUD_STYLE_LIST || q->style == DM_CLOUD_STYLE_PAIR) {   // AfterTest / ManualPending.dc.html: the title and its line, the card of rows, the buttons, a line; 36 apart
        int list = q->style == DM_CLOUD_STYLE_LIST, n = list ? (q->nbtn > 3 ? 3 : q->nbtn) : 2, nr = q->nrow > 2 ? 2 : q->nrow;
        float th = LHN * 30 + (q->line[0][0] ? 10 + LHN * 16 : 0), ch = nr ? 40 + nr * LHN * 15 + (nr - 1) * 12 : 0, bh = list ? (n ? 68 + (n - 1) * 70 : 0) : 64, fh = q->foot[0] ? LHN * 14 : 0;
        float y = (b.H - (th + (ch ? 36 + ch : 0) + 36 + bh + (fh ? 36 + fh : 0))) / 2;
        L.y_title = y; L.y_body = y + LHN * 30 + 10; y += th + 36;
        if (ch) { L.y_card = y; L.card_hgt = ch; y += ch + 36; }
        L.y_main = y; L.n = n;
        for (int i = 0; i < n; i++) {
            if (list) { L.r[i][0] = b.W / 2 - 300; L.r[i][1] = y + (i ? 68 + 10 + (i - 1) * 70 : 0); L.r[i][2] = 600; L.r[i][3] = i ? 60 : 68; }
            else { L.r[i][0] = b.W / 2 - 300 + i * 308; L.r[i][1] = y; L.r[i][2] = 292; L.r[i][3] = 64; }
        }
        L.y_foot = y + bh + 36;
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
    float total = 0; for (int i = 0; i < nb; i++) total += hs[i] + (i ? gap : 0);
    float cnt = q->count[0] && q->nbtn && !q->ncards ? 12 + LHN * 14 : 0; total += cnt;
    float y = (b.H - total) / 2;
    for (int i = 0; i < nb; i++) { *ys[i] = y; y += hs[i] + gap; if (ys[i] == &L.y_cbar) { L.y_count = y - gap + 12; y += cnt; } }
    L.y_body = L.y_title + LHN * 30 + (L.body_b ? 12 : 10);
    if (q->ncards) { L.n = 2; for (int i = 0; i < 2; i++) { L.r[i][0] = b.W / 2 - 456 + i * 472; L.r[i][1] = L.y_main; L.r[i][2] = 440; L.r[i][3] = L.card_hgt; } }
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
        float x = cx - 328, w = 656; rrect(c, x * s, L.y_card * s, w * s, L.card_hgt * s, 16 * s, CARD, 1);   // (width 600 plus its padding: CSS's content box)
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
static void head_line(stv_canvas *c, const stv_view *v, box b, float y) {   // the narrow screens' head (Main.dc.html's): the icon 72, the name over the version, centred
    float s = b.s, cx = b.W / 2; int icon = v->icon && v->icon_w > 0 && v->icon_h > 0;
    char nm[400]; float nsz = 30 * s, nw = fit(nm, sizeof nm, v->name, 1, &nsz, -0.5f * s, (fminf(b.W - 128, 1000) - 92) * s) / s;
    float vw = text_w(v->version, 0, 15 * s, 0) / s, tw = fmaxf(nw, vw), x0 = cx - (92 + tw) / 2, ty = y + (72 - (LHN * 30 + 4 + LHN * 15)) / 2;
    if (icon) image(c, v->icon, v->icon_w, v->icon_h, x0 * s, y * s, 72 * s, 16 * s); else rrect(c, x0 * s, y * s, 72 * s, 72 * s, 16 * s, EDGE, 1);
    text_at(c, nm, (x0 + 92) * s, (ty + (LHN * 30 - LHN * nsz / s) / 2 + ASC * nsz / s) * s, 1, nsz, INK, -0.5f * s);
    line(c, v->version, (x0 + 92) * s, (ty + LHN * 30 + 4) * s, 0, 0, 15 * s, DIM, LEFT, tw * s, 0);
}
static void foot_at(stv_canvas *c, const char *t, box b, float bottom, float size) { line(c, t, b.W / 2 * b.s, (b.H - bottom - LHN * size) * b.s, 0, 0, size * b.s, DIM2, CENTER, (b.W - 64) * b.s, 0); }

typedef struct { float r[4][4], head_y, status_y, colx; } llay;   // the items (0..2) and Start (3); the narrow head's and statuses' tops; the wide left column's x
static llay ln_layout(box b, int wide) {
    llay L; memset(&L, 0, sizeof L);
    float ih = wide ? 60 : 64, mt = wide ? 24 : 32, list = 3 * ih + 3 * 10 + mt + 72, x, y;
    if (!wide) { float total = 72 + 36 + LHN * 14 + 36 + list; L.head_y = (b.H - total) / 2; L.status_y = L.head_y + 72 + 36; y = L.status_y + LHN * 14 + 36; x = b.W / 2 - 260; }
    else { L.colx = (b.W - 916) / 2; x = L.colx + 340 + 56; y = (b.H - list) / 2; }
    for (int i = 0; i < 3; i++) { L.r[i][0] = x; L.r[i][1] = y + i * (ih + 10); L.r[i][2] = 520; L.r[i][3] = ih; }
    L.r[3][0] = x; L.r[3][1] = y + 3 * (ih + 10) + mt; L.r[3][2] = 520; L.r[3][3] = 72;
    return L;
}
static void status_at(stv_canvas *c, const stv_status *st, float x, float top, float s, float gap, double t) {   // the icon (16) and the words (14), top: the line's
    step_icon(c, st->state, x * s, (top + (LHN * 14 - 16) / 2) * s, 16 * s, t);
    line(c, st->text, (x + 16 + gap) * s, top * s, 0, 0, 14 * s, st->state == STV_RUN ? INK : st->state == STV_FAIL ? WARN : DIM, LEFT, 0, 0);
}
static void badge_a(stv_canvas *c, float s, float cx, float cy) { disc(c, cx * s, cy * s, 14 * s, BG, 0.18f); line(c, "A", cx * s, (cy - LHN * 14 / 2) * s, 0, 1, 14 * s, BG, CENTER, 0, 0); }
static void draw_launcher(stv_canvas *c, const stv_view *v, double t) {
    box b = box_of(c->w, c->h); float s = b.s; int wide = wide_of(c->w, c->h); const stv_launcher *n = &v->ln; llay L = ln_layout(b, wide);
    if (!wide) {   // Launcher.dc.html: the head, the three statuses in a row, the items
        background(c, 1, c->w / 2.0f, (b.H / 2 - 100) * s, 600 * s, 110 * s, 0.16f, v->glow);
        head_line(c, v, b, L.head_y);
        float w[STV_NSTEPS], tot = 0;
        for (int i = 0; i < STV_NSTEPS; i++) { w[i] = 16 + 6 + text_w(n->status[i].text, 0, 14 * s, 0) / s; tot += w[i] + (i ? 22 : 0); }
        float x = b.W / 2 - tot / 2;
        for (int i = 0; i < STV_NSTEPS; i++) { status_at(c, &n->status[i], x, L.status_y, s, 6, t); x += w[i] + 22; }
    } else {   // LauncherWide.dc.html: the icon (88), the name (two lines at most), the version and the statuses in a column of 340 at the left, the items at the right
        background(c, 1, (L.colx + 108) * s, (b.H / 2 - 10) * s, 460 * s, 100 * s, 0.16f, v->glow);
        float lw = 340, nl = 1.25f * 28; char l1[400] = "", l2[400] = ""; const char *sp = NULL; float sz = 28 * s;
        if (text_w(v->name, 1, sz, -0.5f * s) > lw * s) for (const char *q = v->name; *q; q++) if (*q == ' ') { char tmp[400]; size_t k = (size_t)(q - v->name); if (k >= sizeof tmp) break;
            memcpy(tmp, v->name, k); tmp[k] = 0; if (text_w(tmp, 1, sz, -0.5f * s) <= lw * s) sp = q; else break; }
        if (sp) { size_t k = (size_t)(sp - v->name); memcpy(l1, v->name, k); l1[k] = 0; snprintf(l2, sizeof l2, "%s", sp + 1); } else snprintf(l1, sizeof l1, "%s", v->name);
        int lines = l2[0] ? 2 : 1; float h = 88 + 22 + lines * nl + 6 + LHN * 14 + 22 + 3 * LHN * 14 + 2 * 10, y = (b.H - h) / 2, x = L.colx;
        if (v->icon && v->icon_w > 0 && v->icon_h > 0) image(c, v->icon, v->icon_w, v->icon_h, x * s, y * s, 88 * s, 20 * s); else rrect(c, x * s, y * s, 88 * s, 88 * s, 20 * s, EDGE, 1);
        y += 88 + 22;
        line(c, l1, x * s, y * s, nl * s, 1, 28 * s, INK, LEFT, lw * s, -0.5f * s); if (lines > 1) line(c, l2, x * s, (y + nl) * s, nl * s, 1, 28 * s, INK, LEFT, lw * s, -0.5f * s);
        y += lines * nl + 6; line(c, v->version, x * s, y * s, 0, 0, 14 * s, DIM, LEFT, lw * s, 0); y += LHN * 14 + 22;
        for (int i = 0; i < STV_NSTEPS; i++) { status_at(c, &n->status[i], x, y, s, 8, t); y += LHN * 14 + 10; }
    }
    float ls = wide ? 17 : 18, ns = wide ? 13 : 14;
    for (int i = 0; i < 3; i++) {
        const float *r = L.r[i]; int sel = n->sel == i && !n->waiting;
        if (sel) choice_frame(c, s, r, 14, 1); else rrect(c, r[0] * s, r[1] * s, r[2] * s, r[3] * s, 14 * s, CARD, 0.85f);
        float lw = line(c, n->item[i], (r[0] + 28) * s, r[1] * s, r[3] * s, 0, ls * s, INK, LEFT, (r[2] - 56) * 0.55f * s, 0) / s;
        line(c, n->note[i], (r[0] + r[2] - 28) * s, r[1] * s, r[3] * s, 0, ns * s, DIM, RIGHT, (r[2] - 56 - lw - 16) * s, 0);
    }
    const float *r = L.r[3]; float k = 22 * s / 24, ix = (r[0] + 28) * s, iy = (r[1] + (r[3] - 22) / 2) * s;
    if (!n->waiting) {   // Start: the accent's fill, the play icon, the name, A; chosen: the soft ring around it
        if (n->sel == 3) halo(c, s, r, 16, ACC, 0.18f);
        rrect(c, r[0] * s, r[1] * s, r[2] * s, r[3] * s, 16 * s, ACC, 1);
        float p[6] = {ix + 7 * k, iy + 4.5f * k, ix + 7 * k, iy + 19.5f * k, ix + 20 * k, iy + 12 * k}; tri(c, p, BG);
        line(c, n->start, (r[0] + 28 + 22 + 16) * s, r[1] * s, r[3] * s, 1, 22 * s, BG, LEFT, (r[2] - 28 - 22 - 16 - 28 - 28 - 16) * s, 0);
        badge_a(c, s, r[0] + r[2] - 28 - 14, r[1] + r[3] / 2);
    } else {   // pressed before the start was ready (LauncherWide.dc.html): the accent's edge, a spinner, the name over the line that says it starts once ready
        choice_frame(c, s, r, 16, 1);
        step_icon(c, STV_RUN, ix, iy, 22 * s, t);
        float bh = LHN * 20 + 2 + LHN * 13, ty = r[1] + (r[3] - bh) / 2, tx = r[0] + 28 + 22 + 16;
        line(c, n->start, tx * s, ty * s, 0, 1, 20 * s, INK, LEFT, (r[2] - tx + r[0] - 28) * s, 0);
        line(c, n->start_sub, tx * s, (ty + LHN * 20 + 2) * s, 0, 0, 13 * s, DIM, LEFT, (r[2] - tx + r[0] - 28) * s, 0);
    }
    if (n->keys[0]) foot_at(c, n->keys, b, wide ? 24 : 36, wide ? 13 : 14);
}

typedef struct { float x0, y0, top, mh, note_y, rx, rw, card_y, card_h, r[5][4]; } clay;   // r: the modes (0..2), Upload now (3), Back to the cloud save (4)
static clay cp_layout(const stv_cloudpage *p, box b) {
    clay L; memset(&L, 0, sizeof L);
    L.x0 = (b.W - 1040) / 2; L.y0 = fmaxf(48, 96 + (b.H - 960) / 2); L.top = L.y0 + 80;
    float y = L.top + LHN * 14 + 2 + 12; L.mh = 44 + LHN * 19 + 4 + LHN * 14;
    for (int i = 0; i < 3; i++) { L.r[i][0] = L.x0; L.r[i][1] = y + i * (L.mh + 12); L.r[i][2] = 520; L.r[i][3] = L.mh; }
    L.note_y = y + 3 * L.mh + 2 * 12 + 12 + 4;
    L.rx = L.x0 + 560; L.rw = 480; L.card_y = y; int n = p->nrow < 0 ? 0 : p->nrow > 4 ? 4 : p->nrow;
    L.card_h = 40 + n * LHN * 15 + (n > 1 ? (n - 1) * 12 : 0);
    float uy = L.card_y + L.card_h + 12;
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
    box b = box_of(c->w, c->h); float s = b.s; const stv_cloudpage *p = &v->cp; clay L = cp_layout(p, b);
    background(c, 0, 0, 0, 0, 0, 0, 0);
    page_title(c, p->title, v->name, L.x0, 1040, L.y0, s);
    line(c, p->label[0], L.x0 * s, L.top * s, 0, 0, 14 * s, DIM, LEFT, 520 * s, 0);
    for (int i = 0; i < 3; i++) {   // the modes: a radio, the name (19 bold) over what it does (14); the one chosen with the accent's edge
        const float *r = L.r[i]; int sel = p->sel == i && !p->modes_off, on = p->checked == i;
        if (sel) edged(c, s, r, 16, 2, ACC, CARD_SEL); else edged(c, s, r, 16, 2, EDGE, CARD);
        float cx = r[0] + 26 + 11, cy = r[1] + 22 + 2 + 11;
        ring(c, cx * s, cy * s, 10 * s, 2 * s, on ? ACC : RADIO_OFF); if (on) disc(c, cx * s, cy * s, 5 * s, ACC, 1);
        float tx = r[0] + 26 + 22 + 16, tw = r[2] - (tx - r[0]) - 26;
        line(c, p->mode[i], tx * s, (r[1] + 22) * s, 0, 1, 19 * s, p->modes_off && !on ? DIM2 : INK, LEFT, tw * s, 0);
        line(c, p->desc[i], tx * s, (r[1] + 22 + LHN * 19 + 4) * s, 0, 0, 14 * s, p->modes_off && !on ? DIM2 : BODY, LEFT, tw * s, 0);
    }
    if (p->note[0]) para(c, p->note, L.x0 * s, L.note_y * s, 520 * s, 13 * s, 1.5f * 13 * s, DIM2, LEFT, 3);
    line(c, p->label[1], L.rx * s, L.top * s, 0, 0, 14 * s, DIM, LEFT, L.rw * s, 0);
    kv_card(c, s, L.rx, L.card_y, L.rw, L.card_h, p->nrow > 4 ? 4 : p->nrow, p->key, p->val, p->col, 24);
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
    if (p->keys[0]) foot_at(c, p->keys, b, 36, 14);
}

typedef struct { float x0, y0, lab1, lab2, note_y, sw[4], card[4], btn[2][4]; } alay;
static alay ap_layout(box b) {
    alay L; memset(&L, 0, sizeof L);
    L.x0 = (b.W - 720) / 2; L.y0 = fmaxf(56, 120 + (b.H - 960) / 2);
    float y = L.y0 + LHN * 30 + 28; L.lab1 = y; y += LHN * 14 + 10;
    float sh = 40 + LHN * 18 + 4 + 2 * 1.5f * 14; L.sw[0] = L.x0; L.sw[1] = y; L.sw[2] = 720; L.sw[3] = sh; y += sh + 28;
    L.lab2 = y; y += LHN * 14 + 10;
    float ch = 40 + 3 * LHN * 15 + 2 * 12; L.card[0] = L.x0; L.card[1] = y; L.card[2] = 720; L.card[3] = ch; y += ch + 10;
    for (int i = 0; i < 2; i++) { L.btn[i][0] = L.x0 + i * 365; L.btn[i][1] = y; L.btn[i][2] = 355; L.btn[i][3] = 56; }
    L.note_y = y + 56 + 10;
    return L;
}
static void draw_apppage(stv_canvas *c, const stv_view *v) {
    box b = box_of(c->w, c->h); float s = b.s; const stv_apppage *p = &v->ap; alay L = ap_layout(b);
    background(c, 0, 0, 0, 0, 0, 0, 0);
    page_title(c, p->title, v->name, L.x0, 720, L.y0, s);
    line(c, p->label[0], L.x0 * s, L.lab1 * s, 0, 0, 14 * s, DIM, LEFT, 720 * s, 0);
    const float *r = L.sw;   // the switch's card: the name (18), two lines (14, 1.5), the switch (56 x 32) at the right
    if (p->sel == 0) edged(c, s, r, 16, 2, ACC, CARD); else rrect(c, r[0] * s, r[1] * s, r[2] * s, r[3] * s, 16 * s, CARD, 1);
    line(c, p->quick, (r[0] + 24) * s, (r[1] + 20) * s, 0, 0, 18 * s, INK, LEFT, (r[2] - 48 - 56 - 20) * s, 0);
    for (int i = 0; i < 2; i++) line(c, p->quick_desc[i], (r[0] + 24) * s, (r[1] + 20 + LHN * 18 + 4 + i * 21) * s, 21 * s, 0, 14 * s, DIM, LEFT, (r[2] - 48 - 56 - 20) * s, 0);
    float tx = r[0] + r[2] - 24 - 56, ty = r[1] + (r[3] - 32) / 2;
    rrect(c, tx * s, ty * s, 56 * s, 32 * s, 16 * s, p->quick_on ? ACC : TRACK_OFF, 1);
    disc(c, (tx + (p->quick_on ? 28 : 4) + 12) * s, (ty + 4 + 12) * s, 12 * s, p->quick_on ? 0xFFFFFFu : BODY, 1);
    line(c, p->label[1], L.x0 * s, L.lab2 * s, 0, 0, 14 * s, DIM, LEFT, 720 * s, 0);
    kv_card(c, s, L.card[0], L.card[1], L.card[2], L.card[3], p->nrow > 3 ? 3 : p->nrow, p->key, p->val, p->col, 24);
    for (int i = 0; i < 2; i++) {
        const float *k = L.btn[i]; int off = i ? p->clear_off : p->verify_off;
        if (p->sel == i + 1) edged(c, s, k, 14, 2, ACC, CARD_SEL); else rrect(c, k[0] * s, k[1] * s, k[2] * s, k[3] * s, 14 * s, BTN, 1);
        line(c, i ? p->clear : p->verify, (k[0] + k[2] / 2) * s, k[1] * s, k[3] * s, 0, 16 * s, off ? DIM2 : INK, CENTER, (k[2] - 32) * s, 0);
    }
    if (p->note[0]) para(c, p->note, L.x0 * s, L.note_y * s, 720 * s, 13 * s, 1.5f * 13 * s, DIM2, LEFT, 2);
    if (p->keys[0]) foot_at(c, p->keys, b, 36, 14);
}
int stv_page_hit(const stv_view *v, int w, int h, float x, float y) {
    box b = box_of(w, h); float X = x / b.s, Y = y / b.s; const float *r[5]; int n = 0;
    llay ll; clay cl; alay al;
    if (v->screen == STV_SCREEN_LAUNCHER) { ll = ln_layout(b, wide_of(w, h)); for (int i = 0; i < 4; i++) r[n++] = ll.r[i]; }
    else if (v->screen == STV_SCREEN_CLOUD) { cl = cp_layout(&v->cp, b); for (int i = 0; i < 5; i++) r[n++] = cl.r[i]; }
    else if (v->screen == STV_SCREEN_APP) { al = ap_layout(b); r[n++] = al.sw; r[n++] = al.btn[0]; r[n++] = al.btn[1]; }
    for (int i = 0; i < n; i++) if (X >= r[i][0] && X < r[i][0] + r[i][2] && Y >= r[i][1] && Y < r[i][1] + r[i][3]) return i;
    return -1;
}

void stv_draw(stv_canvas *c, const stv_view *v, double t) {
    if (!c || !c->px || c->w <= 0 || c->h <= 0) return;
    if (v->ask && v->ask->ask) draw_ask(c, v->ask);
    else if (v->screen == STV_SCREEN_LAUNCHER) draw_launcher(c, v, t);
    else if (v->screen == STV_SCREEN_CLOUD) draw_cloudpage(c, v);
    else if (v->screen == STV_SCREEN_APP) draw_apppage(c, v);
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
