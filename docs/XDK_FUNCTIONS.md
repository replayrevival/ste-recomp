# What is recompiled, what is replaced

The game (an Xbox title built with the Microsoft XDK, version 5849 libraries) is **statically linked**: the XDK libraries
(Direct3D 8, DirectSound, XAPI, ...) are part of `default.xbe`. The recompiler translates *all* of that code, game and XDK
alike, to C. Almost nothing is replaced at function level; the replacement happens one layer down, where the XDK code
talks to the console hardware and to the Xbox kernel.

## 1. Recompiled unchanged
Identified with [XbSymbolDatabase](https://github.com/Cxbx-Reloaded/XbSymbolDatabase) (database e26a3039), run on the supported
`default.xbe`:

| XDK library | identified functions | sections of the XBE |
|---|---:|---|
| D3D8 (Direct3D 8) | 163 | `.text`, `D3D` |
| DSOUND (DirectSound) | 112 | `.text`, `DSOUND`, `.rdata` |
| XAPILIB (XAPI, XPP/USB, threads, files) | 49 | `.text`, `XPP` |
| XONLINES (XOnline stubs) | 21 | `.text`, `XONLINE`, `XNET` |
| XGRAPHC (graphics helpers) | 6 | `.text`, `XGRPH` |

All of these run as recompiled C. Direct3D 8 is the important one: the game's own `D3DDevice_*` code builds NV2A push buffers
exactly as on the console; `xbox_nv2a` / the "Kelvin" renderer consumes those push buffers and draws with Direct3D 11.
DirectSound likewise programs the MCPX audio hardware, emulated by `xbox_apu` (voice processor and the DSP56300
interpreter). The XMV movie decoder of the XDK also runs recompiled. The rest of the 16159 functions in `ste/gen/functions.tsv`
is the title's own code and the C runtime.

## 2. Replaced by hand (`ste/src/recomp_manual.c`, `ste/gen/exclude_manual.c`)
The XDK USB stack (XPP) drives the OHCI host controller registers directly, which have no host equivalent here, so its public
entry points are replaced by functions that answer from the host's gamepads and keyboard (XInput / SDL3):

| address | XDK function | replacement |
|---|---|---|
| `0x002D803E` | `XInitDevices` | no-op, reports success |
| `0x002D8E87` | `XGetDevices` | bit mask of the connected virtual pads |
| `0x002D8EA9` | `XGetDeviceChanges` | insertions / removals of pads |
| `0x002D8B44` | `XInputOpen` | per-port handle |
| `0x002D8B9A` | `XInputClose` | releases the handle |
| `0x002D8BA6` | `XInputGetCapabilities` | gamepad capabilities |
| `0x002D8D7E` | `XInputGetState` | host pad / keyboard state, remapped, dead zones |
| `0x002D8DF1` | `XInputSetState` | rumble |
| `0x002D990F` | `XGetDeviceEnumerationStatus` | always idle |
| `0x002D7C84` | `MU_Init` | reports no memory units |

Two title functions are **wrapped** (the original body still runs): `sub_0001F740` (camera projection set-up; the wrapper applies the
widescreen aspect correction and then calls the original as `ste_orig_sub_0001F740`) and `sub_00015512` (a start-up trace, only
active with `STE_STARTUP_TRACE`).

## 3. Replaced below the library level (runtime libraries, `xboxrecomp/src`)
* **xboxkrnl**: the 142 kernel imports this title uses are implemented on top of Win32 (`xbox_kernel`: memory map, files and
  paths, threads and synchronisation, time, ...). The game's save/data volumes map to host folders; nothing under the
  game folder is ever written.
* **NV2A GPU** (`xbox_nv2a`, `xbox_d3d8`): push-buffer replay to Direct3D 11, shader generation from NV2A vertex programs and
  register combiners, scan-out and presentation (flip model, V-Sync / VRR handling, scaling).
* **MCPX APU / DSP** (`xbox_apu`, `xbox_dsound`): voice processor and DSP interpreter, mixed to the host audio device.
* **USB / OHCI** (`xbox_usb`): the host-controller registers the XDK USB code expects (the XPP entry points of section 2 sit above it).
* `xbox_input` and `xbox_video` are linked but not used by this title (input goes through section 2; movies through the game's own XMV code).

## 4. Instruction hooks (`ste/gen/ste_hooks.toml`)
The generator can call a function of `recomp_manual.c` before, after or instead of a single instruction
(`docs/formats/RECOMP_HOOKS.md`). They sit inside **title** functions (not XDK code) and implement optional gameplay and
camera features, all **off by default** (the original game is the default); each corresponds to a key in `ste_enhance.ini`
or `ste_video.ini`:

| addresses | feature |
|---|---|
| `0x0010D36A` `0x0010D380` `0x0010D381` | no knockback scaling by monster size (and a logging probe) |
| `0x0011ED54` | no juggle limit |
| `0x00112AC0` `0x00112B0B` `0x00112B33` `0x00112B44` | health floor: logging probes only |
| `0x000E0F8D` `0x000E0F99` | no dimming of the results screen |
| `0x000EA9AB` `0x000EA9CA` | more AI vehicles |
| `0x0016D79D` | weapon hits always push vertically |
| `0x00121B4B` | player size |
| `0x0011AE6D` `0x0011AE7A` `0x0011AE98` | footstep camera shake |
| `0x001119D4` `0x0010228C` | rage meter on the HUD |
| `0x0013BF08` `0x0013C118` `0x0013C347` `0x0017CE4C` `0x0017D27A` | draw distance (`original` | N | `unlimited`) |

The gameplay options are modelled on the community STEM patch set for the PS2 version; STEM is only the specification (see `CREDITS.md`).