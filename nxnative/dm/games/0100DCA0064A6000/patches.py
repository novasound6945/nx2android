# Luigi's Mansion 3 (0100DCA0064A6000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# CUTS, OBS and FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
OBS = {
  # Luigi's Mansion 3 1.4.0 (v327680) main: the job system's helping wait (main+0x7a9130, and its twin in main+0x7a9c..): loop { job = TryPop(worker, queue) (main+0x7a91e0);
  # if none: WaitLightEvent or SleepThread(0); } until the group says it is done. A job that is not ready comes back from TryPop at once, so the loop spins on a big core for the
  # rest of the frame (dm/host/fork_poll_wait.c). The gates are at the two loop heads, each before the call's arguments are set (no x9-x17 read before the bl)
  '79e5950ffa85acf63e28c9aec051eac2': [
    (0x7a917c, 26, 'aa1403e0', 'job system helping wait, loop head: a fruitless poll backs off in the host; orig mov x0, x20'),
    (0x7a9d04, 26, 'aa1503e0', 'job system helping wait (twin), loop head: a fruitless poll backs off in the host; orig mov x0, x21'),
  ],
}
