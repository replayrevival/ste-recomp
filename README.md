<p align="center">
  <img src="docs/brand/replay-revival-banner.png" alt="Replay Revival" width="460">
</p>

<h3 align="center">Godzilla: Save the Earth, rebuilt to run natively on your PC:<br>widescreen, 4K, 60 fps, modern controllers.</h3>

<p align="center">
  <a href="https://github.com/replayrevival/ste-recomp/releases/latest"><img alt="Latest release" src="https://img.shields.io/github/v/release/replayrevival/ste-recomp?include_prereleases&label=release"></a>
  <a href="LICENSE"><img alt="Licence: GPL-2.0-or-later" src="https://img.shields.io/badge/licence-GPL--2.0--or--later-blue"></a>
  <img alt="Platform: Windows 10/11 64-bit" src="https://img.shields.io/badge/platform-Windows%2010%2F11-lightgrey">
  <img alt="Status: early alpha" src="https://img.shields.io/badge/status-early%20alpha-orange">
</p>

![Godzilla 2000's atomic breath hits Gigan in Tokyo at 21:9 ultrawide](docs/screenshots/0.1.5_hero_ultrawide_fight_tokyo.png)

*Godzilla 2000's atomic breath hits Gigan in Tokyo at 21:9 ultrawide, with Unlimited draw distance: the whole street, Mount Fuji and the original HUD in view.*

## What you get

| | |
|---|---|
| 🖥️ **Widescreen and ultrawide** | 16:9, 21:9 ultrawide and wider screens show more of the city. Nothing is stretched. |
| 🔍 **4K and 60 fps** | Sharp at up to 4K. The game runs at 60 frames per second and at the correct speed. |
| 🏙️ **Unlimited draw distance** | See the whole city at a smooth 60 fps. |
| 🎮 **Any controller** | Use almost any gamepad, remap every button, and pick Xbox, PlayStation or original Xbox button pictures. |
| 🚀 **A friendly launcher** | Pick a preset, change settings, set up controllers and press Play. No config files. |
| 🎛️ **In-game menu** | Press Back and Start or F1 to change settings or go back to the launcher without leaving the game. |
| 🧩 **Mods** | Install and switch mods from the launcher, including mods that change the game's own files without sharing them. Your game files are never changed. |
| 🎯 **Original gameplay by default** | The ORIGINAL preset plays the game as it was. Extras are optional. |
| 🆓 **Free and open source** | GPL licence. The source is in this repository. |

![Original 4:3 next to 21:9](docs/screenshots/compare_original_vs_ultrawide.jpg)

*The same moment: the original 4:3 view on the left, 21:9 on the right.*

![The Tokyo skyline runs all the way to the hills and the harbour](docs/screenshots/0.1.5_ultrawide_tokyo_skyline_unlimited.png)

*Unlimited draw distance: the Tokyo skyline runs all the way to the hills and the harbour.*

## Play it in 3 steps

1. **Download** the zip from [Releases](https://github.com/replayrevival/ste-recomp/releases/latest) and unpack it anywhere.
2. **Run the launcher** (`launcher.exe`) and point it at your own copy of the game: a disc image (.iso) or a folder.
3. **Press PLAY.**

![The launcher](docs/screenshots/0.1.5a_launcher_overview.png)

## What you need

- Your own **Xbox copy of Godzilla: Save the Earth** (a backup of your disc, as an .iso, or the files in a folder).
- **Windows 10 or 11**, 64-bit.
- A graphics card that supports **DirectX 11**.
- A controller is recommended. The keyboard works too.

## What's new in v0.1.5a

- **An in-game menu.** Hold Back and Start together for a second (or press F1) and the game pauses. Change the volume or other settings, set up your controllers, or go back to the launcher, then carry on playing. The Guide / Home button can open it too if you switch that on in the launcher's Controllers tab.
- **0.1.5a fixes a freeze when opening that menu** (seen on a real PC in true fullscreen: the window stopped answering and Alt+F4 did nothing). The window now keeps answering while the menu is open, and closing it ends the game.
- **The words follow your controller style.** With Xbox or PlayStation button pictures chosen, texts such as "PRESS START" now say "PRESS MENU" (or "PRESS OPTIONS" on PlayStation where it fits) and "Xbox console" says "this computer". Original Xbox keeps the original words.
- Volume and the mouse pointer change at once from the in-game menu. Button prompts and the enhancements apply from the next match; other video settings the next time you start.

<table>
<tr>
<td><img src="docs/screenshots/0.1.5a_overlay_paused.png" alt="The in-game menu over the game: Resume, Video, Enhancements, Controllers, Return to launcher, Quit game"></td>
<td><img src="docs/screenshots/0.1.5a_overlay_video.png" alt="The in-game menu's Video page with the Volume row"></td>
</tr>
<tr>
<td colspan="2"><img src="docs/screenshots/0.1.5_press_menu_wording_playstation.png" alt="With PlayStation button pictures the text says 'Press MENU to add COM'"></td>
</tr>
</table>

Earlier: 0.1.4 made the game start about 20 seconds sooner and made mods that change the game's own files work. 0.1.3a fixed a rare freeze. 0.1.3 brought unlimited draw distance at a smooth 60 fps, a big performance boost, Xbox / PlayStation / original Xbox button pictures, the Mods tab and Save crash report.

[Read the full release notes](https://github.com/replayrevival/ste-recomp/releases/tag/v0.1.5a-alpha).

**Coming next:** typing names with a keyboard, the missing Mechagodzilla missile trails, then a beta, then 1.0. Linux and Mac come later.

<table>
<tr>
<td><img src="docs/screenshots/0.1.5a_launcher_video.png" alt="Video settings"></td>
<td><img src="docs/screenshots/0.1.5a_launcher_controllers_playstation.png" alt="Controller settings with PlayStation button pictures"></td>
</tr>
<tr>
<td><img src="docs/screenshots/0.1.5_button_prompts_xbox.png" alt="The character-select screen with Xbox button pictures"></td>
<td><img src="docs/screenshots/0.1.5_button_prompts_playstation.png" alt="The character-select screen with PlayStation button pictures"></td>
</tr>
</table>

## FAQ

**Do I need the game?** Yes. This project contains none of the game. You need your own copy.

**Is this an emulator?** No. An emulator pretends to be the console and runs the original game inside it. Here the game's own program was translated once into a normal Windows program, so it runs on your PC like any other game.

**Is it legal?** The project includes no game code and no game files, and you must own the game. It is an unofficial fan project. Laws differ from country to country, and this is not legal advice.

**Why the Xbox version?** The Xbox release has the sharpest textures and effects of the original versions, so it's the best starting point.

**Can I mod it?** Yes. The launcher installs mods from a zip or a folder. See the [modding guide](https://github.com/replayrevival/launcher/blob/main/docs/MODDING.md). Mods must never contain game files.

**Linux, Mac or Steam Deck?** Windows only for now. Linux and Mac are planned for later. A guide for trying it with Proton on Linux is in [docs/TESTING_PROTON.md](docs/TESTING_PROTON.md), but it is not officially supported yet.

**Where do I report bugs?** If the game stops, press **Save crash report** in the launcher and attach the zip to an [issue](https://github.com/replayrevival/ste-recomp/issues) after you have looked it over. Please add your Windows version, graphics card and what you did. Never attach game files.

## Disclaimer

This is an unofficial fan project. It is not affiliated with or endorsed by the game's publishers or rights holders (Toho, Atari or Pipeworks). No game files are included. Godzilla and all related names and trademarks belong to their owners. You must own the game.

---

# For developers

> **Early alpha.** Work in progress. Expect bugs and rough edges.

> **No game code and no game data are published here.** The recompiled C is generated on your machine from your own copy of `default.xbe` (see [BUILD.md](BUILD.md)) and is git-ignored.

## How it works
This is a native PC *static recompilation* of **Godzilla: Save the Earth** (Xbox, 2004). The game's own x86 code is translated to C once, compiled for Windows, and run against a small replacement of the Xbox kernel, GPU (NV2A "Kelvin" to Direct3D 11), audio and USB hardware. There is no emulator and no game data in this repository.

## Why this exists
A personal passion project, built over several months with limited resources, so the game can be played on modern machines at modern resolutions without compatibility problems. If the Godzilla community gets something out of it too, even better.

It is **AI-assisted**: much of the reverse-engineering and recompilation work was done with AI tools, guided and tested by a human. You are free to ignore that, or to use the work to help keep the games we love alive.

## What is in this repository
| Path | What |
|---|---|
| `xboxrecomp/` | Runtime libraries (kernel, Direct3D 8 and NV2A "Kelvin" to Direct3D 11, MCPX audio and DSP, USB/OHCI, input, video) and the recompiler/generator (`xboxrecomp/tools/recomp`). Based on [sp00nznet/xboxrecomp](https://github.com/sp00nznet/xboxrecomp) v0.14.0 with this project's changes; see `xboxrecomp/NOTICE` and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). |
| `ste/src/` | The game-specific executable: `main.c` (start-up, command line, crash handling), `recomp_manual.c` (hand-written replacements and options), `ste_pb_hook.c` (per-frame hook, window, dumps, watchdog), `ste_sdlinput.c` (SDL3 gamepads). |
| `ste/gen/` | Inputs of the generator: `functions.tsv` (function address ranges), `ste_hooks.toml` (instruction hooks), `exclude_manual.c` (functions replaced by hand). Addresses only. |
| `ste/scripts/` | `build.ps1` (whole build), `generate.py` (code generation from your XBE), `verify_generated.py` (regression checks on the generated code). |
| `docs/` | [LAUNCHER_INTERFACE.md](docs/LAUNCHER_INTERFACE.md) (command line, environment, ini keys, exit codes, log lines), [XDK_FUNCTIONS.md](docs/XDK_FUNCTIONS.md) (what is recompiled and what is replaced), file formats, hooks. |

The launcher (game folder picker, presets, settings, mods) is a separate program in [replayrevival/launcher](https://github.com/replayrevival/launcher).

## Building
Windows 10/11, Visual Studio 2022 Build Tools, CMake, Python 3, and your own extracted `default.xbe`:

```
.\ste\scripts\build.ps1 -Xbe "D:\path\to\extracted\default.xbe"
```
Details, expected output and the validation steps are in [BUILD.md](BUILD.md).

## Running from the command line
`ste-recomp.exe --game <folder that contains default.xbe>`; options are read from `ste_video.ini` / `ste_enhance.ini` (see [docs/LAUNCHER_INTERFACE.md](docs/LAUNCHER_INTERFACE.md)). Disc images (.iso) are unpacked by the launcher; the game program itself reads folders.

## Licence and credits
Own code: **GPL-2.0-or-later** (see [LICENSE](LICENSE)). Third-party components keep their own licences ([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), [CREDITS.md](CREDITS.md), `xboxrecomp/NOTICE`). Binary releases are published together with the matching source from this repository.
