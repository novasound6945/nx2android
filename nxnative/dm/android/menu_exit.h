// The pause menu's Exit game (this fork's, upstream PR #463): the button row (the last, Resume's) is split at its midline, Exit game in its left half, Resume in
// its right. Everything of it is here and in menu_exit_android.inc (front_android.c: the hold, the end of the app) and menu_exit_draw.inc (gfx_vk_present.c: the
// row's drawing), so the upstream menu code (overlay_layout.h, front_android.c, gfx_vk_present.c) carries only the calls into them. After overlay_layout.h
#pragma once
#include <stdint.h>
#include "overlay_layout.h"
static inline int ov_menu_exit_at(float x, float y, float W, float H) {   // (x, y) on Exit game's half (front_android.c menu_touch; the drawing splits the row at the same midline)
    float p[4]; ov_menu_rect(W, H, p);
    return ov_menu_row_at(x, y, W, H) == OVM_ROWS - 1 && x < p[0] + p[2] / 2;
}
// Exit game ends the game only when held OV_EXIT_HOLD_NS (a finger on its half, or A with it chosen): a tap, or a slip of the finger onto it, never does. While held its
// half fills red from the left and reads "Hold to exit"; let go before the end, or the finger off its half, and it is as it was. A tap (let go within OV_EXIT_TAP_NS) shows
// "Hold to exit" for OV_EXIT_HINT_NS instead. Times: CLOCK_MONOTONIC ns (front_android.c runs it, a thread of its own times the hold)
#define OV_EXIT_HOLD_NS 1500000000ll
#define OV_EXIT_TAP_NS 500000000ll
#define OV_EXIT_HINT_NS 1500000000ll
enum { OV_EXIT_IDLE, OV_EXIT_HINT, OV_EXIT_HOLDING, OV_EXIT_GOING };   // what the half shows: Exit game, Hold to exit (the tap's hint, or held), Exiting...
typedef struct { int64_t t0, hint; int going; } ov_exit_hold;   // t0: held since (0 not held); hint: the hint shows until; going: the hold completed
static inline void ov_exit_press(ov_exit_hold *h, int64_t now) { if (!h->going) { h->t0 = now ? now : 1; h->hint = 0; } }
static inline void ov_exit_release(ov_exit_hold *h, int64_t now) {   // let go before the end: a tap shows the hint, a longer hold is just let go
    if (!h->t0 || h->going) return;
    h->hint = now - h->t0 < OV_EXIT_TAP_NS ? now + OV_EXIT_HINT_NS : 0; h->t0 = 0;
}
static inline void ov_exit_leave(ov_exit_hold *h) { if (!h->going) { h->t0 = 0; h->hint = 0; } }   // the finger off the half, the menu closed: as it was
// the state now and how far the hold is (0..1); OV_EXIT_GOING once, when the hold completes (the caller ends the game)
static inline int ov_exit_tick(ov_exit_hold *h, int64_t now, float *f) {
    *f = 0;
    if (h->going) { *f = 1; return OV_EXIT_GOING; }
    if (h->t0) {
        int64_t d = now - h->t0; if (d < 0) d = 0;
        if (d >= OV_EXIT_HOLD_NS) { h->going = 1; h->t0 = 0; *f = 1; return OV_EXIT_GOING; }
        *f = (float)d / OV_EXIT_HOLD_NS; return OV_EXIT_HOLDING;
    }
    if (h->hint && now < h->hint) return OV_EXIT_HINT;
    h->hint = 0; return OV_EXIT_IDLE;
}
int dm_menu_exit(float *f, int *chosen);   // front_android.c: the half's state (OV_EXIT_*), the hold's progress, whether a controller has it chosen (else Resume)
unsigned dm_menu_exit_ver(void);           // changes with them (the panel is drawn again)
