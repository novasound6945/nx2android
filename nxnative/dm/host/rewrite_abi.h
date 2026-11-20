// REWRITE_ABI: the number of the contract between the rewriter's output (dm/tools/patch_nso.py: <module>.patched.bin and its fixups / gates / sites / cuts / obs tables)
// and the host that runs it. The contract is what the rewritten code and its tables expect of the host: the fixups header keys and the key page (thread slots K18, KTLS,
// KTLR, KGATE and how a slot is reached per DM_TARGET: tpidrro_el0 on Apple, tpidr_el0[2] on Android), x18 kept in its slot, the gate block's entry and return (gates.tsv rows
// by return point, the gate kinds and their arguments), the HLE entry the gates call, the sites / cuts / obs table formats, the OBS_* numbers of hle.h, the frame step state
// page's layout (FRAME_STEP stubs) and the patch area's placement (PATCH_OFF, FAR).
//
// The only place the number is defined. patch_nso.py writes it into every <module>.fixups.tsv (# REWRITE_ABI), porter records it in the data pack's nx2a-data.json
// (rewrite_abi), the host (loader.c, front_android.c) and porter apk-update compare a pack's number with this one, and refuse a pack of another number.
//
// Raise it only when the contract really changes: when a host of the new number cannot run a module rewritten at the old one, or the reverse (a slot access, a gate's calling
// sequence, a table column, an OBS number renumbered, a fixups key the host now requires). Not for a fix in which sites are rewritten or how (the output changes, the contract
// does not), nor for a new fixups key an older host may ignore (FSTEP_FLOAT, RENDER_SCALE: the host reads it when it is there). A raise makes every data pack of the old number
// need a port again from the game's files, so porter apk-update (a new host into a released APK, the data pack kept) refuses them.
// Packs from before the number was written carry none: porter apk-update judges them by the rewriter's git history (dm/tools/rewrite_abi.py COMMITS), the host warns.
#pragma once
#define DM_REWRITE_ABI 1
