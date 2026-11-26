# Cadence of Hyrule: Crypt of the NecroDancer featuring The Legend of Zelda

Title ID `01000B900D8B0000`. Read the profile's porting log in the comment header of [`android.tsv`](android.tsv).

## Game version

- Update 1.5.0 (`v458752`); the base game, the update and the DLC dumps are in one folder.

## Frame rate

- The title runs at 60 fps. The first overworld screen runs at 60 fps on the Retroid Pocket Nova (300 frames shown, none dropped, p99 16.67 ms; GPU 3.2 ms a frame).
- Before `DM_VK_INVAL_KEEP=1` the same screen ran at 30 fps (1,223 render passes a frame, GPU 20.7 ms).

## Tested devices

| Device | GPU | Vulkan driver | Game version | nx2android | Result |
|---|---|---|---|---|---|
| Retroid Pocket Nova | Adreno 740 | PurpleVK | 1.5.0 (`v458752`) | this fork, 2026-11-26 | plays: the title, a new game, the intro movie and the first overworld screen, in Korean |

## Options

- Korean with `-l ko` (`--language ko`).
- Tested with the PurpleVK Vulkan driver (`--vulkan-driver`).

## Quirks and known issues

- Not checked by ear: sound and the rhythm timing.
- Dungeons, long sessions and the Retroid Pocket 6 are not tested.
