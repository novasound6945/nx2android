// The start screen (before the game: the data download, the shader preparation, the save's cloud sync) as one picture of its state: startscreen.c draws a stv_view into
// an RGBA canvas the size of the window, with Noto Sans KR (assets/font/start-*.ttf, rasterised by stb_truetype) and the overlay's pixel font for what that font lacks.
// No Android here: datasync_android.c owns the window and the thread, and tests/test_start_screen.py renders the screens on the computer (the
// design's mock-ups are 1280 px wide; everything scales with the window).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "savecloud.h"

typedef struct { uint32_t *px; int w, h; } stv_canvas;   // RGBA bytes in memory, opaque
enum { STV_WAIT, STV_RUN, STV_DONE, STV_SKIP, STV_FAIL };   // a step's state
enum { STV_DATA, STV_SHADERS, STV_CLOUD, STV_NSTEPS };
typedef struct { int state; char name[48], note[96]; } stv_step;
typedef struct {
    char name[160], version[128];             // the game's name and "1.4.1 · build 1"
    const uint32_t *icon; int icon_w, icon_h; // the game's icon (RGBA); NULL: its place stays empty
    uint32_t glow;                            // 0xRRGGBB: the icon's colour, behind the screen
    stv_step step[STV_NSTEPS];
    int bar;                                  // -1: none, else 0..1000: the progress block above the steps (a download, the preparation)
    char bar_title[96], bar_left[96], bar_right[96];
    char hint[200], hint2[200]; int hint_warn;   // the line under the steps (hint_warn: an error, in orange) and a smaller one under it
    char foot[160];                           // the line at the bottom (B's hint)
    const dm_cloud_view *ask;                 // a question of the cloud's start: drawn instead of the steps (NULL: none)
} stv_view;

// The fonts: TrueType files the caller keeps until stv_release (bold NULL: the regular one for both), the pixel font pack (pixel_font.h) for code points they lack
int stv_font_set(int bold, const uint8_t *ttf, size_t n);
void stv_font_fallback(const uint8_t *pack);
void stv_release(void);   // the glyphs made, the background kept, the fonts forgotten: nothing of the screen's stays
void stv_draw(stv_canvas *c, const stv_view *v, double t);   // t: seconds, for the spinner
int stv_animated(const stv_view *v);   // a spinner turns: drawn again soon
// A touch at window pixels on the question q (or on the steps' bottom line when q is not a question but has a button): the card (kind 1) or button (kind 2) under it, -1 none
int stv_hit(const dm_cloud_view *q, int w, int h, float x, float y, int *kind);
uint32_t stv_icon_colour(const uint32_t *rgba, int w, int h);   // the icon's representative colour, made bright enough to glow (0x5B9BFF for a grey one)

// The download's speed (the last 5 s) and time left, smoothed so neither jumps
typedef struct { int64_t t[48]; double b[48]; int n; double speed, eta; int64_t last; } stv_rate;
void stv_rate_reset(stv_rate *r);
void stv_rate_feed(stv_rate *r, int64_t t_ms, double done, double left);
void stv_fmt_eta(char *o, size_t n, double s, int ko);    // "약 9분 남음", "" when not known
void stv_fmt_speed(char *o, size_t n, double bps);        // "18 MB/s"
void stv_fmt_bytes(char *o, size_t n, uint64_t done, uint64_t total);   // "7.2 / 17.2 GB"
void stv_fmt_int(char *o, size_t n, long long v);         // "8,544"
