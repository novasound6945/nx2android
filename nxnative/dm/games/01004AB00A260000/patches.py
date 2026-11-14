# DARK SOULS: REMASTERED (01004AB00A260000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# CUTS, OBS and FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
from nso_asm import ADRP, B
#
# 60 fps through the frame step (30 on the Switch), 1.0.3 (v196608) main, build df3766a2bb651a3e822828abf10b6f7b. Two constants hold the game at 30:
#   main+0x706fa4 mov w1,#30 (321f0fe1)         frame limiter target. The game presents at interval 1; its render thread (HGCommandDispatcher, main+0x8a4b28) ends each frame in a CPU
#                                               limiter, main+0x707090(this, fps): 1000 / fps ms less the time since its last call (nn::os::GetSystemTick), slept out when more than 2 ms
#                                               are left; the main thread waits for the render thread. The 30 is a constant at the only call.
#   main+0x667904 ldr s8,[x8,#0x6ec] (bd46ed08) frame update step, 1/30 from rodata (+0x834 holds 1/60): the main thread's frame function (main+0x6678d0) loads it into s8 once and
#                                               hands it as v0 to the frame update (main+0x6a337c) and to every subsystem update after it; x8 (the adrp's page) is dead after the load.
# History: cuts to mov w1,#60 and ldr s8,[x8,#0x834] (a fixed 1/60 step) were tried and taken out (Legion Y700, 2026-11-06): the frame work sat at the limit (p50 16.7 ms, half the
# frames over 16.7, 0-133 of 300 dropped; DM_NVN_ASYNC=1 did not help: the game's own CPU work is the limit), the Android adaptive frame rate stepped down to 40 fps, and with a
# fixed 1/60 step the whole game then ran at two thirds speed (a long run stayed at 40 for minutes). Climbing ladders also misplayed at 60 with that fixed step (as with the
# community's 60 fps cheat for this build); with the time really elapsed as the step, ladders played right on a device.
# Now (FRAME_STEP below, a float step): the update takes the host's step in seconds (state page +0x38, hle_nvn.c fstep_tick, nvn_fstep.h nvn_fstepf_next), the time the frame
# really stays on screen: 1/60 at 60 fps, 1/40 at the adaptive rate's 40, 1/30 at 30, with late frames paid back over the next ones, so game time = real time at any rate.
# The limiter takes the target fps (state +0) while that step runs. At target 30 (DM_FPS_TARGET=30) the host leaves +0x38 at 0, and both stubs run the game's own instructions:
# its 1/30 step and its 30 fps limiter. At target 0 the host does not step frames at all
CUTS = {}

# Both sites are followed by a bl before x16 / x17 / v16 are read again (mov v0,v8; bl main+0x6a337c and mov x0,x19; bl main+0x707090), so the stubs take those three as scratch;
# every other register ends as the original instruction leaves it (x8 stays the adrp's page on both paths of the step stub)
def frame_step(ctx):
    here, emit, fs_site, fs_put, FS_STATE_OFF = ctx.here, ctx.emit, ctx.site, ctx.put, ctx.state
    ctx.float_step = 1   # fixups FSTEP_FLOAT: the host writes the float step at +0x38
    fs_site(0x667904, '08ed46bd')
    start = here(); emit(ADRP(16, here(), FS_STATE_OFF))
    for w in (0xb9400211,   # ldr w17,[x16]          target fps (float bits; 0 = off)
              0x340000b1,   # cbz w17,orig (+5)
              0xb9403a11,   # ldr w17,[x16,#0x38]    float step in seconds (0 = not decided yet, or the game's own)
              0x34000071,   # cbz w17,orig (+3)
              0x1e270228):  # fmov s8,w17
        emit(w)
    emit(B(here(), 0x667908))                                        # back
    emit(0xbd46ed08); emit(B(here(), 0x667908))                      # orig: ldr s8,[x8,#0x6ec] (1/30); back
    fs_put(0x667904, [B(0x667904, start)], 'frame step: the frame update steps by the host\'s float step in seconds (state +0x38; 0 or target 0 = the game\'s 1/30)')
    fs_site(0x706fa4, 'e10f1f32')
    start = here(); emit(ADRP(16, here(), FS_STATE_OFF))
    for w in (0x321f0fe1,   # mov w1,#30             the game's own target
              0xb9400211,   # ldr w17,[x16]          target fps (float bits; 0 = off)
              0x340000b1,   # cbz w17,back (+5)
              0xb9403a11,   # ldr w17,[x16,#0x38]    float step (0: the update takes 1/30, so the limiter keeps 30)
              0x34000071,   # cbz w17,back (+3)
              0xbd400210,   # ldr s16,[x16]
              0x1e380201):  # fcvtzs w1,s16          the target (60)
        emit(w)
    emit(B(here(), 0x706fa8))                                        # back
    fs_put(0x706fa4, [B(0x706fa4, start)], 'frame step: the render thread\'s frame limiter at the target fps while the float step runs (state +0, +0x38; else the game\'s 30)')

FRAME_STEP = {'df3766a2bb651a3e822828abf10b6f7b': frame_step}
