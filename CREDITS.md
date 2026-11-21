# Credits

**Godzilla: Save the Earth, recompiled** (a Replay Revival project) stands on other people's work. This file has two clearly
separate parts: what is **actually included** in this project (with licences), and the **research and inspiration** behind it
(nothing from those is included). Community handles only. Game data is never distributed; users supply their own legally owned copy.
The detailed working record (hashes, dates, how each item was used) is [docs/CREDITS_AND_SOURCES.md](docs/CREDITS_AND_SOURCES.md);
licence texts are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and `xboxrecomp/LICENSES/`.

---

## 1. Code and assets used in this project

Everything here is included in this repository, linked into the executable, or shipped in the release package.

| Resource | Source | Version / commit | Licence | Exactly where it is used |
|---|---|---|---|---|
| xboxrecomp (sp00nznet) | https://github.com/sp00nznet/xboxrecomp | v0.14.0 `eb0aa1d` (earlier base `0655e8e`) | MIT; LGPL-2.1+ for the APU code and NV2A register header taken from xemu | `xboxrecomp/` (modified): the recompiler/generator (`xboxrecomp/tools/recomp`), the kernel, video, audio, input and USB runtime libraries (`xboxrecomp/src/*`) |
| Zippingrocks xboxrecomp fork, branch `godzilla-save-the-earth-runtime` | https://github.com/Zippingrocks/xboxrecomp | `7e93e41` | MIT / LGPL-2.1+ / GPL-2.0+ per its own NOTICE | merged into `xboxrecomp/src/{nv2a,d3d,apu}`: the NV2A Kelvin to Direct3D 11 renderer (`nv2a_kelvin.c/.h`, `nv2a_pgraph_d3d11.c`), Direct3D 8 device/shader/combiner changes, the DSP interpreter |
| xemu code (espes, Jannik Vogel, Matt Borgerson and contributors) | https://github.com/xemu-project/xemu | as carried by the two rows above | GPL-2.0 / LGPL-2.1+ | `xboxrecomp/src/apu/apu_*.c|h`, `fpconv.h`, `xboxrecomp/src/nv2a/nv2a_regs.h`, and (through the fork) the register-combiner port `xboxrecomp/src/d3d/d3d8_nv2a_psh.c|h`. Our Kelvin vertex-program translator (`nv2a_kelvin.c`, `d3d8_kvp.c|h`) is our own code that follows the instruction semantics of xemu's `vsh-prog.c` |
| MCPX DSP56300 interpreter (xemu, from Hatari and ARAnyM) | via the fork | as in `7e93e41` | GPL-2.0-or-later | `xboxrecomp/src/apu/dsp/` |
| stb_image.h (Sean Barrett) | https://github.com/nothings/stb | v2.28 | public domain / MIT | `xboxrecomp/src/nv2a/third_party/stb_image.h`, used by `kelvin_texpack.c` to decode PNG texture packs |
| SDL3 (libsdl-org) | https://github.com/libsdl-org/SDL | 3.4.18 | zlib | not stored here; fetched at build time and linked statically; used by `ste/src/ste_sdlinput.c` (gamepads, keyboard bindings) |
| Kenney Input Prompts | https://kenney.nl/assets/input-prompts | 1.5 (zip SHA-256 `ac2fcf599080b0f3ba2d174c9474db6df1a0e96ff0662580e2da79a122ab78a1`) | CC0 1.0 | not stored here; the button pictures (Xbox Series and PlayStation Series colour buttons) ship in the release package as the optional "button prompts" texture packs |
| AMD FidelityFX Super Resolution 1.0 (`ffx_a.h`, `ffx_fsr1.h`) | https://github.com/GPUOpen-Effects/FidelityFX-FSR | commit `a21ffb8` | MIT | not stored here; the shader sources ship in the launcher package (`fsr1` folder) for the optional scan-out upscaler |
| Microsoft Visual C++ runtime | Microsoft | VS 2022 | Microsoft redistributable terms | `vcruntime140*.dll`, `msvcp140.dll` shipped app-local in binary releases |

Our own code is GPL-2.0-or-later. Because GPL components are linked in, binaries are distributed under the GPL with the
corresponding source in this repository.

---

## 2. Research and inspiration (no code used)

These people and projects helped through research, documentation, tools or ideas. **None of their code or files is included in
this project.** Where a feature of ours was inspired by someone's work, it is our own implementation and says so where it is
described ("Inspired by <name>'s research (our own code; no code from <name> used). Full credits: this file").

| Who / what | What they contributed | Included here? |
|---|---|---|
| **ANILORAK** (Godzilla Ultra, PS2) | research and design of Solo Destruction, the idea of a third monster-select page, and the PS2 online lobby research that informed our online plans; his published modding pack was read locally to understand PS2 file layouts and script natives | No code, no files. Solo Destruction, the third select row and our tools are our own implementations |
| **G:STEM / NastySoft community** (Gojirex, Shame Devs, GR8GODZILLAGOD and the STEM community) | the research and published specification behind the "G:STEM-inspired options" (gameplay rules such as knockback, juggle, health floor, rage meter, player size), the GSTE knowledge database, format knowledge, an HD texture set (never redistributed) | No code, no files. The options are re-implemented by us for the Xbox build |
| **Digitzaki**: GZBuildr and GZ-Model-Editor | the Pipeworks bundle, BSF script and model format knowledge; used offline as a cross-check while we wrote our own readers | No code, no files. Our `pw_bundle.py`, `bsf_edit.py` and recipe tools are written from our own reading of the formats; GZBuildr is never bundled or required |
| **DylanRocket**: "Unlocking Biollante" tutorial and Spigot-engine symbol idea | the observation that Biollante's data is still on the retail disc and how the select screen reaches it; the idea of naming functions by engine pattern | No text or files. Our Biollante recipe is our own implementation |
| **Cxbx-Reloaded** and **XbSymbolDatabase** | kernel export signatures (read as data for an audit) and identification of XDK library functions in the executable | No code or files |
| **xemu** project | hardware behaviour of the Xbox GPU/APU, NV2A method names, the reference for our own vertex-program translation (the xemu *code* that is included is listed in part 1) | see part 1 |
| **Zippingrocks**: Godzilla: Destroy All Monsters Melee recompilation | the approach of a per-frame push-buffer hook | No code; our hook is independently written |
| **PCSX2** wiki and patches, KrystianLesniak's patch list, The Cutting Room Floor | widescreen and culling research for the PS2 game, cut-content notes | No code or files |
| **SuperCheats** | the console cheat gesture and three numeric codes (facts) | No text copied |
| Offline tools: extract-xiso, 7-Zip, maxcso, Dolphin, vgmstream, Ghidra with the ghidra-xbe loader, Pillow, imagehash, numpy, MSVC, CMake, Python | extracting and analysing the user's own discs and files, building, perceptual image matching | None is part of this repository or the release |
| **Claude** (Anthropic), Claude Code | AI-assisted analysis and coding | tooling only |

---

## The game
*Godzilla: Save the Earth* (Xbox, 2004), developed by Pipeworks Software, published by Atari. Godzilla and related characters are
trademarks of Toho Co., Ltd. Not affiliated with or endorsed by Toho, Atari or Pipeworks.
