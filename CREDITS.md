# Credits

Tools, libraries, references and community work used by **Godzilla: Save the Earth, recompiled** (a Replay Revival
project). Community handles only. Game data is never distributed; users supply their own legally owned copy.

## Code in this repository or linked into the build
| Resource | Source | Version | Licence | How used |
|---|---|---|---|---|
| xboxrecomp | https://github.com/sp00nznet/xboxrecomp | v0.14.0 `eb0aa1d` (earlier base `0655e8e`) | MIT; LGPL-2.1+ for APU and NV2A register header (from xemu) | generator, lifter, runtime libraries (base of `xboxrecomp/`) |
| Zippingrocks xboxrecomp fork, branch `godzilla-save-the-earth-runtime` | https://github.com/Zippingrocks/xboxrecomp | `7e93e41` | MIT / LGPL-2.1+ / GPL-2.0+ per its NOTICE | NV2A Kelvin to D3D11 path, combiner shaders and DSP interpreter, merged into `xboxrecomp/` |
| MCPX DSP56300 interpreter | via the fork; originally xemu, Hatari, ARAnyM | as in the fork | GPL-2.0-or-later | audio DSP emulation |
| xemu | https://github.com/xemu-project/xemu | upstream of the above | GPL-2.0 / LGPL-2.1 | origin of the APU, DSP and NV2A code, hardware knowledge |
| stb_image.h | https://github.com/nothings/stb | v2.28 | public domain / MIT | PNG decoding for texture packs |
| SDL3 | https://github.com/libsdl-org/SDL | 3.4.18 | zlib | gamepad input (linked statically) |
| Kenney Input Prompts | https://kenney.nl/assets/input-prompts | as in the downloaded zip (SHA-256 `ac2fcf599080b0f3ba2d174c9474db6df1a0e96ff0662580e2da79a122ab78a1`) | CC0 1.0 | button pictures (Xbox Series, PlayStation Series) for the optional button-prompt texture packs; the packs are built locally by `ste/scripts/build_button_pack.py`, no Kenney files are in this repository |
| AMD FidelityFX Super Resolution 1.0 | https://github.com/GPUOpen-Effects/FidelityFX-FSR | commit `a21ffb8` | MIT | optional scan-out upscaler (shaders ship with the launcher package, not here) |

## Referenced only (no code taken)
| Resource | Source | How used |
|---|---|---|
| Cxbx-Reloaded | https://github.com/Cxbx-Reloaded/Cxbx-Reloaded | kernel export signatures (facts) |
| XbSymbolDatabase | https://github.com/Cxbx-Reloaded/XbSymbolDatabase | identification of XDK library functions in the XBE |
| Godzilla: Destroy All Monsters Melee recompilation (Zippingrocks) | https://github.com/Zippingrocks/Godzilla-Destroy-All-Monsters-Melee-Recompilation | approach reference (per-frame push-buffer hook); our code is independently written |
| GZBuildr and GZ-Model-Editor | Digitzaki, https://github.com/Digitzaki/GZBuildr | Pipeworks bundle / BSF / PVM format references |
| STEM / NastySoft community research | Gojirex, Shame Devs, GR8GODZILLAGOD and the STEM community | specification of the optional gameplay toggles (re-implemented for the Xbox build) |
| Biollante restoration tutorial (Xbox) | DylanRocket, ModDB | method idea only; no text or files copied |
| PCSX2 wiki and patches | https://pcsx2.net, https://github.com/PCSX2/pcsx2_patches | widescreen and culling research references |
| extract-xiso | https://github.com/XboxDev/extract-xiso | extracting the user's own disc image (documented in BUILD.md) |

## Build tools
Microsoft Visual C++ / Windows SDK, CMake, Python 3 (PSF licence). Offline analysis used Ghidra with the ghidra-xbe loader and
vgmstream, 7-Zip, Dolphin tools and similar utilities; none of them is part of this repository.

## The game
*Godzilla: Save the Earth* (Xbox, 2004), developed by Pipeworks Software, published by Atari. Godzilla and related characters are
trademarks of Toho Co., Ltd. Not affiliated with or endorsed by Toho, Atari or Pipeworks.