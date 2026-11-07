# Third-party notices

This repository combines the project's own code (GPL-2.0-or-later) with components from other projects, each under its
own licence. The authoritative per-file attribution is in `xboxrecomp/NOTICE` and the SPDX / copyright header of each file;
licence texts are in `xboxrecomp/LICENSES/` and below. A binary built from this tree links all of the components listed here,
so the combined executable is distributed under the GPL (version 2 or, at your option, any later version) with the corresponding
source in this repository.

| Component | Where | Licence | Notes |
|---|---|---|---|
| xboxrecomp (sp00nznet) v0.14.0, commit `eb0aa1d` | `xboxrecomp/` (modified) | MIT | runtime libraries, generator, tests; copyright (c) 2026 sp00nz and contributors (`xboxrecomp/CONTRIBUTORS.md`); text below |
| xemu MCPX APU code (espes, Jannik Vogel, Matt Borgerson) | `xboxrecomp/src/apu/apu_*.c|h`, `fpconv.h`, `src/nv2a/nv2a_regs.h` | LGPL-2.1-or-later | listed file by file in `xboxrecomp/NOTICE`; text `xboxrecomp/LICENSES/LGPL-2.1.txt` |
| MCPX DSP56300 interpreter (xemu, from Hatari / ARAnyM) | `xboxrecomp/src/apu/dsp/` | GPL-2.0-or-later | files keep their notices; text `xboxrecomp/LICENSES/GPL-2.0.txt` |
| NV2A register-combiner port of xemu's generator | `xboxrecomp/src/d3d/d3d8_nv2a_psh.c|h` | GPL-2.0-or-later | as above |
| Zippingrocks/xboxrecomp branch `godzilla-save-the-earth-runtime` (`7e93e41`) | merged into `xboxrecomp/src/{nv2a,d3d,apu}` | MIT / LGPL-2.1+ / GPL-2.0+ per its own NOTICE | NV2A Kelvin to Direct3D 11 renderer and related changes |
| stb_image 2.28 | `xboxrecomp/src/nv2a/third_party/stb_image.h` | public domain or MIT (your choice, see the header) | PNG decoding for optional texture packs |
| SDL3 3.4.x (libsdl-org/SDL) | not included; fetched or supplied at build time, linked statically | zlib | gamepad input only; text below |
| AMD FidelityFX Super Resolution 1.0 (`ffx_a.h`, `ffx_fsr1.h`) | not included; the optional scan-out upscaler loads the shader sources from an `fsr1` folder shipped with the launcher package | MIT | (c) 2021 Advanced Micro Devices, Inc. |
| Microsoft Visual C++ runtime | not included; binary releases ship `vcruntime140*.dll` and `msvcp140.dll` app-local | Microsoft redistributable terms | |

Algorithms and hardware behaviour credited but independently implemented (Z-order swizzle, register-combiner translation) are
described in `xboxrecomp/NOTICE`.

## MIT licence of xboxrecomp

MIT License

Copyright (c) 2026 sp00nz

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## SDL3 (zlib licence)

Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>
  
This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:
  
1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required. 
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.