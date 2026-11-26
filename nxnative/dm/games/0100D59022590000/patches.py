# Scott Pilgrim EX (0100D59022590000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# CUTS, OBS and FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py.
# This fork's, derived from MARVEL Cosmic Invasion's (010059D020C26000/patches.py): FNA3D's OpenGL driver in this main is that game's instruction for instruction (PC-relative words
# aside), 0x8f30 bytes further on
CUTS = {
  # Scott Pilgrim EX 1.0.0.13755 (v65536) main: FNA3D's OpenGL driver asks SDL for a desktop OpenGL context; the hosts give it an OpenGL ES 3 context. FNA3D takes the ES 3 path when
  # the SDL hint FNA3D_OPENGL_FORCE_ES3 is set, read in its window attribute setup (main+0x1b73a0) as SDL_GetHintBoolean(name, false) (main+0x6bc0, SDL linked into main). The game's
  # SDL keeps an environment of its own, empty on the console, so the cut makes the default true
  'e8dc5c9e4f0810e7282dc821310140c8': [
    (0x1b73c8, 'e1031f2a', '21008052', 'FNA3D window attributes: the default of the hint FNA3D_OPENGL_FORCE_ES3, mov w1,wzr → mov w1,#1 (an OpenGL ES 3 context instead of desktop OpenGL)'),
    # Startup (main+0x29c0, called from main+0x1448) loads rom:/nro/EOSSDK-Switch-Shipping.nro with ro::LoadModule and aborts when it fails (Result 0x216 at main+0x2b48). The host
    # never loads an online SDK (hle_ro.c) and the main's EOS_* imports stay host stubs (dm/host/fork_eos_offline.c), as for MARVEL Cosmic Invasion, whose startup goes on when
    # the load fails; here the failure branch to the abort becomes a nop, so the function returns as after a load
    (0x2b1c, '20010035', '1f2003d5', 'startup: cbnz w0 after ro::LoadModule of the EOS SDK NRO (to the abort) → nop (the host refuses online SDKs; the game plays offline)'),
  ],
}
OBS = {
  # Scott Pilgrim EX 1.0.0.13755 main: FNA3D's OpenGL driver, CreateTexture2D (main+0x1b36f0). x19 is the renderer from main+0x1b3720 on; the gate hands it to the host, which runs
  # the driver's list of other threads' GL calls when the main thread waits (dm/host/fork_fna3d_pump.c, DM_FNA3D_GL_PUMP). In the function's head, before its first call
  'e8dc5c9e4f0810e7282dc821310140c8': [
    (0x1b3724, 25, '2a0203f6', 'FNA3D OpenGL CreateTexture2D, after mov x19, x0: the renderer for the host; orig mov w22, w2'),
  ],
}
