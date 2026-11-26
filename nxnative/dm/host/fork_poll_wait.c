// This fork's: a job system's "help while waiting" loop as a spin wait.
// Luigi's Mansion 3's job system waits for a job group in a loop that tries to take a job and asks the group whether it is done (main+0x7a917c and its twin at main+0x7a9d04):
// TryPop, a virtual "done?" call, back to the head. The loop calls functions, so the rewriter's spin wait rule (patch_nso.py "Spin waits": loads and branches only) does not take it,
// and while a group's job is not ready to run (the frame's present, a barrier that waits for the other workers) TryPop gives it back at once: the main thread and a worker ran at
// 70-90% of a big core each at the title screen (40 fps, frame work = the whole frame period), which put a Retroid Pocket Nova's CPU at 95 °C within a minute.
// An observation gate at the loop's head (OBS_POLL_WAIT in the game's patches.py) hands each pass to dm_spin_wait, as a spin gate does: passes with no SVC and at most 20 µs of
// guest work between them are one wait, which polls for its first 2 ms and then sleeps with the yield back-off (at most 250 µs a pass, DM_YIELD_BACKOFF). A pass that ran a job
// starts a new wait, so work is not slowed; only a fruitless poll gives its core up.
#include "dm.h"
#include "hle.h"
#include <stdint.h>

void dm_spin_wait(uint64_t gate, uint64_t site);   // hle_thread.c
static void obs_poll(const uint64_t *regs) { dm_spin_wait(regs[30], regs[30]); }   // (x30: the wait function's caller, in the game's main: names the module in the yield lines)
__attribute__((constructor)) static void poll_gate(void) { gate_obs_set(OBS_POLL_WAIT, obs_poll); }
