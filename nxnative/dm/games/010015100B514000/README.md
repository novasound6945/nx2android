<!-- game facts: made by docs/gen_tables.py from android.tsv; edit android.tsv, not this block -->
# Super Mario Bros. Wonder

![Super Mario Bros. Wonder](../../../../docs/images/super-mario-bros-wonder.jpg)

| | |
|---|---|
| Status | **playable** |
| Title ID | `010015100B514000` |
| Tested with nx2android | `v0.1.3` |
| Tested build and device | v327680 Lenovo Legion Y700 TB322FC (Snapdragon 8 Elite, Adreno 830, Android 16) |
| App package | `org.nx2a.supermariobroswonder` |

The profile sets these for you (`android.tsv`); a player changes the pause menu ones in the game, and the env rows in the data pack's `env.txt`:

| Row | What it does |
|---|---|
| `DM_GPU_TURBO=1` | "auto" or 1: ask the GPU for its high clocks while the game is GPU-bound; "need": only while the game is held below its target; unset or 0 = no |

Read the profile's porting log in the comment header of [`android.tsv`](android.tsv). Back to the [games list](../../../../README.md#games).
<!-- end of the game facts -->

## Game version

- Update 1.2.1.
- The profile was checked with `v327680` (the `tested` row of android.tsv).

## Frame rate

- 60 fps, dips to 40.

## Tested devices

| Device | GPU | Vulkan driver | Game version | nx2android | Result |
|---|---|---|---|---|---|
| Lenovo Legion Y700 TB322FC (Snapdragon 8 Elite, Adreno 830, Android 16) | Adreno 830 | not recorded | 1.2.1 (v327680) | `v0.1.3` | playable |

## Options

- Not recorded.

## Quirks and known issues

- Not recorded.
