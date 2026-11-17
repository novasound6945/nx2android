# Multiple Games Porting notes: Minecraft Dungeons I/II, Link's Awakening, Fire Emblem Engage, Dark Souls Remastered, Unicorn Overlord, 13 Sentinels, Disgaea 4/5 (Windows + Android, Turnip)

Hints from the field, not patches.

These are field notes from porting about ten games with nx2android on a Windows PC, tested on two
handhelds. They are offered as hints: what broke, what the cause turned out to be, and what fixed or
worked around it. Nothing here is a pull request. Where I have a local change I say so and can share
the diff if it is useful; where the cause is not known I say that too.

**Setup.** Windows 11 host (no Mac, so no heap check and no `DM_PROBE`). Devices: Retroid Pocket 6
(Snapdragon 8 Gen 2, Adreno 740, Android 13, 8 GB) and Retroid Pocket Nova. Vulkan driver: Mesa Turnip
(the "PurpleVK T30" build) unless noted; the stock Qualcomm driver was used for comparisons.

**Games.** A-Train, The War of Genesis: Remnants of Gray, 13 Sentinels (Asia), Unicorn Overlord (Asia),
Dark Souls Remastered, Disgaea 4, Disgaea 5 (new, playable), Link's Awakening, Fire Emblem Engage,
Minecraft Dungeons (new, playable), Minecraft Dungeons II (new, not booting yet).

Each item says how sure it is: **confirmed** (reproduced, then fixed and re-tested on a device),
**worked around** (symptom gone, cause unknown), or **open**.

---

## 1. Runtime (host) findings

### 1.1 A game can open two audio renderers (Minecraft Dungeons) — confirmed
- Unreal Engine 4.22, SDK 7.7. The game opens a small renderer (48 kHz, 4 voices, used by the movie
  player) and then its sound engine's (32 kHz, 64 voices, a submix, 3 effects).
- `hle_audren.c` drives one: the log says `audren: a second renderer (the host drives only one)`.
- Effect: no game sound, and the first level never finishes loading. The game thread, collecting
  garbage, waits for a voice of the renderer that never runs (backtrace through the Switch audio
  device code; all 34 threads parked).
- Local change: per-renderer state (handle, render thread, output, latest frame, catch-up queue,
  3:2 resampler state, the replacement event) in an array of four; an `AudioRenderSystem` is matched
  to its renderer by the render thread that drives it (thread-local), or by open order when an update
  arrives before the first render. The layout facts, the command list reader and the render lock stay
  shared. With it both renderers render and the level loads and plays at 30 fps.

### 1.2 GPL fast link + dynamic uniform buffers on Turnip — confirmed
- With independent sets (vertex = set 0, fragment = set 1) and both stages using
  `UNIFORM_BUFFER_DYNAMIC`, the fragment library is compiled against a layout whose set 0 is NULL, so
  after linking the fragment stage's dynamic offsets start at the wrong index and it reads the vertex
  stage's constants.
- Dark Souls Remastered: whole objects flash magenta / red / yellow for the 1 to 4 frames a
  fast-linked pipeline is drawn before the whole pipeline replaces it.
- Fire Emblem Engage: the same thing shows as a GPU hang, then `VK_ERROR_DEVICE_LOST` on scene
  changes (three times in 30 minutes).
- Fix that held: with GPL on, only the vertex stage (set 0) uses dynamic uniform buffers. Cost on the
  replay thread: descriptor sets 0.8 → 1.4 ms a frame.
- How to make it reproducible: `DM_VK_NOPREWARM=1` (every pipeline is fast-linked at its first draw,
  as on a first run). A soak with a warm cache links nothing and proves nothing.

### 1.3 Android loader places modules top-down; a game read past `main` — confirmed (Link's Awakening)
- `SIGSEGV` at `main+0x1c180` half a second into boot, in about one start of two.
- The base actor constructor indexes a table with actor type 0xffff ("none") with no range check, so
  it reads past the end of `main`. On the Switch (and on the Mac, where mappings ascend) that lands
  in `subsdk0`. `loader.c` maps each module with `mmap(NULL)`, so on Android the page above `main` is
  whatever happens to be there, or nothing.
- Fixed per game with one cut (`ldrh` → `ldrsh`, so 0xffff reads just below the table).
- General hint: a crash in game code that happens "only on Android, and only sometimes" is worth
  checking against `main`'s base + size. Mapping the modules contiguously in load order, as Horizon
  does, would remove the whole class.

### 1.4 NVN present ignored `vi::SetLayerCrop` — confirmed (Link's Awakening)
- Switching docked → handheld while running left a small picture in a corner of the old frame. The
  game never calls `nvnWindowSetCrop`; it only crops the layer.
- Local change: when the window has no crop, use the layer crop if it fits the texture.
- Same switch: controls stopped working. `BindNpadStyleSetUpdateEvent` events were kept for No1 to
  No8 only and nothing was signalled on an operation mode change; SDK 8 re-selects its controller on
  that event. Keeping the Handheld / Other events too and signalling No1 and Handheld on the change
  fixed it.

### 1.5 `DM_GPU_TURBO` stays on for the whole device — confirmed
- `KGSL_PROP_PWRCTRL` is device-wide. After the process ends the GPU stays at its top clock for every
  app, asleep or not.
- Local change: write the wanted state once at start, and hand control back in `on_pause` /
  `on_destroy`. A force-stop still leaves it on until the next start.

### 1.6 Movie frames in native texture mode cost twice — confirmed (Link's Awakening, 1080p60 VP9)
- Measured per stage on the device: libvpx decode 2 to 3 ms a packet, YUV → RGBA (scalar) 7.5 to 10 ms,
  swizzled write into the guest texture 4 to 14 ms, then the replay sees dirty pages and re-uploads
  (hash + deswizzle + copy, about 7 ms).
- What helped: upload the host's frame straight to the Vulkan image (no guest round trip), and a NEON
  I420 → RGBA. Result: content actually changing on 95 to 96% of display frames (72 to 80% before).
- What did not help: a MediaCodec VP9 hardware decoder. No gain, and it stalled the movie in some
  runs. I had blamed the decoder from whole-frame times alone; the stage timings showed it was never
  the slow part.
- Hint for reviewing movies: the FPS counter reads 60 either way. Look at p95 frame time and at how
  many recorded frames differ from the one before.

### 1.7 Turnip and NaN-aware min / max / clamp — observed, partly helpful
- The load-time rewrite to `NMin` / `NMax` / `NClamp` gives Maxwell's results on the Qualcomm driver.
  On this Turnip build they behaved like the plain forms in one case I looked at.
- Writing the selection out with `OpIsNan` + `OpSelect` reduced an artefact but did not remove it, so
  I did not ship it. Mentioned in case someone sees NaN-shaped artefacts on Turnip only.

### 1.8 Depth pre-pass and colour pass disagree on alpha test, Turnip only — worked around
- Link's Awakening draws its path-edge decals depth first with a generic alpha-tested shader pair,
  then in colour with the material's own pair under an `EQUAL` depth test and the same discard.
- On Turnip the depth pass keeps pixels the colour pass discards (whole parts of triangles; the
  colour pass's alpha there is under 0.1). Nothing is drawn over the clear colour: black pieces at
  the grass tips that flicker, mostly while walking. The stock driver is clean.
- Ruled out by experiment: mip level, the texture coordinate's path through the vertex shader,
  cached dynamic state, the uniforms (mode = texture alpha only), texture view and sampler (the
  same), LRZ / command buffer cuts, dynamic UBOs. A depth pass that discards everything leaves no
  pieces, so discard does stop the depth write.
- Workaround: a per-game list of fragment shader keys whose discard is removed at load (the block
  `OpLabel; OpKill` under a selection branches to the merge block instead). The colour pass's pixels
  are the depth pass's already, so nothing changes where the two agree. The cause is still unknown.

### 1.9 Per-draw overhead when a frame has thousands of draws — measured, open
- 13 Sentinels, a cutscene with about 6,000 draws a frame: `simpleperf` put about 17% in the big-stack
  switch around every NVN call (trivial setters included), 13% in `prepare_draw` (the pipeline key is
  rebuilt for every draw), 6% in `stage_set`.
- The game thread was also scheduled on little cores, and Android 13 has no ADPF `set_threads`, so
  only the present thread gets the hint.

### 1.10 Disgaea 5 (SDK 1.7.1): the audio renderer lives in the service — confirmed, new port
- SDK 1.x has no in-process renderer to switch to CPU / manual mode: there is no
  `ExecuteAudioRendererRendering`, the SDK's client only talks to `audren:u`.
- Local change: the host serves `audren:u` itself for such SDKs, with a real mixer behind it (PCM16
  and DSP-ADPCM with loop context, Catmull-Rom resampling, biquad, volume ramps, submixes, the buffer
  mixer effect, sink downmix). Ported from LASRecomp (MIT, Ryujinx-based). Screen, effects, music and
  voices are right on the device.
- Two traps in the update format: channel resources go by section order, not by ID (an unused entry
  with ID 0 overwrote voice 0's volume with 0), and PCM positions come from the sample offset, not
  the size field.
- Movies on SDK 1 to 3: `nv::SetGraphicsAllocator`'s original has to be called too (so the NvOsAlloc
  heap uses the game's allocator), `movie::Decoder` has a two-argument constructor and other output
  format numbers, and H.264 pictures must keep their real times or B-frames come out of order.
- The game's own movie player has a race: its event thread appends to a vector with no lock while
  the main thread erases from it. One entry is lost and another doubled, and pictures come out of
  order for the rest of the movie; a faster build hits it more often. Seven observation gates plus a
  host lock put both sides under one lock (time going backwards 0, an index twice 0 afterwards).

### 1.11 Unicorn Overlord: the game waits for its own GPU work every frame — measured
- GPU and CPU each fit in 16 ms and the game runs at 48 fps: game gap + replay + GPU tail add up to
  the frame, because the render thread submits the next frame only after the fence of the one before.
  In the port that "GPU work" includes the replay's CPU time.
- Sending work to the GPU while the replay is still going helps: `DM_VK_PASS_SPLIT=256` with
  `DM_VK_PASS_SPLIT_DEPTH=1` took a 5,300-draw screen from 20.8 to 16.5 ms. Smaller command buffers
  (`DM_VK_CB_DRAWS`) did nothing.
- Removing the pack's `DM_VK_ZEROCOPY_MAX_MB` line: PSS 4.96 → 3.45 GB, swap 1.32 GB → 0,
  submit + present p50 4.6–8 → 2.6–3.6 ms.
- A 6,800-draw map is at the GPU's limit on Adreno 740 (86 to 90% busy at the top clock). The time
  does not move with resolution; inside passes it is about 1.6 µs a draw, and roughly half the frame
  seems to go to pass boundaries (95 passes). The app's own "GPU p50" under-reports here because the
  parts sent early are not counted; `gpu_busy_percentage` × frame time is the better number.
- 2D sprite games are the ones with many draws (about 4,000 in battles here, 6,000 in 13 Sentinels).

### 1.12 Smaller ones
- **Disgaea 4 (SDK 7.4)**: `nvnTextureBuilderGetSparseTileLayout` is unimplemented; the stub leaves
  the output struct unfilled and the game sizes a buffer from garbage, crashing later in its own
  free. A red "unimplemented" line is worth chasing even when the crash is elsewhere. Also: DeviceSink
  is command ID 0x10 there although the header is the SDK 8 form, and one draw needed the front face
  inverted.
- **Unreal preset in `profile_guess.py`**: `mem_reserve` equals 256 MB + alias + stack exactly, so the
  heap region is zero and the game aborts at once (`SetHeapSize OOM: req 0x600000 > cap 0`). 128 MB
  more fixes it.
- **Shared font and Korean**: games that draw text with the system's shared font get the device's
  Roboto, which has no Hangul. `--shared-font` works, but a substitute needs the console font's OS/2
  ascent / descent (1158 / 378 per 1024) or text is about 1.5× too large, and a variable font must be
  instanced first (its default instance may be Thin).
- **Fire Emblem Engage**: the lit (point light) variant of one grass shader comes out dark for the
  second a hit light exists. Light table, loop contributions and NaN checks all looked right; cause
  unknown. Worked around by shipping a replacement SPIR-V for that key.
- **Low-memory kills**: on an 8 GB device the first shader preparation (PSS near 4.9 GB) or a save
  load that makes hundreds of MB of pools in a second can end in `lmkd` killing the game with
  "device is not responding".

---

## 2. Tools: readers and porter behaviour

### 2.1 Unreal pak version 8 and version 1 shader code archives (UE 4.22) — confirmed
- Minecraft Dungeons: 46 paks, 140,902 files, all stored, index not encrypted. `uepak.py` reads
  versions 4 and 11, so the split found no shaders.
- Version 8: the legacy per-file index; footer magic 0xac from the end (key GUID, flag, magic,
  version, index offset and size, SHA-1, four 32-byte method names); a **one-byte** compression method
  in each `FPakEntry` (4.22; 4.23 widened it), so the entry repeated in front of the data is 50 bytes.
- Shader code archive version 1: `u32 1`, `i32 count`, then per shader SHA-1 (20), `u64` offset,
  `u32` size, `u32` raw size, `u8` frequency; the code follows the table. Each shader is one zlib
  stream of an LSLG record. 15,596 shaders, all translated.

### 2.2 A pak folder with IoStore shader libraries is taken as "inline shaders" — confirmed
- Minecraft Dungeons II: the pak (version 11, Oodle) holds `GlobalShaderCache-*.bin` and no archive,
  so `inline_paks` takes the folder. Its shaders are the libraries in the `.utoc`, already decoded;
  the split then asks for the Mac's Oodle host and stops.
- A check for `IO_SHADER_LIBRARY` chunks in the folder's containers before counting it fixes it.

### 2.3 Things that cost time on Windows
- **Multi-title XCI** (base + update + DLC in one image): only the base is used. The message says so,
  but it is one line among many. I split such images into one NSP per title (meta NCA → `.cnmt` →
  content IDs; the update's delta fragments are not in the image, which is normal).
- **`patches.py` in a provisional profile is not read.** `patch_nso.py` loads `dm/games/*/patches.py`
  only. The rewrite step then says "reused (inputs unchanged)" and the port looks fine. Copying the
  provisional profile into `dm/games/<id>/` is what makes cuts apply.
- **Two adb devices connected**: the Unreal Oodle decode step stops with "several devices connected:
  choose one", but the run still ends with exit 0 and an old pack. `--device` avoids it.
- **No heap check on Windows**, so guessed memory keys go to the device unverified. The first log's
  `guest AS` and `SetHeapSize` lines are the check.
- **Regional releases** have another title ID, so an existing profile is not found. Copying the
  profile folder to the new ID (identity keys and `romfs_marker` changed) and letting the porter make
  a derived profile worked for 13 Sentinels Asia.
- **`porter install` rewrites `env.txt`** from the pack; lines added on the device are gone after it.
- **`shaders.dmsv` is pushed last.** A user who opens the app during the push gets "no shader bundle"
  and a set of bogus `captured_shaders`.

---

## 3. Open problem: Minecraft Dungeons II stops after the logo

Unreal with IoStore, Oodle, Havok, Wwise, Xbox Live; SDK 20.5.15. Builds and installs.

1. First run: `nn::diag abort` at frame 59. `XalInitialize` fails and the game raises
   `LowLevelFatalError "XblInitialize failed"`. One cut in the online subsystem factory (take the
   "not enabled" branch) removes the subsystem cleanly.
2. Then: the logo movie plays, and right after it frames stop (under 300 presented). No crash.
   - Game thread: `FlushRenderingCommands` → fence wait, under the Slate renderer, in every one of 14
     samples.
   - Render thread: its idle task loop in every sample, yet its wait count keeps rising (the
     heartbeat wakes it). RHI thread, task workers, IoDispatcher and the async loading thread have
     not woken since about the time the movie ended.
   - Not the cause: Xbox Live (same stall with the subsystem gone), `r.GTSyncType` (0, 1 and 2 all
     stall), `-norhithread`.
   - The named-thread enum has RHI = 0, Game = 1, Render = 2, and the workers are called
     "Foreground / Background Worker", so this build uses the newer task system.
   - My reading: the render commands of this flush never reach the render thread. Whether that is an
     atomic sequence in the rewritten code or something the host answers differently, I do not know.

If this pattern is familiar, a pointer would help.

---

## 4. Techniques that paid off

- **Thread dump of a stalled game**: `run-as <pkg> kill -ABRT <pid>` on a debug build writes every
  guest thread's wait and backtrace to the log; the `DM_CTL` line `threads` does it without ending
  the run. Sampling it 10 to 15 times tells "always here" from "happened to be here".
- **Naming stripped functions**: for each return address, list the strings the surrounding function's
  `adrp` + `add` pairs point to (ASCII and UTF-16). Enough to recognise most Unreal functions.
- **One alternative shader per run**: `DM_VK_SPV_DIR` + `DM_VK_SPV_ALT=1`. The lookup is cached per
  shader for the session, misses included, so each variant needs its own start; the log line
  "alternative code from" confirms it took.
- **Diagnostic shaders instead of guesses**: make the colour pass paint what it would discard, in
  bands by the value in question. That showed the alpha mismatch was gross (under 0.1), not rounding.
- **Catching fast-linked frames**: dump only the frames in which a fast-linked pipeline drew, then
  take the set difference of pipelines between bad and good frames.
- **Trace order**: in a `DM_VK_TRACE_FRAME` log a draw's descriptor and pipeline lines come *before*
  its `draw:` header. I read them the other way round for a while.
- **Per-renderer audio dump**: writing each renderer's output to a file and checking for repeated
  frames, single silent frames and jumps at 240-sample boundaries separates "broken stream" from
  "wrong content".
- **Engine arguments without a new port**: `DM_GAME_ARGS_ADD` in `env.txt`.
- **A person may be holding the device.** Look at a screenshot and the resumed activity before
  stopping or installing anything; the touch overlay and system UI leave no line in the log.

---

## 5. If I could ask for five things

1. Several audio renderers at once (1.1).
2. Android loader: contiguous module placement in load order (1.3).
3. A port that stops early should not end with exit 0 and an old pack (2.3).
4. `patches.py` honoured in a provisional profile, or a line saying it was ignored (2.3).
5. Pak version 8 / archive version 1 readers, and the IoStore check in `inline_paks` (2.1, 2.2).
