# ONE PIECE: Grand Gourmet (01008BF025F34000), update 1.0.4 (v196608): code patches by main's build ID.
# CUTS as described in dm/tools ("Registered patches").
#
# 4:3 screens (the rewrite's variant "4x3": android.tsv `variant 4x3`, mods-4x3/ in the data pack, picked by the app on a screen narrower than 3:2;
# Retroid Pocket Nova, 1280x960). Kairosoft's games lay themselves out for the screen size Unity reports, so one source of that size is changed:
# the Switch ScreenManager's GetCurrentResolution (main+0x72dc20, vtable 0x4215650 slot +0x178) answers (1920,1080) docked / (1280,720) handheld, picked by
# the operation mode (main+0x7f2100 == 1) from two shared rodata literals (main+0x3538af8 / main+0x353a5b8). GfxDeviceNvn's init (main+0x7d08f4) asks it for
# the window size, adds the display with it (main+0x7f1ad0), and the active display's size becomes the ScreenManager's width / height (+0xa4 / +0xa8:
# SetActiveDisplay main+0x7f1c60), which Screen.width / height read and the window crop takes (main+0x7e9e1c). The cut builds 1440x1080 / 960x720 in
# registers instead of loading the literals (the literals are shared, so they are left alone).
import os as _os
_VARIANT = _os.environ.get('DM_PATCH_VARIANT', '')
if _VARIANT not in ('', '4x3'): raise ValueError(f'DM_PATCH_VARIANT={_VARIANT!r}: ONE PIECE: Grand Gourmet has the variants "" (16:9) and "4x3"')
ASPECT_4_3 = _VARIANT == '4x3'
CUTS = {'3c29c4b3ad35087ea52a1bc31d8d3c07': [
    (0x72dc38, 'e8179f1a000d040e687001b001dd42fd487001f0027d45fd880780d22800c0f2680600f900543f0f00a8a00e401c612e600200fd',
               '087880520ab480524801881a095a80520a8780524901891a68260029880780d22800c0f2680600f91f2003d51f2003d51f2003d5',
     'ScreenManager GetCurrentResolution: 1440x1080 docked / 960x720 handheld (4:3 at the same lines) instead of the 16:9 literals'),
]} if ASPECT_4_3 else {}
