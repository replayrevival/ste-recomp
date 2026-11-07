# Godzilla: Save the Earth, recompiled: source

*A Replay Revival project.* Source code of the runtime and the build tooling behind the Windows release.

A native PC static recompilation of **Godzilla: Save the Earth** (Xbox, 2004): the game's own x86 code is translated
to C once, compiled for Windows, and run against a small replacement of the Xbox kernel, GPU, audio and USB hardware.
No emulator, no game data in this repository.

> **Early alpha.** Work in progress, development continues. Expect bugs and rough edges.

> **No game code and no game data are published here.** The recompiled C is generated on your machine from your own
> copy of `default.xbe` (see [BUILD.md](BUILD.md)) and is git-ignored. You must own the game.

## Why this exists
A personal passion project, built over several months with limited resources, so the game can be played with the
next generation at modern resolutions without compatibility problems. If the Godzilla community gets something out
of it too, even better.

It is **AI-assisted**: much of the reverse-engineering and recompilation work was done with AI tools, guided and
tested by a human. You are free to ignore that, or to use the work to help keep the games we love alive.

## What is in this repository
| Path | What |
|---|---|
| `xboxrecomp/` | Runtime libraries (kernel, Direct3D 8 and NV2A "Kelvin" to Direct3D 11, MCPX audio and DSP, USB/OHCI, input, video) and the recompiler/generator (`xboxrecomp/tools/recomp`). Based on [sp00nznet/xboxrecomp](https://github.com/sp00nznet/xboxrecomp) v0.14.0 with this project's changes; see `xboxrecomp/NOTICE` and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). |
| `ste/src/` | The game-specific executable: `main.c` (start-up, command line, crash handling), `recomp_manual.c` (hand-written replacements and options), `ste_pb_hook.c` (per-frame hook, window, dumps, watchdog), `ste_sdlinput.c` (SDL3 gamepads). |
| `ste/gen/` | Inputs of the generator: `functions.tsv` (function address ranges), `ste_hooks.toml` (instruction hooks), `exclude_manual.c` (functions replaced by hand). Addresses only. |
| `ste/scripts/` | `build.ps1` (whole build), `generate.py` (code generation from your XBE), `verify_generated.py` (regression checks on the generated code). |
| `docs/` | [LAUNCHER_INTERFACE.md](docs/LAUNCHER_INTERFACE.md) (command line, environment, ini keys, exit codes, log lines), [XDK_FUNCTIONS.md](docs/XDK_FUNCTIONS.md) (what is recompiled and what is replaced), file formats, hooks. |

The launcher (game folder picker, presets, settings) is a separate program and is not part of this repository.

## Building
Windows 10/11, Visual Studio 2022 Build Tools, CMake, Python 3, and your own extracted `default.xbe`:

```
.\ste\scripts\build.ps1 -Xbe "D:\path\to\extracted\default.xbe"
```
Details, expected output and the validation steps are in [BUILD.md](BUILD.md).

## Running
`ste-recomp.exe --game <folder that contains default.xbe>`; options are read from `ste_video.ini` / `ste_enhance.ini`
(see `docs/LAUNCHER_INTERFACE.md`). Disc images (.iso) are not mounted yet: extract them first.

## Reporting bugs
Open an issue with your Windows version, GPU, and what happened. Please attach the log, never game files or dumps.

## Legal
Not affiliated with or endorsed by Toho, Atari or Pipeworks. Godzilla and all related trademarks belong to their
respective owners. This project contains no game assets; you must own a legal copy of the game.

Own code: **GPL-2.0-or-later** (see [LICENSE](LICENSE)). Third-party components keep their own licences
([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), [CREDITS.md](CREDITS.md), `xboxrecomp/NOTICE`). Binary releases are
published together with the matching source from this repository.