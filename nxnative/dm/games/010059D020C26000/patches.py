# MARVEL Cosmic Invasion (010059D020C26000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# CUTS, OBS and FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
CUTS = {
  # MARVEL Cosmic Invasion 1.10 (v589824) main: FNA3D's OpenGL driver asks SDL for a desktop OpenGL context and then wants the desktop entry points ("OpenGL 2.1 support is required!").
  # The hosts give it an OpenGL ES 3 context. FNA3D takes the ES 3 path (the ES profile for the context, the ES entry points, MojoShader's glsles3 shaders) when the SDL hint
  # FNA3D_OPENGL_FORCE_ES3 is set, read in its window attribute setup (main+0x1ae470) as SDL_GetHintBoolean(name, false). The game's SDL keeps an environment of its own, empty on the
  # console, so neither getenv nor the SDK's environ reaches the hint; the cut makes the default true
  '4b8064a4a57b1ec333220daf331d31de': [
    (0x1ae498, 'e1031f2a', '21008052', 'FNA3D window attributes: the default of the hint FNA3D_OPENGL_FORCE_ES3, mov w1,wzr → mov w1,#1 (an OpenGL ES 3 context instead of desktop OpenGL)'),
  ],
}
OBS = {
  # MARVEL Cosmic Invasion 1.10 main: FNA3D's OpenGL driver, CreateTexture2D (main+0x1aa7c4). x19 is the renderer from main+0x1aa7f0 on; the gate hands it to the host, which runs
  # the driver's list of other threads' GL calls when the main thread waits (dm/host/fork_fna3d_pump.c, DM_FNA3D_GL_PUMP). In the function's head, before its first call
  '4b8064a4a57b1ec333220daf331d31de': [
    (0x1aa7f4, 25, '2a0203f6', 'FNA3D OpenGL CreateTexture2D, after mov x19, x0: the renderer for the host; orig mov w22, w2'),
  ],
}
