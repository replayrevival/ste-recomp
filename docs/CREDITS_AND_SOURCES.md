# Credits, sources and third-party code

> Authoritative credits: see [CREDITS.md](../CREDITS.md) (two parts: code and assets used / research and inspiration). This file is the detailed working record.

Living record of every external resource used or referenced by this project. Required before any public
release. RULE: whenever a new tool, repo, library, document, dataset or person's work is used or consulted,
add a row here (what, where, version/commit, license, how it was used, code copied yes/no).

Game data is never distributed. Users supply their own legally owned copy.

## Code compiled into the build
| Resource | Source | Version / commit | License | How used | Code copied |
|---|---|---|---|---|---|
| xboxrecomp (toolchain + runtime) | https://github.com/sp00nznet/xboxrecomp | v0.14.0 `eb0aa1d` (earlier base `0655e8e`) | MIT (most files); LGPL-2.1+ for `src/apu` and NV2A register header (from xemu) | Generator, lifter, runtime libs (kernel, APU, video, input) | Yes (base) |
| stb_image.h (PNG decoder for texture packs) | https://github.com/nothings/stb ; the copy compiled in (src/nv2a/third_party/stb_image.h, sha256 2D264EC3E6CE404FC2D0CFFF4476C982EB3EDF5A61122B03884FDEF7BACD9DEB) was taken from E:\GodzillaSTE\tools\imhex\sdk\lib\third_party\imgui\backend\include\stb_image.h | v2.28 (header says "stb_image - v2.28") | Public domain / MIT (dual, see end of file); GPL-compatible | Decodes replacement-texture PNGs in kelvin_texpack.c (c128, STBI_ONLY_PNG) | Yes (header copied unmodified; STBI_ONLY_PNG/STBI_NO_STDIO defined by the includer) |
| Zippingrocks xboxrecomp fork, branch `godzilla-save-the-earth-runtime` | https://github.com/Zippingrocks/xboxrecomp | `7e93e41` (2026-10-11) | MIT / LGPL-2.1+ / GPL-2.0+ per its NOTICE | NV2A Kelvin->D3D11 renderer (`nv2a_kelvin.c/.h`), `nv2a_pgraph_d3d11.c` changes, d3d8 device/shader/combiner changes | Yes (merged into v014k) |
| MCPX DSP56300 interpreter (`src/apu/dsp/`) | via Zippingrocks fork; originally xemu / Hatari / ARAnyM | as in `7e93e41` | GPL-2.0-or-later | GP/EP audio DSP emulation | Yes |
| NV2A register-combiner shader generator port (`d3d8_nv2a_psh.*`) | via Zippingrocks fork; port of xemu | as in `7e93e41` | GPL-2.0-or-later | Pixel shader generation | Yes |
| Kelvin GPU vertex programs (`nv2a_kelvin.c` HLSL generator, `d3d8_kvp.c/.h`), c69 | this project; instruction semantics and microcode field layout follow xemu `hw/xbox/nv2a/pgraph/glsl/vsh-prog.c` (already used by the CPU interpreter) | xemu master as consulted 2026-10 | GPL-2.0-or-later (derived semantics) | NV2A vertex program -> HLSL vertex shader, cached by program hash; CPU interpreter kept as `KELVIN_CPU_VP=1` fallback and verifier | No verbatim copy; generator written from the interpreter's decode, semantics from xemu |
| xemu | https://github.com/xemu-project/xemu | (upstream of the above) | GPL-2.0 / LGPL-2.1 | Origin of APU, DSP, NV2A code and hardware knowledge | Indirectly (above) |

GPL NOTE: the DSP interpreter and combiner port are GPL-2.0+. Distributing a linked executable requires
publishing the corresponding source.

## Referenced only (no code copied)
| Resource | Source | License | How used |
|---|---|---|---|
| Godzilla: Destroy All Monsters Melee recompilation (Zippingrocks) | https://github.com/Zippingrocks/Godzilla-Destroy-All-Monsters-Melee-Recompilation (`9083e32`) | none stated (all rights reserved) | Read for the approach of a per-frame pushbuffer -> Kelvin hook; our hook (`tests/tools/ste_pb_hook*.c`) is independently written |
| Cxbx-Reloaded kernel export sources | https://github.com/Cxbx-Reloaded/Cxbx-Reloaded | GPL-2.0 | Kernel function signatures parsed as data for the argument-size audit |
| XbSymbolDatabase | https://github.com/Cxbx-Reloaded/XbSymbolDatabase | ODbL / see repo | Identified XDK library functions in default.xbe (XPP input functions, D3DDevice_Swap) |
| NV2A / NV097 method names | xemu `nv2a_regs.h`, xboxrecomp `nv2a_regs.h` | LGPL-2.1+ | Naming pushbuffer methods |

## Tools
| Tool | Use |
|---|---|
| Microsoft Visual C++ (MSVC), CMake | Building generated code and runtime |
| Python 3 (lab venv) | Generator, assembly scripts, probes |
| LLVM `llvm-pdbutil`, Microsoft DbgHelp | Symbol extraction from PDBs |
| WSL git | Version control / diffs |
| Claude (Anthropic), Claude Code | AI-assisted analysis, probes and code changes |

## Community research (reference / documentation)
| Resource | People | How used |
|---|---|---|
| GSTE Knowledge Database (`docs/community/GSTE_Knowledge_Database.md`) | Shame Devs, Gojirex, GR8GODZILLAGOD and the STEM community | File-format and game-data background |
| Godzilla: Save the Earth - Melee (STEM) / NastySoft Discord research (`docs/community/STE_Discord_Technical_Research.md`) | STEM / NastySoft community members named in that file | Format knowledge, modding leads, contacts |
| GZBuildr | Digitzaki - https://github.com/Digitzaki/GZBuildr | Pipeworks bundle/BSF/PVM format reference; planned mod packing tool |
| GZ-Model-Editor | Digitzaki | Planned model import/export for mods |
| Biollante restoration tutorial (Xbox) | DylanRocket - https://www.moddb.com/members/dylanrocket/blogs/unlocking-biollante-in-godzilla-save-the-earth-xbox (read 2026-10-23 via Wayback snapshot 2020-05-17: http://web.archive.org/web/20200517013723id_/https://www.moddb.com/members/dylanrocket/blogs/unlocking-biollante-in-godzilla-save-the-earth-xbox , saved at F:\GodzillaFamily_Assets\reports\hermes9\tutorial.md; moddb itself returns 403) | Idea/method of replacing an existing slot with Biollante: the tutorial renames her bundle over another monster's .zip/.cmf (CMF inside ZIP, same name). Correction: it does NOT edit ControlRoom.bdl/ConventionalGame.bsf (earlier note here was wrong). No text or files copied; our pack uses name-string swaps instead |
| Godzilla: Save the Earth - Melee (STEM) | Gojirex and the STEM/NastySoft team | Source of community characters (planned); roster info |
| Spigot-engine symbol idea (Namco Museum 50th) | DylanRocket | Approach for naming functions |

## Game
Godzilla: Save the Earth (Xbox, 2004) - developed by Pipeworks Software, published by Atari. Godzilla and
related characters are trademarks of Toho Co., Ltd. Not affiliated with or endorsed by any of them.

## Tools (additional downloaded tools; appended)
| Tool | Version | Source | Licence | How used |
|---|---|---|---|---|
| 7-Zip | 26.04 | https://www.7-zip.org/ | GNU LGPL with 7-Zip unRAR restriction (per distributed licence) | Command-line extraction of Dolphin archives and PS2 ISO contents; not used to modify source media |
| maxcso | 1.13.0 | https://github.com/unknownbrackets/maxcso/releases | GNU GPL-3.0-or-later (project licence) | Command-line decompression of the Unleashed PS2 ZSO to ISO |
| Dolphin / DolphinTool | 2509 x64 | https://dolphin-emu.org/download/ | GNU GPL v2 (included `license.txt`) | `DolphinTool.exe` DATA-partition extraction of the Unleashed Wii WBFS; GUI emulator not used |
| GZBuildr (Digitzaki) | GZ_3.0.2 / GZ_DAMMSDAY | https://github.com/Digitzaki/GZBuildr/releases | GNU GPL v3 (included `LICENSE`) | Pipeworks bundle parsing, entry listing, and format comparison; local changes include the `light_bdg.py` shim and `monster_data_txt_tool.py` fixes |
| extract-xiso | 2.7.1 (`build-202609111233`) | https://github.com/XboxDev/extract-xiso/releases | MIT (included `LICENSE.TXT`) | Command-line extraction of the DAMM Xbox ISO; source media remained read-only |
| extract-xiso licence clarification | 2.7.1 included `LICENSE.TXT` | https://github.com/XboxDev/extract-xiso/releases | Modified Berkeley-style permissive licence, copyright in@fishtank.com; not MIT | Corrects the licence description for the downloaded extractor |

| vgmstream r2117 | vgmstream contributors | https://github.com/vgmstream/vgmstream/releases/tag/r2117 | LGPL-2.1-or-later (project components; see included COPYING) | Portable CLI decode test for STEM PS2 `.mic` and STE Xbox `.wxv`; not installed. |

| SuperCheats (Godzilla: Save the Earth Xbox cheat page) | as relayed by the project owner 2026-10-22 | SuperCheats (page named by the owner; no text copied) | n/a (facts only: menu gesture and 3 numeric codes) | Cheat-entry gesture and codes 525955 / 659996 / 975013 used to unlock content in a test profile; findings in docs\community\XBOX_CHEAT_MENU.md |

| Pop-in research (Hermes) 2026-10-22 | reports in F:\GodzillaFamily_Assets\reports\popin (SUMMARY.md, SOURCES.md, popin_keys.md, stage_geometry.md, frontend_textures.md, widescreen_patches.md) | read-only research by the project's helper agent; no code or data copied | n/a | Texture names/sizes for the front-end glow (GFC_Globe_Halo 16x16 DXT3, Bubbles_1/2/3/6 32x32, static 256x64) used to trace draws (STE_TEXTRACE); cull/fog key survey for step 3 |
| GZBuildr / Digitzaki | https://github.com/Digitzaki/GZBuildr | GPL-3.0 (see GZBuildr row above) | n/a | used by the Hermes reports (PipeworksParser) |
| PCSX2 Godzilla: Save the Earth game page | https://wiki.pcsx2.net/index.php?mobileaction=toggle_view_desktop&title=Godzilla%3A_Save_the_Earth | wiki text | n/a | consulted by the Hermes widescreen/cull research (reference only) |
| PCSX2 patches repository (SLUS-20809 / SLES-52766 widescreen, SLUS-21707 60 FPS pnach) | https://github.com/PCSX2/pcsx2_patches (raw files under patches/) | per repository | patches not used; FOV/render-setup patch values read as research references | widescreen_patches.md: PS2 patches alter FOV/render setup, not culling |
| xemu DAMM compatibility page | https://xemu.app/titles/49470039/ | web page | n/a | consulted (no widescreen/cull patch found) |
| Godzilla DAMM recompilation README (Zippingrocks) | https://raw.githubusercontent.com/Zippingrocks/Godzilla-Destroy-All-Monsters-Melee-Recompilation/HEAD/README.md | per repository | n/a | consulted (frontend/rendering parity notes) |
| The Cutting Room Floor: Godzilla - Destroy All Monsters Melee | https://tcrf.tiago.zip/wiki/Godzilla-3a_Destroy_All_Monsters_Melee | wiki text | n/a | consulted (cut content research) |
| PCSX2 widescreen patch list (KrystianLesniak) | https://github.com/KrystianLesniak/pcsx2_patches_list | per repository | n/a | consulted |
| STEM HD texture pack (GSTEM 0.1.6, PCSX2 replacement DDS set, 3,538 files) | NastySoft / STEM community, as staged in F:\GSTEM_Community_Assets (see the STEM rows above) | GSTEM 0.1.6 | per the authors (not stated in the archive; not redistributed by us) | Q3: tools\mods\texmatch.py matches Xbox STE textures to these by perceptual hash and can write a LOCAL texture pack (artifacts\texturepacks\stem_hd_auto); the pack manifest credits STEM |
| STEM HD texture catalogue (Hermes6) | F:\GodzillaFamily_Assets\reports\hermes6\stem_hd_catalogue.csv / .md (Pillow 13.0.0, imagehash 4.3.2) | 2026-10 | n/a (derived index of the above) | Perceptual hashes (pHash, dHash, 64x64 pHash) of every STEM DDS, input of texmatch.py |
| Pillow 13.0.0 and imagehash 4.3.2 (+ numpy) | https://python-pillow.org/ ; https://github.com/JohannesBuchner/imagehash | as pinned in F:\GodzillaFamily_Assets\tools\venv_img | HPND (Pillow); BSD-2-Clause (imagehash); BSD-3-Clause (numpy) | Offline tooling only (texmatch.py, make_test_texpack.py); nothing is compiled into the game |

## Licence of this project (owner decision 2026-10-22)
The STE recompilation project is released under the GNU General Public License, **version 2 or (at your option) any later version (GPL-2.0-or-later)**. Our own source files carry
`SPDX-License-Identifier: GPL-2.0-or-later`. Third-party code compiled into the runtime must be GPL-compatible and is listed in this file with its licence and exact commit. Known state (Hermes audit,
F:\GodzillaFamily_Assets\reports\hermes2\license_audit.md; drafts in hermes3\release_drafts): the merged runtime already contains GPL-2.0-or-later components (MCPX DSP56300 interpreter, NV2A PSH port), so MIT-only is not possible; LGPL-2.1-or-later and MIT
components keep their notices. Nothing identified forces GPL-3.0 (hermes3 summary). **If Apache-2.0 code is compiled in, the binaries must ship under GPL-3.0.** Before the first public release: audit the merged tree
file by file (SPDX inventory per compiled object), pin immutable commits (xemu, fork subtrees), ship LICENSE / THIRD_PARTY_NOTICES / corresponding source, and never ship game data or mods containing game data (GAME_DATA_NOTICE draft).
GZBuildr (GPL-3.0), vgmstream, maxcso, Dolphin and 7-Zip are offline tools, not compiled into the runtime.| Water wake/foam reference 2026-10-25 | https://www.youtube.com/watch?v=cGVQBw_bK2U ('Free Roaming Boston (Mod) - Godzilla: Save The Earth (PS2)') | reference video only (shows the foam/splash around a wading monster); nothing copied | n/a | used to look for the same effect in the Xbox game (item E) |

| Debugging Tools for Windows (WinDbg 1.2610.1001.0, cdb 10.0.29661.1004) 2026-10-26 | https://aka.ms/windbg/download -> windbg.msixbundle (SHA-256 4662AE17BC0DC77DB987B89CCCEA32C30058BAA06268F861D09E0AB4E4F45C20, Authenticode valid); x64 msix extracted to F:\GodzillaSTE_tools\debuggers\windbg_x64_1.2610.1001.0 (portable, nothing installed system-wide) | Microsoft Software License Terms; not redistributed, no code copied, not part of the project release; used offline for crash minidump analysis of our own runs (cdb -z dump -y pdbs, .ecxr / kn) |

| Real-ESRGAN-ncnn-vulkan 20220424 (Windows build, models realesrgan-x4plus) 2026-10-26 | https://github.com/xinntao/Real-ESRGAN/releases/tag/v0.2.5.0 -> realesrgan-ncnn-vulkan-20220424-windows.zip (SHA-256 ABC02804E17982A3BE33675E4D471E91EA374E65B70167ABC09E31ACB412802D); also realesrgan-ncnn-vulkan-v0.2.0-windows.zip from xinntao/Real-ESRGAN-ncnn-vulkan (SHA-256 1BBBDB12D470AF80B035C773682E144C6C2F6ECE9210832A289AF0A48CE3FA9A); portable under F:\GodzillaSTE_tools\upscale (nothing system-wide) | ncnn-vulkan: MIT (Copyright 2021 Xintao Wang); Real-ESRGAN models: BSD-3-Clause; not shipped, not compiled in; planned offline texture upscaler for the user's own dumped textures (Q3 pilot) |

| AMD FidelityFX Super Resolution 1.0 (FSR 1: EASU + RCAS), files ffx_a.h and ffx_fsr1.h, commit a21ffb8f6c13233ba336352bdff293894c706575 2026-10-26 | https://github.com/GPUOpen-Effects/FidelityFX-FSR (raw files downloaded; copies in mission-l6\artifacts\fsr1 and F:\GodzillaSTE_tools\fsr1; ffx_a.h SHA-256 F60E2722FCD13989523B9164D776AB382B3692791767F3BF8BB19967F763F3FB, ffx_fsr1.h SHA-256 93C3922362EA7FC99CBCC698CA30C98DE4F8C246D1FBB0B09E015DDEF38CE3A5) | MIT, Copyright (c) 2021 Advanced Micro Devices, Inc. (license text in artifacts\fsr1\license.txt) | used unmodified as run-time HLSL (compiled from <exe dir>\fsr1) by the scan-out upscaler (render_scale / upscale=fsr1); only two thin pixel-shader wrappers and the constant set-up (FsrEasuCon / FsrRcasCon written out) are ours; must be listed in THIRD_PARTY_NOTICES for releases |
| STEM 1.7 gameplay patches (PCSX2 .pnach 'STEM-11354_2EBDF94F', comments + patched addresses only) | STEM / NastySoft team | F:\GSTEM_Community_Assets\extracted (local copy, not shipped) | n/a (community patch; no code copied) | c193+: used as the SPECIFICATION for stem_no_juggle_limit / stem_no_knockback_scaling / stem_no_min_health; each was re-located in the Xbox code (PS2 0x003ABA5C -> Xbox sub_0011E490, 0x003C2260/78 -> sub_0010D2E0, 0x003BF7EC -> constant 0x00313050 of sub_00112AC0) and re-implemented; no PS2 code or data is included |

| SDL3 3.4.18 (libsdl-org/SDL), static library, zlib licence (LICENSE.txt SHA-256 1C040B8271B37E5076359F8FD54240E371114112924D2DF81EF87C7D6A1DFDFD) 2026-11-02 | https://github.com/libsdl-org/SDL (source in E:\GodzillaSTE\launcher\.deps\sdl3-src, shared with the launcher; built with MSVC to E:\GodzillaSTE\builds\sdl3-msvc: static, /MD, render/GPU/camera/audio/dialog/vulkan/opengl off) | linked into the game exe for gamepad input (ste_sdlinput.c: SDL_Gamepad, rumble, virtual joysticks for tests) | zlib (GPL-compatible) | no SDL source copied into our files; notice goes into THIRD_PARTY_NOTICES at release |

## Kenney Input Prompts 1.5 (added 2026-11-12)
- What: controller glyph art. Source: https://kenney.nl/assets/input-prompts, zip kenney_input-prompts_1.5.zip, SHA-256 ac2fcf599080b0f3ba2d174c9474db6df1a0e96ff0662580e2da79a122ab78a1. Licence: CC0 1.0 (public domain dedication; credit appreciated, not required).
- Used by: tools\mods\build_button_pack.py (mission-l6) -> texture packs button_prompts_xbox / button_prompts_playstation. The only files used (listed in each pack's USED_FILES.txt): Xbox Series/Default/xbox_button_color_{a,b,x,y}.png and PlayStation Series/Default/playstation_button_color_{cross,circle,square,triangle}.png. The zip itself lives only in launcher\assets\glyphs\ (one copy); the game side reads it from there, nothing is duplicated.
- Code copied: none.
| ANILORAK's Save the Earth modding pack (format research) | ANILORAK | pack zip SHA-256 47bca3bf4a249ec35d71cb4ef95c12ebda34f2c03b57ae9a3d5e77e42c39b2f8, 2026-11-19 | free for anyone except one named person (incompatible with GPL: nothing of it is copied) | read locally to learn: the PS2 file layout (PVOL), the script-native names/ids and PS2 body addresses (FUNCTION_NAMES_FROM_PS2.tsv keeps only the game's own native names and PS2 addresses), the bundle chunk types; his read-only tools were run on copies only | none copied |

| Solo Destruction mod (0.3) | ANILORAK (idea: the "Solo Destruction" mode of his Godzilla Ultra research); Digitzaki's GZBuildr (GPL-3.0) used offline only as a cross-check of our own editor | 2026-11-20 | concept: free for anyone except one named person (nothing copied); GZBuildr: GPL-3.0, not bundled, not required | the idea of a solo city-destruction mode; our own token-level script editor (`tools\modssf_edit.py`) applies our own edits to four of the user's own scripts on the user's PC (recipe mod); GZBuildr output was compared offline | none copied (no GZBuildr code or files in the repo) |
