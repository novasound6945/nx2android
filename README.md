<h1 align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/images/logo-dark.svg">
    <img alt="nx2android" src="docs/images/logo.svg" height="140">
  </picture>
</h1>

<p align="center">
  <b>English</b> &nbsp;·&nbsp; <a href="README.ko.md">한국어</a>
</p>

<p align="center">
  <a href="https://github.com/shirai91/nx2android"><img alt="fork of shirai91/nx2android" src="https://img.shields.io/badge/fork%20of-shirai91%2Fnx2android-2f6fed"></a>
  <img alt="devices: Retroid Pocket 6 and Nova" src="https://img.shields.io/badge/devices-Retroid%20Pocket%206%20%C2%B7%20Nova-e8590c">
  <img alt="GPU: Adreno 740" src="https://img.shields.io/badge/GPU-Adreno%20740-c92a2a">
  <img alt="driver: Turnip" src="https://img.shields.io/badge/driver-Turnip-7048e8">
  <img alt="license: MIT" src="https://img.shields.io/badge/license-MIT-2b8a3e">
</p>

> [!IMPORTANT]
> **A personal fork, not the original project.** nx2android is the work of [shirai91](https://github.com/shirai91) and its contributors, built on [nxnative](https://github.com/seedds/nxnative) by seedds. Everything here stands on their work, with thanks. Releases, the full game list and support: **[shirai91/nx2android](https://github.com/shirai91/nx2android)**.
>
> **개인 포크이며 원본 프로젝트가 아닙니다.** nx2android는 [shirai91](https://github.com/shirai91)과 기여자들의 작업이고, seedds의 [nxnative](https://github.com/seedds/nxnative) 위에 만들어졌습니다. 이 포크의 모든 것은 그 작업 위에 서 있으며 감사드립니다. 릴리스·전체 게임 목록·지원은 **[shirai91/nx2android](https://github.com/shirai91/nx2android)** 로 가세요.

| | English | 한국어 |
|---|---|---|
| 🎯 **Target** | Only the **Retroid Pocket 6** and **Retroid Pocket Nova** (Snapdragon 8 Gen 2, **Adreno 740**) with a **Turnip** Vulkan driver. Every default and measurement is chosen for that GPU; nothing is tested elsewhere. Other devices: use upstream. | 대상은 **Retroid Pocket 6**와 **Retroid Pocket Nova**(Snapdragon 8 Gen 2, **Adreno 740**) + **Turnip** 드라이버뿐입니다. 모든 기본값과 측정값이 이 GPU 기준이고 다른 기기는 시험하지 않습니다. 다른 기기는 업스트림을 쓰세요. |
| 🧩 **What it is** | A few more games brought up on those devices and the fixes they needed, kept merged with upstream's `main`. The work is on the `ports` branch, under the same MIT license. | 그 기기들에서 게임 몇 개를 더 띄우고 필요했던 수정을 얹은 것입니다. 업스트림 `main`과 계속 병합하며, 작업은 `ports` 브랜치에 있고 같은 MIT 라이선스입니다. |
| 🛠️ **Who it is for** | People who want to take it and adapt it for themselves. Offered as it is: no releases (run from the checkout, [guide](docs/DEVELOPMENT_GUIDE.md)), no support. | 받아서 자기에게 맞게 고쳐 쓰려는 분들을 위한 것입니다. 있는 그대로 제공하며 릴리스도 지원도 없습니다(체크아웃해서 실행, [안내](docs/DEVELOPMENT_GUIDE.md)). |
| ⚖️ **Your responsibility** | No games, no keys, nothing extracted from either, and no links to any. Dumping from your own console, owning the games you port and what your law allows are yours alone to answer for. [More](#keys-and-game-files) | 게임도 키도 거기서 추출한 것도 없고 그런 곳으로 연결하지도 않습니다. 자기 콘솔에서 덤프하는 일, 게임을 소유하는 일, 사는 곳의 법이 허용하는 범위는 전적으로 각자의 책임입니다. [자세히](#keys-and-game-files) |
| 🐞 **Problems** | Assume this fork's changes are the cause and report them here, not upstream. | 이 포크의 변경이 원인이라고 보고, 업스트림이 아니라 여기에 알려 주세요. |

<p align="center">
  <b>What this fork adds</b> &nbsp;/&nbsp; <b>이 포크가 더한 것</b><br>
  <a href="#games-brought-up-in-this-fork">Games · 게임</a> &nbsp;·&nbsp;
  <a href="#shader-preparation-at-the-first-launch">Shader preparation · 셰이더 준비</a> &nbsp;·&nbsp;
  <a href="#the-games-data-from-your-own-server">Data from your server · 내 서버에서 데이터 받기</a> &nbsp;·&nbsp;
  <a href="#what-else-this-fork-changes">The rest · 그 밖의 변경</a> &nbsp;·&nbsp;
  <a href="docs/PORTING_NOTES.md">Porting notes · 포팅 노트</a>
</p>

---

Port your own Nintendo Switch games to Android, natively: the game's own arm64 code runs on the device, with the Switch's system services stood in for by a host and NVN graphics replayed on Vulkan. Pick the game's folder, click **Port**, and get an APK plus the game's data, installed on your device if you like. Bring your own game files and keys; nothing from any game is in this repository.

This is the Android port of [nxnative](https://github.com/seedds/nxnative), which does the same on iPhone and iPad. Supported games: [Games](#games).


## Games

### Games brought up in this fork

Tested on the Retroid Pocket 6 and the Retroid Pocket Nova (both Snapdragon 8 Gen 2, Adreno 740, Android 13) with the Turnip Vulkan driver "PurpleVK T30" (`--vulkan-driver`). These two devices are the only ones this fork targets. Several games are Asian releases, which have their own title IDs and so their own profiles.

| Game | Version | Device | Result |
|---|---|---|---|
| The Legend of Zelda: Link's Awakening | update 1.1.0 | Retroid Pocket 6 | **Playable**, 60 fps held. The depth-of-field blur is off by default (a row in the profile turns it back on). Docked or handheld, switched while running. Korean text with `--shared-font` |
| Disgaea 5: Alliance of Vengeance | v196608 (SDK 1.7.1) | Retroid Pocket Nova | **Playable**, 60 fps: logo, opening movie, title, field and battle, with sound effects, music and voices. An SDK this old has no audio renderer in the game's process, so the host serves `audren:u` and mixes the sound itself |
| Disgaea 4 Return | base game | Retroid Pocket 6 | Title, new game, the opening and the hub at 60 fps. Battles not checked yet |
| 13 Sentinels: Aegis Rim (Asian release) | base game | Retroid Pocket 6 | **Playable**, 60 fps; the busiest robot cutscene (about 6,000 draws a frame) falls below |
| Unicorn Overlord (Asian release) | update 1.05 | Retroid Pocket 6 | **Playable**, 60 fps in most scenes; the largest maps (about 6,800 draws a frame) are at the GPU's limit |
| Dark Souls: Remastered | update 1.0.3 | Retroid Pocket Nova | 60 fps held (30 on Switch) at the game's real speed; 4:3 screens; no colour flashes on new pipelines. `DM_FPS_TARGET=30` in `env.txt` keeps the game's own 30 |
| Fire Emblem Engage | update 2.0.0 | Retroid Pocket Nova | **Playable**, 60 fps (30 on Switch): chapters 1 and 2, maps, battles and battle scenes, in Korean. Its 206,899 files travel as 89 bundles |
| Tales of Eternia Remastered | update 2.1.0 | Retroid Pocket 6 | **Playable**, 60 fps held |
| Minecraft Dungeons | base game | Retroid Pocket 6 | **Work in progress**: boots and plays the first level at 30 fps with sound. The runtime and tool support is here (two audio renderers at once, Unreal pak version 8); its profile is not committed yet |

Not working yet: Minecraft Dungeons II stops right after its logo ([docs/PORTING_NOTES.md](docs/PORTING_NOTES.md#3-open-problem-minecraft-dungeons-ii-stops-after-the-logo)).

### Games from upstream

These games play on Android. Tested on a Lenovo Legion Y700 (Snapdragon 8 Elite, Adreno 830), unless a game names another device. The device's GPU must read BC-compressed textures (Adreno, Xclipse; not Mali or PowerVR).

| | |
|:---:|:---:|
| ![13 Sentinels: Aegis Rim](docs/images/13-sentinels.jpg)<br>13 Sentinels: Aegis Rim<br><sub>`v0.1.3` · update 1.10 · 60 fps</sub> | ![Advance Wars 1+2: Re-Boot Camp](docs/images/advance-wars-1-2-re-boot-camp.jpg)<br>Advance Wars 1+2: Re-Boot Camp<br><sub>`v0.1.3` · base game 1.0.0 · 60 fps (30 in battle on Switch)</sub> |
| ![Animal Crossing: New Horizons](docs/images/animal-crossing-new-horizons.jpg)<br>Animal Crossing: New Horizons<br><sub>`v0.4.0` · update 3.0.3 · 30 fps (as on Switch) · local play over Wi-Fi with Switches that run ldn_mitm (Atmosphère 1.11.2 needs ldn_mitm 1.21.2)</sub> | ![Blasphemous 2](docs/images/blasphemous-2.jpg)<br>Blasphemous 2<br><sub>`v0.1.3` · update 1.0.8 · 60 fps</sub> |
| ![Captain Toad: Treasure Tracker](docs/images/captain-toad-treasure-tracker.jpg)<br>Captain Toad: Treasure Tracker<br><sub>`v0.1.3` · update 1.4.2 · 60 fps</sub> | ![Cardfight!! Vanguard Dear Days 2](docs/images/cardfight-vanguard-dear-days-2.jpg)<br>Cardfight!! Vanguard Dear Days 2<br><sub>`v0.4.0` · update 1.7.2 · 30 fps, 60 with the 60 fps box · tested on the AYN Odin 2 with a Turnip driver</sub> |
| ![Castlevania: Belmont's Curse](docs/images/castlevania-belmonts-curse.jpg)<br>Castlevania: Belmont's Curse<br><sub>`v0.2.0` · update 1.1.0 · 60 fps</sub> | ![Celeste](docs/images/celeste.jpg)<br>Celeste<br><sub>`v0.1.3` · update 1.4.1.1 · 60 fps</sub> |
| ![Dark Souls: Remastered](docs/images/dark-souls-remastered.jpg)<br>Dark Souls: Remastered<br><sub>`v0.4.0` · update 1.0.3 · 30 fps</sub> | ![Disco Elysium - The Final Cut](docs/images/disco-elysium-the-final-cut.jpg)<br>Disco Elysium - The Final Cut<br><sub>`v0.4.0` · base game 1.0.7 · 30 fps (as on Switch) · tested on the Sony XQ-DQ72 and the OnePlus 15</sub> |
| ![Donkey Kong Country Returns HD](docs/images/donkey-kong-country-returns-hd.jpg)<br>Donkey Kong Country Returns HD<br><sub>`v0.1.3` · update 1.1.0 · 60 fps</sub> | ![Final Fantasy Resonance](docs/images/final-fantasy-resonance.jpg)<br>Final Fantasy Resonance<br><sub>`v0.4.0` · update 1.0.1 · 60 fps</sub> |
| ![Fire Emblem Engage](docs/images/fire-emblem-engage.jpg)<br>Fire Emblem Engage<br><sub>`v0.4.0` · update 2.0.0 · 60 fps (30 on Switch) · tested on the AYN Thor (Adreno 740) with a Turnip Vulkan driver; Qualcomm's own driver mis-draws Engage Attacks (#122)</sub> | ![FM24 Touch](docs/images/fm24-touch.jpg)<br>FM24 Touch<br><sub>`v0.4.0` · update 1.0.1 · 60 fps · OpenGL ES · touch or controller</sub> |
| ![Football Manager 26 Touch](docs/images/football-manager-26-touch.jpg)<br>Football Manager 26 Touch<br><sub>`v0.4.0` · update 1.0.2 · 30 fps (as on Switch) · played by touch · port with a Turnip Vulkan driver (the crowd glitches on Qualcomm's driver)</sub> | ![Graveyard Keeper 2](docs/images/graveyard-keeper-2.jpg)<br>Graveyard Keeper 2<br><sub>`v0.1.2` · update 1.0.4 · 60 fps (30 on Switch)</sub> |
| ![Hades II](docs/images/hades-ii.jpg)<br>Hades II<br><sub>`v0.1.2` · update 1.0 · 60 fps</sub> | ![Hollow Knight: Silksong](docs/images/hollow-knight-silksong.jpg)<br>Hollow Knight: Silksong<br><sub>`v0.1.3` · update 1.0.30000 · 60 fps</sub> |
| ![Kirby and the Forgotten Land](docs/images/kirby-and-the-forgotten-land.jpg)<br>Kirby and the Forgotten Land<br><sub>`v0.4.0` · update 1.1.0 · 30 fps (as on Switch) · tested on the AYN Thor (Adreno 740) with a Turnip Vulkan driver</sub> | ![Kirby's Return to Dream Land Deluxe](docs/images/kirbys-return-to-dream-land-deluxe.jpg)<br>Kirby's Return to Dream Land Deluxe<br><sub>`v0.4.0` · 60 fps · tested on the AYN Thor (Adreno 740) with a Turnip Vulkan driver · needs your console's firmware for its fonts (`--firmware`)</sub> |
| ![Lonely Mountains: Downhill](docs/images/lonely-mountains-downhill.png)<br>Lonely Mountains: Downhill<br><sub>`v0.3.0` · base game 1.0.5 · 60 fps on Retroid Pocket 6 with PurpleVK · **work in progress**: brief transition glitch and occasional black/white flashes</sub> | ![Mario vs. Donkey Kong](docs/images/mario-vs-donkey-kong.jpg)<br>Mario vs. Donkey Kong<br><sub>`v0.1.3` · update 1.0.1 · 60 fps</sub> |
| ![Mega Man Battle Network Legacy Collection Vol. 1](docs/images/mega-man-battle-network-legacy-collection-vol-1.jpg)<br>Mega Man Battle Network Legacy Collection Vol. 1<br><sub>`v0.4.0` · update 1.0.2 · 60 fps, held · tested on the AYN Odin 2 (Adreno 740) with a Turnip Vulkan driver (Qualcomm's driver crashes or runs at 30) · Battle Network 1, 2 and 3 played: saves, battles, maps, gallery · the launcher's menus draw fully from the second port with the device connected after a first run (it builds some shaders at run time)</sub> | ![Mega Man Battle Network Legacy Collection Vol. 2](docs/images/mega-man-battle-network-legacy-collection-vol-2.jpg)<br>Mega Man Battle Network Legacy Collection Vol. 2<br><sub>`v0.4.0` · update 1.0.3 · 60 fps, held · tested on the AYN Odin 2 (Adreno 740) with a Turnip Vulkan driver · Battle Network 4, 5 and 6 played: saving, loading, battles, gallery · the launcher's menus draw fully from the second port with the device connected after a first run (it builds some shaders at run time)</sub> |
| ![Metroid Dread](docs/images/metroid-dread.jpg)<br>Metroid Dread<br><sub>`v0.3.0` · update 2.1.0 · 60 fps</sub> | ![NEO: The World Ends with You](docs/images/neo-the-world-ends-with-you.jpg)<br>NEO: The World Ends with You<br><sub>`v0.1.2` · update 1.03</sub> |
| ![OCTOPATH TRAVELER 0](docs/images/octopath-traveler-0.jpg)<br>OCTOPATH TRAVELER 0<br><sub>`v0.4.0` · update 1.0.5 · 60 fps · tested on the AYN Thor (Adreno 740) with a Turnip Vulkan driver</sub> | ![OCTOPATH TRAVELER 0 (Chinese release)](docs/images/octopath-traveler-0-chinese.jpg)<br>OCTOPATH TRAVELER 0 (Chinese release)<br><sub>`v0.4.0` · update 1.0.5 · 60 fps · the Chinese and Korean release · tested on the Xiaomi 23117RK66C (Android 16) · port with Game language: Chinese (Simplified) (`--language zh-Hans`)</sub> |
| ![Paper Mario: The Origami King](docs/images/paper-mario-the-origami-king.jpg)<br>Paper Mario: The Origami King<br><sub>`v0.2.0` · update 1.0.1 · 60 fps (30 on Switch)</sub> | ![Paper Mario: The Thousand-Year Door](docs/images/paper-mario-the-thousand-year-door.jpg)<br>Paper Mario: The Thousand-Year Door<br><sub>`v0.2.0` · update 1.0.1 · 60 fps (30 on Switch) · **work in progress**: playable, but lighting flickers (walls go black for single frames after a fall)</sub> |
| ![Picross S+](docs/images/picross-s-plus.png)<br>Picross S+<br><sub>`v0.3.0` · update 1.0.1 · Retroid Pocket 6 / PurpleVK · menu and puzzle verified; **Mali rendering issues**</sub> | ![Pokémon Brilliant Diamond](docs/images/pokemon-brilliant-diamond.jpg)<br>Pokémon Brilliant Diamond<br><sub>`v0.4.0` · update 1.3.0 · 60 fps (30 on Switch) · tested on the AYN Thor (Adreno 740) with a Turnip Vulkan driver</sub> |
| ![Pokémon Legends: Arceus](docs/images/pokemon-legends-arceus.jpg)<br>Pokémon Legends: Arceus<br><sub>`v0.4.0` · update 1.1.1 · 30 fps (as on Switch) · tested on the AYN Thor (Adreno 740) with a Turnip Vulkan driver</sub> | ![Pokémon Legends: Z-A](docs/images/pokemon-legends-z-a.jpg)<br>Pokémon Legends: Z-A<br><sub>`v0.3.0` · update 2.0.2 · 30 fps (as on Switch) · **tested to Lumiose station**: the city and battles not checked yet</sub> |
| ![Pokémon Mystery Dungeon Rescue Team DX](docs/images/pokemon-mystery-dungeon-rescue-team-dx.jpg)<br>Pokémon Mystery Dungeon Rescue Team DX<br><sub>`v0.4.0` · update 1.0.2 · 30 fps (as on Switch), 60 with the 60 fps option · tested on the AYN Odin 2 (Adreno 740) with a Turnip Vulkan driver</sub> | ![Pokémon Scarlet](docs/images/pokemon-scarlet.jpg)<br>Pokémon Scarlet<br><sub>`v0.4.0` · update 4.0.0 · 50–60 fps (30 on Switch) · tested on the AYN Thor (Adreno 740) with a Turnip Vulkan driver · **work in progress**: tested in the opening and the South Province fields</sub> |
| ![Pokémon Shield](docs/images/pokemon-shield.jpg)<br>Pokémon Shield<br><sub>`v0.4.0` · update 1.3.2 · 30 fps (as on Switch; 60 runs the game twice as fast) · tested on the AYN Odin 2 (Adreno 740) with a Turnip Vulkan driver · **tested to Postwick** and in wild battles</sub> | ![Pokémon Sword](docs/images/pokemon-sword.jpg)<br>Pokémon Sword<br><sub>`v0.4.0` · update 1.3.2 · 30 fps (as on Switch; 60 runs the game twice as fast) · tested on the AYN Odin 2 (Adreno 740) with a Turnip Vulkan driver · **tested to the Wild Area**, wild battles included; handheld mode draws blurred (docked is the default)</sub> |
| ![Rhythm Heaven Groove](docs/images/rhythm-heaven-groove.jpg)<br>Rhythm Heaven Groove<br><sub>`v0.4.0` · base game 1.0.0 · 60 fps · tested on the Xiaomi 15 (Adreno 830)</sub> | ![South Park: The Stick of Truth](docs/images/south-park-the-stick-of-truth.jpg)<br>South Park: The Stick of Truth<br><sub>`v0.3.1` · base game 1.0.0 · 30 fps · a Vulkan 1.3 driver is needed (Turnip packed on the Adreno 640)</sub> |
| ![Super Mario 3D World + Bowser's Fury](docs/images/super-mario-3d-world-bowsers-fury.jpg)<br>Super Mario 3D World + Bowser's Fury<br><sub>`v0.4.0` · update 1.2.2 · 60 fps · tested on the AYN Thor (Adreno 740) with a Turnip Vulkan driver</sub> | ![Super Mario Bros. Wonder](docs/images/super-mario-bros-wonder.jpg)<br>Super Mario Bros. Wonder<br><sub>`v0.1.3` · update 1.2.1 · 60 fps, dips to 40</sub> |
| ![Super Mario Odyssey](docs/images/super-mario-odyssey.jpg)<br>Super Mario Odyssey<br><sub>`v0.2.0` · update 1.4.1 · 60 fps · shake the device to throw Cappy</sub> | ![Tales of Eternia Remastered](docs/images/tales-of-eternia-remastered.jpg)<br>Tales of Eternia Remastered<br><sub>`v0.1.2` · update 2.0.0 · 60 fps</sub> |
| ![The Legend of Zelda: Breath of the Wild](docs/images/the-legend-of-zelda-breath-of-the-wild.jpg)<br>The Legend of Zelda: Breath of the Wild<br><sub>`v0.4.0` · update 1.9.0 · 60 fps in the open world and shrines; about 30 in the busiest villages, which hold 60 while the phone is cool (30 on Switch) · the profile turns on `DM_GPU_TURBO=1` (the GPU at its highest clock: the phone runs hot, 67-76 °C; `DM_GPU_TURBO=0` in env.txt turns it off) · tested on the Galaxy S25 Ultra (Adreno 830) with Turnip (Stable Turnip v2, Mesa 26.2.3) · **work in progress**: tested on the Great Plateau, in shrines, villages and teleports</sub> | ![The Legend of Zelda: Echoes of Wisdom](docs/images/zelda-echoes-of-wisdom-rp6.png)<br>The Legend of Zelda: Echoes of Wisdom<br><sub>`v0.3.0` · version 1.1.0 · playable on Retroid Pocket 6 with PurpleVK · **work in progress**: minor transparency artifacts around rifts</sub> |
| ![The Legend of Zelda: Tears of the Kingdom](docs/images/the-legend-of-zelda-tears-of-the-kingdom.jpg)<br>The Legend of Zelda: Tears of the Kingdom<br><sub>`v0.3.0` · update 1.4.3 · 30 fps, 60 in caves and shrines · smoothest with a Turnip Vulkan driver · **work in progress**: 60 fps in the open world</sub> | ![The World Ends with You -Final Remix-](docs/images/the-world-ends-with-you-final-remix.jpg)<br>The World Ends with You -Final Remix-<br><sub>`v0.4.0` · base game 1.0.0 · 60 fps · played by touch</sub> |
| ![Tomodachi Life: Living the Dream](docs/images/tomodachi-life-living-the-dream.jpg)<br>Tomodachi Life: Living the Dream<br><sub>`v0.4.0` · update 1.0.4 · 60 fps</sub> | ![Ultra Kaiju Monster Rancher](docs/images/ultra-kaiju-monster-rancher.jpg)<br>Ultra Kaiju Monster Rancher<br><sub>`v0.4.0` · update 1.0.1 · 30 fps (60 runs the game twice as fast) · tested on the AYN Odin 2 (Adreno 740) with a Turnip Vulkan driver · handheld 3D resolution by default, docked in the app menu · tested through the tutorials and battles</sub> |
| ![UNDER NIGHT IN-BIRTH Exe:Late[cl-r]](docs/images/under-night-in-birth-exe-late-cl-r.jpg)<br>UNDER NIGHT IN-BIRTH Exe:Late[cl-r]<br><sub>`v0.2.0` · update 1.0.3 · 60 fps</sub> | ![UNDER NIGHT IN-BIRTH II Sys:Celes](docs/images/under-night-in-birth-ii-sys-celes.jpg)<br>UNDER NIGHT IN-BIRTH II Sys:Celes<br><sub>`v0.2.0` · update 1.42 · 60 fps</sub> |
| ![Unicorn Overlord](docs/images/unicorn-overlord.jpg)<br>Unicorn Overlord<br><sub>`v0.1.3` · update 1.05 · 60 fps · **boots**: saving and a full battle not tested yet</sub> | ![Unpacking](docs/images/unpacking.jpg)<br>Unpacking<br><sub>`v0.1.2` · update 2.01 · 60 fps · played by touch</sub> |
| ![Untitled Goose Game](docs/images/untitled-goose-game.jpg)<br>Untitled Goose Game<br><sub>`v0.4.0` · update 1.1.2 · 60 fps</sub> | ![Viractal: Will You Trust Your Party?](docs/images/viractal.jpg)<br>Viractal: Will You Trust Your Party?<br><sub>`v0.1.0` · update 1.4.5</sub> |
| ![Xenoblade Chronicles 3](docs/images/xenoblade-chronicles-3.jpg)<br>Xenoblade Chronicles 3<br><sub>`v0.1.0` · update 2.2.0 · 30 fps · **work in progress**: some effects and models don't draw yet</sub> | ![Xenoblade Chronicles: Definitive Edition](docs/images/xenoblade-chronicles-definitive-edition.jpg)<br>Xenoblade Chronicles: Definitive Edition<br><sub>`v0.4.0` · update 2.0.0 · 30 fps · tested on the AYN Odin 2 with a Turnip driver · **tested to Colony 9**, the pause menu and saving: later maps not checked yet · the scene drops to 896x504 in heavy fights · rarely crashes at boot (restart)</sub> |

The tag (`v0.1.3`) is the latest nx2android version the game was tested on. Older games are not tested again for each release: if a later version breaks one, a patch release fixes it. A tag newer than the [latest release](https://github.com/shirai91/nx2android/releases/latest) is the next release, and the game needs the development checkout until it ships.

A game that is not in this table is not supported yet. The games we look at next, and the ones done: [docs/PORT_CHECKLIST.md](docs/PORT_CHECKLIST.md).

## Shader preparation at the first launch

*In this fork.* A game pipeline met for the first time used to cost the driver's whole compile in the middle of play: a stutter at every new effect, or draws left out until the pipeline was ready. With a driver that has `VK_EXT_graphics_pipeline_library` (Turnip has it; Qualcomm's own driver on the Retroid Pocket 6 does not), the app makes every shader's part once, before the game starts, behind a progress screen: Dark Souls: Remastered's 3,099 shaders take about 17 s on a Retroid Pocket 6. During play a new pipeline is linked from those parts in about 0.03 ms, so nothing is left out, and the fully optimized pipeline is built in the background and takes over.

- It happens once per pack and driver. Later launches find the mark and check it in about half a second.
- Port with a Turnip driver (`--vulkan-driver <file>`) to get it. Without the extension the app runs as before.
- `DM_VK_GPL=0` in `env.txt` turns it off. To see a first start again, delete `files/vk_gpl.id`, `vk_pipeline_cache.bin` and `vk_pipelines.bin` with the game stopped.

How it is built: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) ("Graphics pipeline libraries").

## The game's data from your own server

*In this fork.* A port is an APK plus a data pack of several GB, normally copied over USB. Ported with `--data-url`, the APK installs by itself, on a device that never sees the PC, and fetches the pack from a web server of yours at its first launch. At every later launch it compares the server's list with its own and downloads only what changed, so a new build of the pack reaches the device without a cable.

1. Port with the server folder's address: `porter port <game folder> --data-url https://<host>/<path>/` (add `--data-cred <file>`, a file of one line `user:password`, if the server asks for Basic authentication).
2. Upload the `<App>-data` folder to that address as it is, `files.json` last. Any HTTP or HTTPS server that answers GET with Range works (a NAS's WebDAV share, for example); no directory listing is needed.
3. Install the APK on the device and start it.

After changing the pack, `porter publish <App>-data` writes the list again (unchanged files keep their hashes, so a 4 GB RomFS is hashed once). A pack of very many small files can travel as bundles (`data_bundles` in the profile's `android.tsv`): Fire Emblem Engage's 205,000 small files became 89.

The address and the `user:password` are inside the APK, masked but not encrypted: anyone who has the APK can read them. Use a read-only account made for this, keep the server private, and don't share the APK. The pack is your own dump; this is for your own devices.

How it is built: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) ("The data from a web server").

## What else this fork changes

- **Sound.** Several audio renderers at once (Minecraft Dungeons opens one for movies and one for the game). `audren:u` served by the host, with a real mixer, for SDK 1.x games (Disgaea 5).
- **Movies.** Frames go straight to the Vulkan image, and the colour conversion uses NEON: a 1080p60 VP9 opening that changed on 72 to 80% of display frames now does on 95 to 96%.
- **Graphics.** With pipeline libraries only the vertex stage's uniform buffers are dynamic (colour flashes in Dark Souls: Remastered, lost devices in Fire Emblem Engage on Turnip). NVN's alpha test as a fragment shader variant; real occlusion queries for conditional rendering; the layer's crop honoured at present; a per-game list of shaders whose alpha-test discard is removed (`DM_VK_FS_NOKILL`); a texture binding given another's texture (`DM_VK_BIND_ALIAS`, Link's Awakening's blur switch).
- **Running.** Docked and handheld switched while the game runs (`DM_CTL` `opmode`), with the controller events a game waits for. `DM_GPU_TURBO` gives the GPU's clock back when the app goes to the background. A frame rate limit in the pause menu.
- **Tools.** Unreal paks of version 8 and version 1 shader archives (UE 4.22); Disgaea's shader containers; a pak folder with IoStore shader libraries no longer mistaken for inline shaders.
- **Diagnostics.** A thread dump of a stalled game, per-renderer audio dumps, per-pass statistics over seconds, uniform buffer logs and traps for an effect that lasts one second.

## For players: port a game without programming

1. Download `nx2android-<version>-windows-x64.zip` (Windows), `nx2android-<version>-macos-arm64.zip` (Mac with Apple silicon), `nx2android-<version>-linux-x64.zip` (Linux on a PC) or `nx2android-<version>-linux-arm64.zip` (Linux on ARM) from [Releases](https://github.com/shirai91/nx2android/releases/latest), unzip it and start **nx2android** (on a Mac, see [packaging/INSTALL.md](packaging/INSTALL.md#1-start-the-app) first).
2. **Setup** tab: click **Download missing tools** (the Android NDK, the .NET SDK and platform-tools, about 1 GB, from Google and Microsoft), then set your **prod.keys**. Every line turns green.
3. **Port** tab: choose the folder with the game's NSP, XCI, NSZ or XCZ files (base game, update, DLC) and click **Port**.
4. Tick **Install on the device when done** with the device plugged in and USB debugging on, or copy the APK and the data folder yourself.

Pokémon Legends: Arceus and Pokémon Legends: Z-A need the device during the port, even without **Install on the device when done**: their shaders are in Oodle-compressed archives, which the port decodes with the game's own decoder on the device. Have platform-tools (adb) from step 2, and plug in the device with USB debugging on before you click **Port**.

A game played by touch: tick **Touch game** on the Port tab (`--touch`). The on-screen gamepad then starts hidden and every touch but one on the menu button goes to the game; the pause menu shows the gamepad again.

Chinese or Korean text drawn as `???`: choose a TrueType font with those characters as **System font** on the Port tab (`--shared-font`, a single `.ttf`, not a `.ttc`). It goes in the data pack; the tool ships none. The tool's own text entry panel shows those characters in its title and the game's initial text, but its keyboard types only ASCII.

A game that reads system data from the console's firmware (`sysdata` in its profile; Mario Kart 8 Deluxe builds its Mii heads from MiiModel, title 0100000000000802) needs your console's own system content files, which the tool cannot ship. Copy them (`.nca`, for example NAND SYSTEM `Contents/registered` in DBI's MTP mode) to a folder on the PC and give it with `--firmware <folder>` (or `porter config firmware <folder>` once). Without it the host's empty stand-in is used.

A game whose profile lists the shared font titles (`0100000000000811` and `0100000000000810`) needs the same `--firmware` folder for its text and button icons; without it the host uses Android's own font and generated icons, and a game that needs a glyph those lack can fail at start. `--shared-font <ttf>` still wins for the standard font.

A game slowed by its draw calls in busy scenes: tick **Multithread replay** on the Port tab (`--mt-replay`). The game's Vulkan commands are then recorded on a thread of their own. It stays on until you port the game again without it.

When **Show gamepad** is enabled, using a controller hides the on-screen controls. Touch the screen to bring them back. The draggable menu button stays available even with **Show gamepad** off.

To arrange the on-screen controls, open the pause menu's **Gamepad** tab and choose **Edit layout**: drag a control to move it, pinch or use **− / +** to resize it. **Save** keeps the layout for the next launch and **Cancel** discards your edits. **Snap to grid** (off at first, and remembered) shows a square grid and snaps each control's center to it as you drag.

A controller whose A is the bottom button (Xbox labels, most Android devices' built-in controls) can have its face buttons follow their labels: the **Gamepad** tab's **Face buttons** row switches between **Position** (the Switch's layout, the default) and **Label**. It applies at once, also to the pause menu's own A and B, and each device keeps its own choice.

A game whose frame rate wavers in its heaviest scenes can be held at a steady one: the **Game** tab's **Frame rate limit** row steps through **Auto** (the game's own setting, or the adaptive rate), **60 fps**, **40 fps** and **30 fps** each time you press it. 40 fps suits a 120 Hz screen (three refreshes a frame). It applies within a second of going back to the game, and each device keeps its own choice.

For Android's Unicode keyboard, port with `--native-keyboard`. It supports composed text, non-Latin scripts and emoji through the system IME, preserves initial text, and applies the guest's UTF-16 length limits. This option requires `javac` and an Android SDK with platform and build-tools (`ANDROID_HOME`). The default APK keeps the existing controller keyboard and does not require Java build tools. Combine `--external-display --native-keyboard` to opt into the native keyboard on an external-screen port; external display alone keeps the controller keyboard. The combined mode still needs runtime qualification over the full-screen phone gamepad.

For the experimental external-screen controller, port from the command line with `--external-display`. The game moves to a connected display and the phone shows the gamepad. In the pause menu's **Game → Audio output**, choose the phone, a connected output, or the system default; the list updates as devices connect or disconnect. Bluetooth call-only routes are omitted; the system may not honor the requested device. This option requires `javac` and an Android SDK with a platform and build-tools (`ANDROID_HOME`). Android 12 or newer uses the saved native layout; Android 10–11 use the fallback controller. Support varies by device and game.

Already have the game on the device for an emulator? The app can read the game's files (its RomFS, most of its size) from that dump's folder instead of a second copy: type the dump's RomFS folder in the app's **RomFS on the device** field (or give it to `--romfs-at`) before you install, which must be the same game version as the port. [packaging/INSTALL.md](packaging/INSTALL.md#the-game-is-already-on-the-device) says how.

A game that crashes with "AllocateAddressRegion … refused (0x1803)" in its log needs more address space: port it again with a larger **Extra address space** on the Port tab (`--alias-headroom` on the command line). [packaging/INSTALL.md](packaging/INSTALL.md#when-something-goes-wrong) says more.

The step-by-step guide is [packaging/INSTALL.md](packaging/INSTALL.md). You need your own game files and `prod.keys`, dumped from your own Switch; the tool never ships or downloads either. NSZ and XCZ files are decompressed during the port (into the build folder's `tmp/`, deleted once extracted).

## Saves

The game's pause menu (Game tab) has **Export save** and **Import save**: an export is a folder in `Android/data/<package>/files/saves/` on the device, the save's files as a console's save tools copy them, so a save from a console imports the same way. The app's **Saves** tab moves them between the device and the PC.

An import replaces the save the game has; the app exports that one first on the device (a folder ending `-before-import`). Uninstalling the app deletes its saves and its exports: copy them to the PC first.

## For developers

Running from the repository, the command line, the repository layout, building the bundle, tests and commit messages: [docs/DEVELOPMENT_GUIDE.md](docs/DEVELOPMENT_GUIDE.md). How it is put together: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Keys and game files

nx2android decrypts your game files with your `prod.keys` while it ports them, as Switch emulators do. This repository and its releases contain no keys, no game files and nothing extracted from them, and the tool never downloads either. Dump both from your own Switch, and don't share them or the APKs and data packs built from them.

Nintendo's takedown notices so far have gone to whoever hosts a project (13 Switch emulator repositories on GitHub in February 2026), not to players. Whether decrypting your own games for your own use is allowed depends on where you live.

## License

MIT, as nxnative (see `nxnative/LICENSE`). Third-party code keeps its own license.
