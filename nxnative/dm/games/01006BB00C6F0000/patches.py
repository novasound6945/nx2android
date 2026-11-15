# The Legend of Zelda: Link's Awakening (01006BB00C6F0000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# CUTS, OBS and FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
CUTS = {
  # The Legend of Zelda: Link's Awakening 1.1.0 (v131072) main: the physics step (main+0x15730, main thread) rebuilds each world's PhysX completion task in place
  # (a PxLightCpuTask at world+0xc8) and passes it to simulate. A worker runs it (main+0x10ccf80: fetchResults, world+0x1d8 done = 1, notify, unlock) and then calls its
  # release (main+0x10cd070), which calls the task's own destructor: vtable back to the PxLightCpuTask base, mTm = 0 (main+0x10cced0). At 30 fps the game steps twice
  # per frame (step, wait, step, wait); the unlock hands the world's mutex to the woken main thread, and when the worker is descheduled there the main thread's
  # next step rebuilds the task before the late destructor runs, which then wipes the new task. The next simulation's NpSceneCompletion::release reads its mTm = 0:
  # crash at main+0x10ccf28 (lr main+0x13c236c; seen on the iPhone and in 2 of 5 Mac runs, in every Mac run under CPU load). On the Switch the release follows the
  # unlock within nanoseconds. The release becomes a plain ret: the destructor only resets the vtable and mTm (no resources), the step rebuilds every field, and this
  # release is used by this one vtable (main+0x1c51308 slot 7) only
  '909e904af78ac1b8deefe97ab2ccdb51': [
    (0x10cd070, '080040f9', 'c0035fd6', 'physics completion task release (calls its own destructor): ldr x8,[x0] → ret (no longer destroyed; the step function rebuilds it in place every step)'),
    # The base actor constructor (main+0x1bdb0) copies the actor type from its create parameters (u16, +0x38 → +0xa0) and reads that type's category byte from the actor
    # type table (main+0x1bcc898, 0x275 entries of 0x58 bytes, +0x49) at main+0x1c16c with no bounds check; every other reader of the table checks the type against 0x275
    # first (main+0x238f8, main+0x8e0d1c, main+0x8e1e3c). The main camera ("MainCamera_1P", made at boot through main+0xbdaa0, which sets its type to 0xffff = none at
    # main+0xbdb68) reads table + 0xffff × 0x58 + 0x49 = main+0x214c889, past the module's end (0x1cda000 with bss). On the Switch the modules lie one after another and the
    # read lands in subsdk0 (offset 0x472889, the byte 0x03), and the Mac host's ascending mappings put subsdk0 there too (0x7b); Android's mmap places each module below
    # the one before, nothing is mapped after main, and the game crashed with SIGSEGV at main+0x1c180 half a second into nnMain (Retroid Pocket 6, 2026-11-15). The
    # category goes into a hash map with the actor (main+0x397f4 → main+0x321e0); 0x03 and 0x7b both played to Marin and Tarin's house, so the camera does not depend on it.
    # The type is loaded sign-extended: 0xffff becomes -1, the read lands one entry before the table inside main's data (0 in the image), and types 0..0x274 read as before
    (0x1c16c, '68424179', '68428179', "base actor constructor, the actor type's category: ldrh w8,[x19,#0xa0] → ldrsh x8,[x19,#0xa0] (type 0xffff = none reads inside main, not past its end)"),
  ],
}
