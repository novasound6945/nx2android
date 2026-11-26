<!-- game facts: made by docs/gen_tables.py from android.tsv; edit android.tsv, not this block -->
# The Legend of Zelda: Echoes of Wisdom

![The Legend of Zelda: Echoes of Wisdom](../../../../docs/images/zelda-echoes-of-wisdom-rp6.png)

| | |
|---|---|
| Status | **playable** |
| Title ID | `01008CF01BAAC000` |
| Tested with nx2android | `v0.4.0` |
| Tested build and device | v196608 Retroid Pocket 6 (Adreno 740, PurpleVK) |
| App package | `org.nx2a.t01008cf01baac000` |

The profile sets these for you (`android.tsv`); a player changes the pause menu ones in the game, and the env rows in the data pack's `env.txt`:

| Row | What it does |
|---|---|
| `DM_VK_SUBGROUP_LACK=0x30` | this fork's: subgroup classes (`VkSubgroupFeatureFlagBits`: shuffle 0x10, shuffle relative 0x20) a fragment shader is taken not to have, though the device … |
| `DM_GPU_TURBO=1` | "auto" or 1: ask the GPU for its high clocks while the game is GPU-bound; "need": only while the game is held below its target; unset or 0 = no |
| `DM_FPS_PIN=60` | pin the governor at 60, 40 or 30 fps; 0 = no pin |

Read the profile's porting log in the comment header of [`android.tsv`](android.tsv). Back to the [games list](../../../../README.md#games).
<!-- end of the game facts -->

## Game version

- Version 1.1.0.
- The profile was checked with `v196608` (the `tested` row of android.tsv).

## Frame rate

- Not recorded.

## Tested devices

| Device | GPU | Vulkan driver | Game version | nx2android | Result |
|---|---|---|---|---|---|
| Retroid Pocket 6 (Adreno 740, PurpleVK) | Adreno 740 | PurpleVK | 1.1.0 (v196608) | `v0.4.0` | playable |

Playable on Retroid Pocket 6 with PurpleVK.

## Options

- Tested with the PurpleVK Vulkan driver (`--vulkan-driver`).

## Quirks and known issues

- **work in progress**: minor transparency artifacts around rifts.
