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
// ---- the launcher (Launcher.dc.html, LauncherWide.dc.html) and its two pages (CloudSettings.dc.html, AppSettings.dc.html) ----
enum { STV_SCREEN_STEPS, STV_SCREEN_LAUNCHER, STV_SCREEN_CLOUD, STV_SCREEN_APP, STV_SCREEN_LINK };   // LINK: the device link's page (LinkCode / LinkDone.dc.html)
enum { STV_COL_INK, STV_COL_GOOD, STV_COL_WARN, STV_COL_DIM };   // a value's colour
typedef struct { int state; char text[96]; } stv_status;          // a line of the launcher's top: its icon (STV_DONE a check, STV_RUN a spinner, STV_SKIP a dash, ...) and its words
typedef struct {
    stv_status status[STV_NSTEPS];       // the game data, the shaders, the cloud save
    char item[3][48], note[3][96];       // Play without the sync, Cloud save, App settings: the name and the words at its right
    char start[32], start_sub[96];       // Start's name; while it waits for the start to be ready (waiting): the line under it
    char keys[96];                       // the bottom line
    int sel, waiting;                    // sel: 0..2 the items, 3 Start
} stv_launcher;
typedef struct {
    char title[48], label[2][32];        // the page's title; the two columns' small titles
    char mode[3][32], desc[3][96]; int checked, modes_off;   // the three modes, the one in use; modes_off: none can be chosen now (note says why)
    char note[200];                      // under the modes
    int nrow; char key[4][32], val[4][96]; int col[4];      // the state's card: a name, its value (STV_COL_*)
    char up[48], up_note[64]; int up_off;                   // Upload now, the words at its right; up_off: nothing to send (or it cannot be now)
    char back[48], back_note[64]; int back_off, hold;       // Back to the cloud save (held 1.5 s): hold 0..1000 of it held so far
    int link;                            // 0: no link button; 1 not linked: Link this device at the top of the right column (no Upload now, no Back to the cloud save);
                                         // 2 linked: Unlink this app under the card
    char lbtn[48], lnote[96]; int larm;  // that button's name and the words at its right; larm: pressed once, the next press unlinks
    char keys[96]; int sel;              // sel: 0..2 the modes, 3 Upload now, 4 Back to the cloud save, 5 the link's button
} stv_cloudpage;
typedef struct {
    char title[48], label[2][32];        // the page's title; its two small titles (Start, Game data)
    char quick[48], quick_desc[2][96]; int quick_on;        // the start-at-once switch, two lines under its name
    int nrow; char key[3][32], val[3][96]; int col[3];      // the data's card
    char verify[48], clear[48]; int verify_off, clear_off;  // Check the data again, Clear the shader cache
    char note[200], keys[96]; int sel;   // the line under the buttons; sel: 0 the switch, 1 verify, 2 clear
} stv_apppage;
// the device link's page: BUSY a code is asked for (a spinner); CODE the QR code, the address, the code and the time left (LinkCode.dc.html); EXPIRED the same, the code
// faded and run out; FAIL no code (the server not reached, asked too often); DONE linked (LinkDone.dc.html: a check, the device's name, Continue)
enum { STV_LINK_BUSY, STV_LINK_CODE, STV_LINK_EXPIRED, STV_LINK_FAIL, STV_LINK_DONE };
typedef struct {
    int state;
    char title[96], line[160];           // the title; BUSY, FAIL, DONE: the line under it
    char qr[400];                        // the QR code's text
    char step1[128], host[128], step2[128], code[24], status[128];   // CODE, EXPIRED: the two steps (the address, the code) and the line of the wait
    int nbtn, sel; char btn[2][32];      // the buttons (sel the chosen one)
    int nrow; char key[3][32], val[3][96]; char foot[160];          // DONE: the card of rows, the line under it
    char keys[96];
} stv_linkpage;
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
    int screen;                               // STV_SCREEN_*: the steps (a start without the launcher, a download), or the launcher and its pages (name, version, icon above)
    int anim;                                 // the launcher: something moves (a spinner, a held button): drawn again soon
    stv_launcher ln; stv_cloudpage cp; stv_apppage ap; stv_linkpage lk;
} stv_view;

// The fonts: TrueType files the caller keeps until stv_release (bold NULL: the regular one for both), the pixel font pack (pixel_font.h) for code points they lack
int stv_font_set(int bold, const uint8_t *ttf, size_t n);
void stv_font_fallback(const uint8_t *pack);
void stv_release(void);   // the glyphs made, the background kept, the fonts forgotten: nothing of the screen's stays
void stv_draw(stv_canvas *c, const stv_view *v, double t);   // t: seconds, for the spinner
int stv_animated(const stv_view *v);   // a spinner turns: drawn again soon
// A touch at window pixels on the question q (or on the steps' bottom line when q is not a question but has a button): the card (kind 1) or button (kind 2) under it, -1 none
int stv_hit(const dm_cloud_view *q, int w, int h, float x, float y, int *kind);
// A touch at window pixels on the launcher or one of its pages (v->screen): what it would choose there (stv_launcher.sel, stv_cloudpage.sel, stv_apppage.sel,
// stv_linkpage.sel), -1 none. The cloud page's rectangles depend on cp.link, the link page's on lk.state and lk.nbtn
int stv_page_hit(const stv_view *v, int w, int h, float x, float y);
uint32_t stv_icon_colour(const uint32_t *rgba, int w, int h);   // the icon's representative colour, made bright enough to glow (0x5B9BFF for a grey one)

// The download's speed (the last 5 s) and time left, smoothed so neither jumps
typedef struct { int64_t t[48]; double b[48]; int n; double speed, eta; int64_t last; } stv_rate;
void stv_rate_reset(stv_rate *r);
void stv_rate_feed(stv_rate *r, int64_t t_ms, double done, double left);
void stv_fmt_eta(char *o, size_t n, double s, int ko);    // "약 9분 남음", "" when not known
void stv_fmt_speed(char *o, size_t n, double bps);        // "18 MB/s"
void stv_fmt_bytes(char *o, size_t n, uint64_t done, uint64_t total);   // "7.2 / 17.2 GB"
void stv_fmt_int(char *o, size_t n, long long v);         // "8,544"
