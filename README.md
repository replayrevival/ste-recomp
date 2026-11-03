# Godzilla: Save the Earth — Recompiled
*A Replay Revival project*

A native PC static recompilation of **Godzilla: Save the Earth** (Xbox, 2004), so the game can be
played on modern hardware at modern resolutions, without emulation.

> **Very early alpha.** Expect bugs, missing features and rough edges.

## Why this exists
This is a personal passion project, built over several months with limited resources. I made it
so I could play this game with my kids, and with this generation, without outdated resolutions
and compatibility problems. If the Godzilla gaming community gets something out of it too, even
better.

This project is **AI-assisted**: much of the reverse-engineering and recompilation work was done
with the help of AI tools, guided and tested by a human. I know not everyone agrees with that
approach. You're free to ignore it, or to use it to help keep the games we love alive.

## Requirements
- Windows 10/11 (64-bit) with a Direct3D 11 GPU (runtime DLLs included)
- **Your own copy of the Xbox version**, extracted to a folder (for example with
  [extract-xiso](https://github.com/XboxDev/extract-xiso)). Disc images (.iso) aren't supported
  directly yet. No game files are included or distributed.
- Windows "N" editions need the Media Feature Pack.

## Getting started
1. Download the latest release from the [Releases](../../releases) page.
2. Extract it to a folder you can write to (not Program Files).
3. Run `launcher.exe`, choose your extracted game folder, pick a preset and press Play.

## Status
**Working:** boots and plays, fights playable, launcher with video presets and settings,
Xbox controllers detected, with dead zone and rumble settings.
**Not yet:** .iso loading (coming next), button remapping in game, some distant buildings still
pop in, PlayStation/Switch pads untested, in-game overlay unfinished.

## Roadmap
1. Load .iso files directly (next update)
2. Stable widescreen and high resolutions, locked 60 fps
3. Controller remapping in game, wider controller testing
4. Optimisation (4K 60 on capable PCs, 1080p 60 on low-end PCs)
5. Enhancements and toggles, including restoring Biollante, who is unused on the disc
6. Linux (and maybe Mac) support
7. Longer term: a full decompilation, laying the groundwork for future Godzilla recomp projects

## Reporting bugs
Open an [issue](../../issues) with your Windows version, GPU, and what happened.

## Legal
Not affiliated with or endorsed by Toho, Atari or Pipeworks. Godzilla and all related trademarks
belong to their respective owners. This project does not include any game assets; you must own
a legal copy of the game to use it.

Licensed under the GPL-3.0. See [LICENSE](LICENSE) and [CREDITS](CREDITS.md).
