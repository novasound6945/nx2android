<!-- game facts: made by docs/gen_tables.py from android.tsv; edit android.tsv, not this block -->
# Dark Souls: Remastered

![Dark Souls: Remastered](../../../../docs/images/dark-souls-remastered.jpg)

| | |
|---|---|
| Status | **playable** |
| Title ID | `01004AB00A260000` |
| Tested with nx2android | `v0.4.0` |
| Tested build and device | v196608 Lenovo Legion Y700 TB322FC (Snapdragon 8 Elite, Adreno 830, Android 16) |
| App package | `org.nx2a.darksoulsremastered` |

The profile sets these for you (`android.tsv`); a player changes the pause menu ones in the game, and the env rows in the data pack's `env.txt`:

| Row | What it does |
|---|---|
| `DM_VK_ASYNC_PIPE=0` | 0 = new pipelines are built on the replay thread, not in the background |
| `DM_FPS_PIN=60` | pin the governor at 60, 40 or 30 fps; 0 = no pin |
| `DM_VK_DYN_UBO=0` | no dynamic uniform-buffer offsets (descriptor reuse drops) |

Read the profile's porting log in the comment header of [`android.tsv`](android.tsv). Back to the [games list](../../../../README.md#games).
<!-- end of the game facts -->

## Game version

- Update 1.0.3.
- The profile was checked with `v196608` (the `tested` row of android.tsv).

## Frame rate

- 30 fps.

## Tested devices

| Device | GPU | Vulkan driver | Game version | nx2android | Result |
|---|---|---|---|---|---|
| Lenovo Legion Y700 TB322FC (Snapdragon 8 Elite, Adreno 830, Android 16) | Adreno 830 | not recorded | 1.0.3 (v196608) | `v0.4.0` | playable |

## Options

- Not recorded.

## Quirks and known issues

- Not recorded.
