# Credits

Tools, libraries, references, and community resources used so far on
**Godzilla: Save the Earth — Recompiled** (a Replay Revival project).

Community handles only. Unknown details marked TBD. Game data is never
distributed; users supply their own legally owned copy.

## Runtime / toolchain (compiled or planned)

| Resource | Source | Version / notes | License | How used |
|---|---|---|---|---|
| xboxrecomp | http<local-folder>/ | v0.14.0 `eb0aa1d` (earlier base `0655e8e`) | MIT (most); LGPL-2.1+ for APU / NV2A register header (from xemu) | Generator, lifter, runtime |
| Zippingrocks xboxrecomp fork (`godzilla-save-the-earth-runtime`) | http<local-folder>/ | `7e93e41` | MIT / LGPL-2.1+ / GPL-2.0+ per NOTICE | NV2A Kelvin→D3D11 path and related |
| MCPX DSP56300 interpreter | via fork; originally xemu / Hatari / ARAnyM | as in fork | GPL-2.0-or-later | Audio DSP emulation |
| NV2A register-combiner shader generator | via fork; port of xemu | as in fork | GPL-2.0-or-later | Pixel shader generation |
| xemu | http<local-folder>/ | upstream of above | GPL-2.0 / LGPL-2.1 | Hardware knowledge / origin of ports |
| stb_image.h | http<local-folder>/ | v2.28 | Public domain / MIT | PNG decode for texture packs |
| SDL3 | http<local-folder>/ | 3.4.18 | zlib | Gamepad input |
| AMD FidelityFX Super Resolution 1.0 (`ffx_a.h`, `ffx_fsr1.h`) | http<local-folder>/ | commit `a21ffb8` | MIT | Optional scan-out upscaler |

## Referenced only (no project redistribution of their code unless noted)

| Resource | Source | How used |
|---|---|---|
| Godzilla: Destroy All Monsters Melee recompilation | http<local-folder>/ | Approach reference |
| Cxbx-Reloaded | http<local-folder>/ | Kernel export signatures (data) |
| XbSymbolDatabase | http<local-folder>/ | XDK library identification |
| GZBuildr | Digitzaki — http<local-folder>/ | Pipeworks bundle/BSF/PVM formats |
| GZ-Model-Editor | Digitzaki | Planned model import/export |
| Biollante restoration tutorial (Xbox) | DylanRocket (ModDB / Wayback) | Method idea only; no text/files copied |
| STEM / NastySoft community research | Gojirex, Shame Devs, GR8GODZILLAGOD, and STEM community | Formats, mods, roster leads |
| Spigot-engine symbol idea (Namco Museum 50th) | DylanRocket | Naming approach |
| SuperCheats STE Xbox cheat page | facts only (gesture + codes) | Unlock testing notes |
| PCSX2 wiki / patches (research) | http<local-folder>/ / http<local-folder>/ | Widescreen/cull research refs |
| TCRF DAMM page | community wiki | Cut-content research |
| STEM HD texture pack (GSTEM) | NastySoft / STEM | Local matching experiments only; not redistributed |
| STEM gameplay pnach specs | STEM / NastySoft | Spec for Xbox re-implementations |

## Offline tools (not shipped in this repo)

| Tool | Source | License | How used |
|---|---|---|---|
| MSVC + CMake | Microsoft | proprietary | Build |
| Python 3 | python.org | PSF | Scripts / probes |
| LLVM llvm-pdbutil / DbgHelp | LLVM / Microsoft | various | PDB / symbols |
| 7-Zip | http<local-folder>/ | LGPL (+ unRAR restriction) | Archive extraction |
| maxcso | http<local-folder>/ | GPL-3.0-or-later | CSO/ZSO handling (research) |
| Dolphin / DolphinTool | http<local-folder>/ | GPL-2 | Disc partition tools (research) |
| extract-xiso | http<local-folder>/ | Modified Berkeley-style | Xbox ISO extract (research) |
| vgmstream | http<local-folder>/ | LGPL-2.1-or-later | Audio decode tests |
| WinDbg / cdb | Microsoft | MS license | Crash dumps (offline) |
| Real-ESRGAN-ncnn-vulkan | http<local-folder>/ | MIT / BSD-3 models | Offline texture upscale pilot |
| Pillow / imagehash / numpy | PyPI | HPND / BSD | Offline texture match tooling |
| Claude / Claude Code | Anthropic | proprietary | Assisted analysis (not shipped) |

## Game

Godzilla: Save the Earth (Xbox, 2004) — developed by Pipeworks Software, published by Atari.
Godzilla and related characters are trademarks of Toho Co., Ltd.
Not affiliated with or endorsed by Toho, Atari, or Pipeworks.

## Project license note

Historical project decision: GPL-2.0-or-later for own sources. This export package
includes the full GPL-3.0 license text in `LICENSE` for the initial public package.
Third-party notices and SPDX inventory for a full binary release: TBD.

## Living source list

A more detailed working ledger (including paths that must not ship) lives in
`docs/CREDITS_AND_SOURCES.md` and still needs privacy scrub before any broader
distribution beyond this private staging repo.
