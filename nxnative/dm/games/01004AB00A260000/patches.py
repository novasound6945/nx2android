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
#
# 4:3 screens (ASPECT_4_3 below; Retroid Pocket Nova, unverified on a device): the game renders 960x720 instead of 1280x720. The UI is not covered by these edits: it is laid out in a
# fixed virtual 1280x720 space and came out 0.75 wide on a device (Nova, 2026-11-14); the UI stubs (ui_4_3 below) put it in a 16:9 band centred vertically. Every screen sized buffer of the RenderTargetMan (main+0xfb72c: Half, Quarter, Bloom,
# ReflectDepth, WaterHalfDepth, AO) is derived from the width / height getters (main+0x69c7c8 / main+0x69c838), so the width default is all that has to change. Two kinds of edits:
#   cuts     the settings default of Normal.Resolution.Width (main+0x69c608), the width getter default (main+0x69c7f4) and the handheld display mode width (main+0x711ec8): 1280 -> 960;
#            the static initial values of the width globals (main+0x1aeca70.. UI scale, int / float W, W/2; RenderTargetMan main+0x1ae3a90..) in .data: 1280 / 640 -> 960 / 480 (and 1.0 -> 0.75)
#   aspect   the projection command builder main+0xb74e8 (every perspective projection goes through it, 3 callers; s0 fov, s1 aspect): a stub replaces an aspect of exactly 16:9 (0x3fe38e39)
#            with float W / float H (main+0x1aeca7c / +0x1aeca84)
# Why the .data initial values and the aspect stub, not only the three defaults: a camera made before the settings function (main+0x69c5ac) has run, or one that gets 16:9 passed in directly
# (main+0x883980 holds a 16/9 constant), keeps 16:9 while the main picture is 4:3. The passes then project with different aspects, and the reflection and water passes, whose screen space
# coordinates are looked up from the main picture, are off (seen in another port of this game as reflections that do not sit on their object). The stub makes every pass use W/H.
# The docked display mode (main+0x711ee0, 1920 -> 1440) is not cut: untested. Not verified on a device (static analysis only; design in tools/dsr-aspect/REPORT.md).
# The switch is the rewrite's variant (DM_PATCH_VARIANT, set by the porter for each variant android.tsv declares: '' the game's own 16:9, '4x3' this): the porter rewrites main
# once per variant into mods/ and mods-4x3/ of one data pack, and the app picks one by the screen (front_android.c aspect_variant). The variant is part of the rewrite's
# reuse key (build_ipa_from_nsp.py rewrite), so one variant's output is never reused as another's.
import os as _os
_VARIANT = _os.environ.get('DM_PATCH_VARIANT', '')
if _VARIANT not in ('', '4x3'): raise ValueError(f'DM_PATCH_VARIANT={_VARIANT!r}: DARK SOULS: REMASTERED has the variants "" (16:9) and "4x3"')
ASPECT_4_3 = _VARIANT == '4x3'   # 4:3 screens (Retroid Pocket Nova): the game renders 960x720 with an undistorted UI and one aspect for every projection; else the game's own 1280x720
CUTS = {'df3766a2bb651a3e822828abf10b6f7b': [
    (0x69c608, '02a08052', '02788052', 'settings Normal.Resolution.Width default 1280 -> 960 (0x69c5ac: W, float W, W/2, W/1280 UI scale)'),
    (0x69c7f4, '02a08052', '02788052', 'width getter default 1280 -> 960 (RenderTargetMan 0xfb72c sizes Resolve/Half/Quarter/Bloom/AO from it)'),
    (0x711ec8, '01a08052', '01788052', 'handheld display mode width 1280 -> 960 (window size / crop)'),
    (0x711ee0, 'e10f1932', '01b48052', 'docked display mode width 1920 -> 1440 (the port runs docked: boost x1.5 renders 1440x1080, and a 1920 window left the scene and the UI squeezed into its left part)'),
    (0x1aeca70, '0000803f', '0000403f', 'static init: UI scale W/1280 (read before 0x69c5ac)'),
    (0x1aeca78, '00050000', 'c0030000', 'static init: int W'),
    (0x1aeca7c, '0000a044', '00007044', 'static init: float W (camera ctor 0xb9360 aspect = W/H)'),
    (0x1aeca88, '80020000', 'e0010000', 'static init: int W/2'),
    (0x1aeca8c, '00002044', '0000f043', 'static init: float W/2'),
    (0x1ae3a90, '00050000', 'c0030000', 'static init: RenderTargetMan W'),
    (0x1ae3a98, '0000a044', '00007044', 'static init: RenderTargetMan float W'),
    (0x1ae3aa0, '80020000', 'e0010000', 'static init: RenderTargetMan W/2'),
    (0x1ae3aa8, '00002044', '0000f043', 'static init: RenderTargetMan float W/2'),
]} if ASPECT_4_3 else {}
# Docked frame buffers (ASPECT_4_3 and DOCKED_FB_1440): main+0x705408.. allocates three render targets at a fixed 1920x1080 (mov w1,#1920 / mov w2,#1080: the docked window of
# the original, colour 37 and depth 130 on a device trace). Docked the game draws its last 2D pass into one of them with a viewport of the window (W = 1440 at 4:3) and the final
# copy to the window samples the whole buffer (UV 0..1), so at 4:3 the picture came out 0.75 wide with 480 px of empty buffer as a black band (Nova, pass trace 2026-11-14).
# Allocated at 1440 they match the docked window. Handheld draws a 960 viewport into them (worked with 1920; unverified with 1440)
DOCKED_FB_1440 = True
if ASPECT_4_3 and DOCKED_FB_1440:
    CUTS['df3766a2bb651a3e822828abf10b6f7b'] += [
        (0x70558c, 'e10f1932', '01b48052', 'docked frame buffer (x19+0x78) width 1920 -> 1440 (the 4:3 docked window; the final copy samples the whole buffer)'),
        (0x705624, 'e10f1932', '01b48052', 'docked frame buffer (x19+0x88) width 1920 -> 1440'),
        (0x705868, 'e10f1932', '01b48052', 'docked frame buffer (x19+0xc0) width 1920 -> 1440'),
    ]

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
    if ASPECT_4_3:
        # Projection aspect (4:3 above): main+0xb74e8 writes every perspective projection command (s0 fov, s1 aspect, s2 near, s3 far). A 16:9 aspect (0x3fe38e39) becomes float W / float H
        # (main+0x1aeca7c / +0x1aeca84, one page); any other aspect passes. Independent of the frame step state: it runs at any fps. The site is a function entry, so x16 / x17 / v16 / v17 and
        # the flags are dead there (the original instruction is stp d11,d10,[sp,#-0x40]!, position independent; it is executed on both paths)
        fs_site(0xb74e8, 'eb2bbc6d')
        start = here()
        for w in (0x1e260030,                    # fmov w16,s1            aspect bits
                  0x52800000 | 0x8e39 << 5 | 17,  # mov w17,#0x8e39
                  0x72a00000 | 0x3fe3 << 5 | 17,  # movk w17,#0x3fe3,lsl #16    16:9
                  0x6b11021f):                    # cmp w16,w17
            emit(w)
        orig = start + 4 * 9                                               # nine words from start (the b.ne is the fifth)
        emit(0x54000000 | (((orig - here()) >> 2) & 0x7FFFF) << 5 | 1)    # b.ne orig
        emit(ADRP(16, here(), 0x1aeca7c))                                  # x16 = the page of float W / float H (main+0x1aeca7c, +0x1aeca84)
        emit(0xbd400000 | (0x1aeca7c & 0xfff) // 4 << 10 | 16 << 5 | 16)   # ldr s16,[x16,#0xa7c]   float W
        emit(0xbd400000 | (0x1aeca84 & 0xfff) // 4 << 10 | 16 << 5 | 17)   # ldr s17,[x16,#0xa84]   float H
        emit(0x1e201800 | 17 << 16 | 16 << 5 | 1)                          # fdiv s1,s16,s17        aspect = W / H
        assert here() == orig; emit(0x6dbc2beb); emit(B(here(), 0xb74ec))          # orig: stp d11,d10,[sp,#-0x40]!; back
        fs_put(0xb74e8, [B(0xb74e8, start)], 'aspect: a 16:9 projection aspect becomes float W / float H (main+0x1aeca7c / +0x1aeca84), any other passes')
        ui_4_3(here, emit, fs_site, fs_put)

# ---- 4:3 UI (ASPECT_4_3; design and evidence in tools/dsr-aspect/UI-REPORT.md) ----
# The menu / HUD 2D is laid out in a virtual 1280x720 space (times the dock scale ds, main+0x1b305c0: 1.0 handheld, 1.5 docked). The menu renderer's camera (main+0x57127c, "MENU REND",
# built once) is an orthographic projection of a fixed 0..1280 x 0..720 (main+0x571750 bl main+0xb5774; docked, the 2D batch main+0xfa99c uses its own 0..1280ds x 0..720ds), so on a
# W x H picture a virtual point lands at px (x W/1280, y H/720): at 960x720 the UI came out 0.75 wide and 1.0 tall. The menu's clip (scissor) rectangle, though, goes to the GPU as
# virtual x ds unconverted (set main+0xfa560 from main+0x5727c8, applied by main+0xfa5f8), which equals the px only at 16:9: at 960 wide the text drawn into a clipped box (floor messages,
# the start menu's tab names) fell left of its own clip and vanished. Two stages, chosen by UI_STAGE:
#   1  scissor  main+0xfa5f8 converts the clip rectangle as the projection does: x * W / (1280 ds), y * H / (720 ds). The UI stays 0.75 wide, the text comes back
#   2  band     the UI keeps its proportions in a 16:9 band centred vertically: virtual height Hv = H * 1280 / W (960 at 4:3), off = (Hv - 720) / 2 (120 at 4:3; 0 at 16:9 or wider, so
#               every stub is then a no-op). The menu camera projects y -off .. 720 + off (main+0x571750; docked, the 2D matrix of main+0xfa99c the same); the scissor takes y as
#               (y / ds + off) * W / 1280; the world -> UI conversions (screen px / (W/1280), main+0x4b3ebc / +0x4b4784 / +0x4b4be8) subtract off from y; and the filled quad
#               helper (main+0xf853c) stretches a quad spanning the whole 720 to -off .. 720 + off, so fades, dims and full screen backgrounds cover the whole picture, not the band
#               (on the Nova the outline helper main+0xf7710 had this first: the fade stayed in the band; the band, the scissor and the world -> UI stubs worked, 2026-11-14)
# UI_TEXT_AREA (separate, unconfirmed): three text objects (main+0xcd2ac, +0xf9458, and the shared measuring one of main+0x572a24) take a layout area (0, 0, int W, int H), the picture's
# px, which at 16:9 equals the virtual 1280ds x 720ds; True gives them 1280ds x 720ds. Who reads that area is not known: turn it on if text right of virtual x 960 (px 720 at 960 wide)
# is still cut off with the scissor fixed
UI_STAGE = 2          # 0: none; 1: scissor only (the UI 0.75 wide); 2: scissor + band (undistorted UI in a centred 16:9 band; band confirmed on the Nova 2026-11-14)
UI_TEXT_AREA = False  # tried on the Nova with the band (scissor not yet fixed): no effect on the missing text, which was the scissor; kept off
def _ui_off(here, emit, d, t):
    # s<d> = max(0, (float H * 1280 / float W - 720) / 2); s<t>, x16, x17 scratch
    emit(ADRP(16, here(), 0x1aeca7c))
    emit(0xbd400000 | (0xa84 // 4) << 10 | 16 << 5 | d)   # ldr s<d>,[x16,#0xa84]   float H
    emit(0xbd400000 | (0xa7c // 4) << 10 | 16 << 5 | t)   # ldr s<t>,[x16,#0xa7c]   float W
    emit(0x1e201800 | t << 16 | d << 5 | d)               # fdiv s<d>,s<d>,s<t>
    emit(0x52a00000 | 0x44a0 << 5 | 17); emit(0x1e270000 | 17 << 5 | t)   # mov w17,#0x44a00000; fmov s<t>,w17   1280
    emit(0x1e200800 | t << 16 | d << 5 | d)               # fmul s<d>,s<d>,s<t>    Hv
    emit(0x52a00000 | 0x4434 << 5 | 17); emit(0x1e270000 | 17 << 5 | t)   # 720
    emit(0x1e203800 | t << 16 | d << 5 | d)               # fsub s<d>,s<d>,s<t>
    emit(0x1e2c1000 | t)                                  # fmov s<t>,#0.5
    emit(0x1e200800 | t << 16 | d << 5 | d)               # fmul
    emit(0x1e2703e0 | t)                                  # fmov s<t>,wzr
    emit(0x1e204800 | t << 16 | d << 5 | d)               # fmax s<d>,s<d>,s<t>

def _ui_scissor(here, emit, fs_site, fs_put, site, orig, rx, ry, band):
    # main+0xfa5f8 reads the clip rect (l, t, r, b; virtual x ds) as two ldp; each ldp becomes: the original load, then x * kx and y * ky (+ oy). Between the two ldp a cmp / csel pair
    # is pending (cmp w9,w10 before, csel w9 after the second ldp): no instruction below sets the flags. Scratch x16, x17, v26..v29 (unused in the function)
    fs_site(site, orig); start = here()
    emit(ADRP(16, here(), 0x1aeca7c)); emit(ADRP(17, here(), 0x1b305c0))
    emit(0xbd400000 | (0x5c0 // 4) << 10 | 17 << 5 | 28)   # ldr s28,[x17,#0x5c0]   ds
    emit(0xbd400000 | (0xa7c // 4) << 10 | 16 << 5 | 26)   # ldr s26,[x16,#0xa7c]   float W
    emit(0x52a00000 | 0x44a0 << 5 | 17); emit(0x1e270000 | 17 << 5 | 27)   # s27 = 1280
    emit(0x1e200800 | 28 << 16 | 27 << 5 | 27)             # fmul s27,s27,s28       1280 ds
    emit(0x1e201800 | 27 << 16 | 26 << 5 | 26)             # fdiv s26,s26,s27       kx = W / (1280 ds)
    if band:
        _ui_off(here, emit, 27, 29)                        # s27 = off
        emit(0x1e200800 | 26 << 16 | 27 << 5 | 27)         # fmul s27,s27,s26
        emit(0x1e200800 | 28 << 16 | 27 << 5 | 27)         # fmul s27,s27,s28       oy = off W / 1280 (ky = kx)
    else:
        emit(0xbd400000 | (0xa84 // 4) << 10 | 16 << 5 | 27)   # ldr s27,[x16,#0xa84]   float H
        emit(0x52a00000 | 0x4434 << 5 | 17); emit(0x1e270000 | 17 << 5 | 29)   # s29 = 720
        emit(0x1e200800 | 28 << 16 | 29 << 5 | 29)         # fmul s29,s29,s28       720 ds
        emit(0x1e201800 | 29 << 16 | 27 << 5 | 27)         # fdiv s27,s27,s29       ky = H / (720 ds)
    emit(int.from_bytes(bytes.fromhex(orig), 'little'))   # the original ldp
    emit(0x1e200800 | 26 << 16 | rx << 5 | rx)             # fmul s<rx>,s<rx>,s26
    if band:
        emit(0x1e200800 | 26 << 16 | ry << 5 | ry)         # fmul s<ry>,s<ry>,s26
        emit(0x1e202800 | 27 << 16 | ry << 5 | ry)         # fadd s<ry>,s<ry>,s27
    else:
        emit(0x1e200800 | 27 << 16 | ry << 5 | ry)         # fmul s<ry>,s<ry>,s27
    emit(B(here(), site + 4))
    fs_put(site, [B(site, start)], f'ui 4:3 scissor (main+0xfa5f8): the clip rect to px like the projection ({"band" if band else "x W/1280ds, y H/720ds"})')

def ui_4_3(here, emit, fs_site, fs_put):
    S, T = 26, 27   # float scratch: v26 / v27 (and x16 / x17) appear nowhere in the functions patched below; every site is mid-function, so flags are only touched where dead
    def back(site): emit(B(here(), site + 4))
    band = UI_STAGE >= 2
    if UI_STAGE >= 1:
        _ui_scissor(here, emit, fs_site, fs_put, 0xfa620, '0005402d', 0, 1, band)   # ldp s0,s1,[x8]      l, t
        _ui_scissor(here, emit, fs_site, fs_put, 0xfa630, '0009412d', 0, 2, band)   # ldp s0,s2,[x8,#8]   r, b
    if band:
        # band: the menu camera's orthographic projection (s0 0 / s1 720 = y, s2 0 / s3 1280 = x, s4 / s5 z) -> y -off .. 720 + off
        fs_site(0x571750, '0910ed97'); start = here()
        _ui_off(here, emit, S, T)
        emit(0x1e203800 | S << 16 | 0 << 5 | 0)             # fsub s0,s0,s<S>
        emit(0x1e202800 | S << 16 | 1 << 5 | 1)             # fadd s1,s1,s<S>
        emit(0x94000000 | ((0xb5774 - here()) >> 2) & 0x3FFFFFF)   # bl main+0xb5774 (the original call)
        back(0x571750)
        fs_put(0x571750, [B(0x571750, start)], 'ui 4:3 band: the menu camera projects y -off .. 720 + off (virtual 1280 x Hv, the 720 layout centred)')
        # band, docked: main+0xfa99c builds its own orthographic matrix when docked (s1 1280ds, s0 720ds, top 0): fsub s5,s4,s0 (top - bottom) -> -(720ds + 2 off ds); bottom + top stays 720ds
        fs_site(0xfacc4, '8538201e'); start = here()
        _ui_off(here, emit, S, T)
        emit(0x52a00000 | 0x4434 << 5 | 17); emit(0x1e270000 | 17 << 5 | T)   # 720
        emit(0x1e201800 | T << 16 | 0 << 5 | T)             # fdiv s<T>,s0,s<T>       ds
        emit(0x1e200800 | T << 16 | S << 5 | S)             # fmul s<S>,s<S>,s<T>     off ds
        emit(0x1e202800 | S << 16 | S << 5 | 5)             # fadd s5,s<S>,s<S>
        emit(0x1e202800 | 0 << 16 | 5 << 5 | 5)             # fadd s5,s5,s0
        emit(0x1e214000 | 5 << 5 | 5)                       # fneg s5,s5
        back(0xfacc4)
        fs_put(0xfacc4, [B(0xfacc4, start)], 'ui 4:3 band (docked 2D matrix): top - bottom = -(720ds + 2 off ds), the y range -off .. 720 + off')
        # world -> UI: y (virtual, from the picture's top) - off
        fs_site(0x4b45e4, '22040c5e'); start = here()       # mov s2,v1.s[1]   (then stp s1,s2 = x, y)
        _ui_off(here, emit, S, T)
        emit(0x5e0c0422)                                    # mov s2,v1.s[1]
        emit(0x1e203800 | S << 16 | 2 << 5 | 2)             # fsub s2,s2,s<S>
        emit(0x6e0c0441)                                    # mov v1.s[1],v2.s[0]
        back(0x4b45e4)
        fs_put(0x4b45e4, [B(0x4b45e4, start)], 'ui 4:3 world -> UI (main+0x4b3ebc): y - off')
        fs_site(0x4b4bb4, '2800261e'); start = here()       # fmov w8,s1        (s1 = x, s2 = y, packed to x8 after)
        _ui_off(here, emit, S, T)
        emit(0x1e203800 | S << 16 | 2 << 5 | 2)             # fsub s2,s2,s<S>
        emit(0x1e260028)                                    # fmov w8,s1
        back(0x4b4bb4)
        fs_put(0x4b4bb4, [B(0x4b4bb4, start)], 'ui 4:3 world -> UI (main+0x4b4784): y - off')
        fs_site(0x4b4d7c, '011ca04e'); start = here()       # mov v1.16b,v0.16b (v0 = x, y, z; stored to x22+4)
        _ui_off(here, emit, S, T)
        emit(0x5e0c0400 | 0 << 5 | T)                       # mov s<T>,v0.s[1]
        emit(0x1e203800 | S << 16 | T << 5 | T)             # fsub s<T>,s<T>,s<S>
        emit(0x6e0c0400 | T << 5 | 0)                       # mov v0.s[1],v<T>.s[0]
        emit(0x4ea01c01)                                    # mov v1.16b,v0.16b
        back(0x4b4d7c)
        fs_put(0x4b4d7c, [B(0x4b4d7c, start)], 'ui 4:3 world -> UI (main+0x4b4be8): y - off')
        # edges: the filled quad helper main+0xf853c (6 callers: the batch's sprite / fill commands; 4 corners). main+0xf7710, patched here before, is the outline helper (8 corners:
        # the rect and the rect inset by s0, the line width) and is left alone. A quad whose y (after its 2D matrix, virtual) spans the whole 720 (top <= 0 and bottom >= 720: fades,
        # dims, full screen backgrounds) gets its top moved up by off and its bottom down by off; any other quad, partly off screen or not, is drawn as before.
        # Entry (ldp s1,s2,[x2]: the rect l,t,r,b): w15 = 1 if min(y(l,t), y(r,b)) <= 0 and max >= 720, y(x,y) = x m01 + y m11 + m31 (x0 +0x64, +0x74, +0x94; z is 0).
        # A quad wholly above y 0 or below 720 was off the original 16:9 screen; the band would show it (the loading screen's second bonfire, Nova 2026-11-14), so its corners
        # all get y 0 (no area). x15 / x16 / x17 and v26..v29 appear nowhere in the function; flags are dead at all five sites
        fs_site(0xf853c, '4108402d'); start = here()
        L = lambda t, n, off: emit(0xbd400000 | (off // 4) << 10 | n << 5 | t)   # ldr s<t>,[x<n>,#off]
        L(26, 2, 0); L(28, 0, 0x64); emit(0x1e200800 | 28 << 16 | 26 << 5 | 26)       # l m01
        L(27, 2, 4); L(28, 0, 0x74); emit(0x1e200800 | 28 << 16 | 27 << 5 | 27)       # t m11
        emit(0x1e202800 | 27 << 16 | 26 << 5 | 26); L(28, 0, 0x94); emit(0x1e202800 | 28 << 16 | 26 << 5 | 26)   # yA
        L(27, 2, 8); L(29, 0, 0x64); emit(0x1e200800 | 29 << 16 | 27 << 5 | 27)       # r m01
        L(29, 2, 12); L(28, 0, 0x74); emit(0x1e200800 | 28 << 16 | 29 << 5 | 29)      # b m11
        emit(0x1e202800 | 29 << 16 | 27 << 5 | 27); L(28, 0, 0x94); emit(0x1e202800 | 28 << 16 | 27 << 5 | 27)   # yB
        emit(0x1e205800 | 27 << 16 | 26 << 5 | 28)          # fmin s28,s26,s27
        emit(0x1e204800 | 27 << 16 | 26 << 5 | 29)          # fmax s29,s26,s27
        emit(0x2a1f03ef)                                    # mov w15,wzr
        emit(0x1e2703fa)                                    # fmov s26,wzr
        emit(0x1e3a23a0)                                    # fcmp s29,s26
        emit(0x54000000 | 12 << 5 | 0x9)                    # b.ls cull (+12)    bottom <= 0: above the screen (unordered: no)
        emit(0x52a00000 | 0x4434 << 5 | 17); emit(0x1e27023a)   # mov w17,#720.0; fmov s26,w17
        emit(0x1e3a2380)                                    # fcmp s28,s26
        emit(0x54000000 | 8 << 5 | 0xa)                     # b.ge cull (+8)     top >= 720: below the screen
        emit(0x1e3a23a0)                                    # fcmp s29,s26
        emit(0x54000000 | 7 << 5 | 0xb)                     # b.lt done (+7)     bottom < 720: not full height
        emit(0x1e2703fa)                                    # fmov s26,wzr
        emit(0x1e3a2380)                                    # fcmp s28,s26
        emit(0x54000000 | 4 << 5 | 0xc)                     # b.gt done (+4)     top > 0: not full height
        emit(0x320003ef)                                    # mov w15,#1         full height
        emit(0x14000002)                                    # b done
        emit(0x321f03ef)                                    # cull: mov w15,#2   wholly off the 720 (16:9) screen
        emit(0x2d400841)                                    # done: ldp s1,s2,[x2] (the original)
        back(0xf853c)
        fs_put(0xf853c, [B(0xf853c, start)], 'ui 4:3 edges (quad helper main+0xf853c, entry): w15 = 1 the quad spans the whole 720 in y, 2 it lies wholly above 0 or below 720')
        for site, orig, y, why in ((0xf858c, '0110241e', 2, 'corner 1'), (0xf8624, '420140bd', 3, 'corner 2'),
                                   (0xf86b4, '6308211e', 3, 'corner 3'), (0xf873c, '0008211e', 0, 'corner 4')):
            fs_site(site, orig); start = here()
            emit(0x34000000 | 28 << 5 | 15)                 # cbz w15,orig (+28)
            emit(0x710009ff)                                # cmp w15,#2
            emit(0x54000000 | 3 << 5 | 0x1)                 # b.ne rule (+3)
            emit(0x1e2703e0 | y)                            # fmov s<y>,wzr      off screen: every corner at y 0, no area
            emit(0x14000000 | 24)                           # b orig (+24)
            _ui_off(here, emit, S, T)                       # rule: (14 words)
            emit(0x1e2703e0 | T)                            # fmov s<T>,wzr
            emit(0x1e202000 | T << 16 | y << 5)             # fcmp s<y>,s<T>
            emit(0x54000000 | 2 << 5 | 0xc)                 # b.gt +2
            emit(0x1e203800 | S << 16 | y << 5 | y)         # fsub s<y>,s<y>,s<S>
            emit(0x52a00000 | 0x4434 << 5 | 17); emit(0x1e270000 | 17 << 5 | T)   # 720
            emit(0x1e202000 | T << 16 | y << 5)             # fcmp s<y>,s<T>
            emit(0x54000000 | 2 << 5 | 0xb)                 # b.lt +2
            emit(0x1e202800 | S << 16 | y << 5 | y)         # fadd s<y>,s<y>,s<S>
            assert here() == start + 4 * 28; emit(int.from_bytes(bytes.fromhex(orig), 'little'))
            back(site)
            fs_put(site, [B(site, start)], f'ui 4:3 edges (quad helper main+0xf853c, {why}): a full-height quad: y <= 0 -> y - off, y >= 720 -> y + off; a quad off the 16:9 screen: y 0')
    if UI_TEXT_AREA:
        # text: layout area 1280ds x 720ds instead of int W x int H. The site scvtf s8,s0 (W) gets both, the later scvtf s9,s0 (H) becomes a nop; a call (the object's lock) follows both
        for s8_site, s9_site, fn in ((0xcd778, 0xcd784, 'main+0xcd2ac'), (0xf9828, 0xf9834, 'main+0xf9458'), (0x572b2c, 0x572b38, 'main+0x572a24')):
            fs_site(s8_site, '08d8215e'); fs_site(s9_site, '09d8215e'); start = here()
            emit(ADRP(16, here(), 0x1b305c0))
            emit(0xbd400000 | (0x5c0 // 4) << 10 | 16 << 5 | T)   # ldr s<T>,[x16,#0x5c0]   dock scale
            emit(0x52a00000 | 0x44a0 << 5 | 16); emit(0x1e270000 | 16 << 5 | 8); emit(0x1e200800 | T << 16 | 8 << 5 | 8)   # s8 = 1280 ds
            emit(0x52a00000 | 0x4434 << 5 | 16); emit(0x1e270000 | 16 << 5 | 9); emit(0x1e200800 | T << 16 | 9 << 5 | 9)   # s9 = 720 ds
            back(s8_site)
            fs_put(s8_site, [B(s8_site, start)], f'ui 4:3 text ({fn}): layout area 1280ds x 720ds (was int W x int H, the picture)')
            fs_put(s9_site, [0xd503201f], f'ui 4:3 text ({fn}): nop (s9 set by the stub above)')

FRAME_STEP = {'df3766a2bb651a3e822828abf10b6f7b': frame_step}
