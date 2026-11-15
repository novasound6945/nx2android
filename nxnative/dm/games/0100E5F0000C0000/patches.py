# Disgaea 5: Alliance of Vengeance (0100E5F0000C0000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a
# module's NSO build ID; CUTS, OBS and FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
OBS = {
  # Disgaea 5 1.0.3 (v196608) main: the movie player's output queues (player at main+0x7c9d5e8; video {index, time, flags} vector at +0x430, audio at +0x448). The main thread
  # takes the front entry once a frame under the game's mutex (main+0x1ecbac: +0xa10 audio, +0xa68 video) and erases it; the decoder event thread (main+0x1ebdd0) appends with
  # no lock, and an append inside an erase lost one entry and doubled another, which showed pictures out of order for the rest of the opening movie (dm/host/hle_movie.c,
  # "Disgaea 5 1.0.3's movie player"). These gates put both sides under one host lock: the event thread from the start of its output event to its next wait, the main thread
  # from its lock to its unlock. Each gate sits right after a bl or before one with x9–x17 unread
  '48f474f4a87011d37e2ce7f015216fe6': [
    (0x1ebf38, 22, 'f9400ae0', 'video output event (main+0x1ebf1c, after movie::Buffer::Buffer): the event thread acquires and appends from here; orig ldr x0, [x23, #0x10]'),
    (0x1ec214, 22, 'f94006e0', 'audio output event (main+0x1ec20c, after ClearEvent): the event thread acquires and appends from here; orig ldr x0, [x23, #8]'),
    (0x1ebe7c, 24, 'aa1303e0', 'event loop head, before WaitAny: every event handler ends here, the appends are done; orig mov x0, x19'),
    (0x1ecbf4, 23, 'f9422ae9', 'main thread, audio queue (after LockMutex +0xa10): the take begins; orig ldr x9, [x23, #0x450]'),
    (0x1ecda4, 24, '9100e3e0', 'main thread, audio queue (after UnlockMutex): the take is done; orig add x0, sp, #0x38'),
    (0x1ecdc0, 23, 'f9421ee8', 'main thread, video queue (after LockMutex +0xa68): the take begins; orig ldr x8, [x23, #0x438]'),
    (0x1ecee4, 24, '910003e0', 'main thread, video queue (after UnlockMutex): the take is done; orig mov x0, sp'),
  ],
}
