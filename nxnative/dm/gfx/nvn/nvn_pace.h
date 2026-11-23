// Present pacer (pure function, shared by Mac / iOS; unit test tests/t_pace.c).
// Semantics as vsync: present slot = previous present slot + period. Early → wait until the present slot; late by less than one period → present immediately and keep the original phase;
// behind by one period or more (stutter) → restart the cadence from the current time, no burst of presents to catch up. Period 0 = no wait (the present slot follows the current time).
#pragma once
#include <stdint.h>

typedef struct { uint64_t target; } nvn_pacer;

// now: monotonic clock (ns); period: this frame's period (ns, 0 = no wait). Returns the ns to wait (0 = present immediately) and updates the internal target time.
static inline uint64_t nvn_pace(nvn_pacer *p, uint64_t now, uint64_t period) {
    if (!period || !p->target) { p->target = now; return 0; }
    uint64_t next = p->target + period;
    if (now <= next) { p->target = next; return next - now; }
    p->target = now - next < period ? next : now;
    return 0;
}

// The game's present interval as presentation follows it in game mode (hle_nvn.c present_itv): run counts the presents in a row that asked for 2 or more
// (nvn_pace_itv_run, this one included). The first of a run is not held: a dynamic interval controller (Super Mario Bros. Wonder's sead framework) asks for 2
// on the one frame after a long one and 1 again before the next. A run of two or more is followed from its second present; lone_hold: every one, as the Switch does
static inline int nvn_pace_itv_run(int interval, int run) { return interval > 1 ? run + 1 : 0; }
static inline int nvn_pace_game_itv(int interval, int run, int lone_hold) { return interval > 1 && (run > 1 || lone_hold) ? interval : 1; }
