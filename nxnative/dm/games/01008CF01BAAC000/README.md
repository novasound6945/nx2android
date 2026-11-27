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
| `DM_VK_GPL_PREP_MAX=5000` | a shader bundle of more vertex and fragment shaders than this is not prepared whole: the parts earlier runs listed are made at the start, a shader's parts when … |
| `DM_VK_ZEROCOPY_LAZY=2` | which zero-copy pools get their buffer only when the GPU first uses them (vk_pool_of, pool_back: a slab's slot or a kept buffer as at creation; until then the … |
| `DM_VK_ZC_CHUNK_MB=32` | the chunk size in MB that a memory pool over it is cut into (unset: 256 MB, or the largest of 128 / 64 / 32 MB that this gralloc takes when it refuses 256 MB, … |

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

- Rifts: the rift mask left a house's door and the paths around it uncovered, and the rift looked brighter than on the console (the minor transparency
  artifacts noted on Retroid Pocket 6). This fork's host copies a depth-stencil texture's stencil too (`gfx_vk_exec_copies.inc`): the game copies its
  scene depth into the rifts' depth-stencil texture every frame, and the stencil marks of earlier frames had stayed there. Fixed on Retroid Pocket Nova.
