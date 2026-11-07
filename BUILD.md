# Building ste-recomp.exe

The build has three stages: the runtime libraries (C, CMake), code generation from **your own** `default.xbe`
(Python), and compiling and linking the generated C together with the hand-written sources in `ste/src/`.
`ste\scripts\build.ps1` runs all of it.

## What you need
* Windows 10/11 x64.
* **Visual Studio 2022 Build Tools** with the "Desktop development with C++" workload (MSVC x64, Windows 10/11 SDK).
* **CMake** 3.20 or newer.
* **Python** 3.11 or newer with the packages in `requirements.txt` (`python -m pip install -r requirements.txt`; the generator needs `capstone`).
* **SDL3** 3.4.x source (zlib licence). Either pass `-Sdl3Src <folder>` or let the script `git clone` the tag
  `release-3.4.18` into `deps\SDL` (needs `git` and network access).
* **Your own `default.xbe`** from *Godzilla: Save the Earth* (Xbox), from a folder extracted from your disc
  (for example with [extract-xiso](https://github.com/XboxDev/extract-xiso)). Its SHA-256 should be the one in
  `ste/gen/xbe.sha256`; the script warns if it differs. Other builds of the game are not supported.
* About 3 GB of free disk space and 10 to 15 minutes (the generated C is roughly 100 MB).

## Build
In a normal PowerShell window (the script finds Visual Studio through `vswhere` and starts its own developer environment):

```
git clone <this repository> ste-recomp
cd ste-recomp
.\ste\scripts\build.ps1 -Xbe "D:\path\to\extracted\default.xbe"
```
Useful options: `-Out <dir>` (default `build`; the script refuses to overwrite an existing `gen` or `ste-recomp.exe`),
`-Sdl3Src <dir>`, `-Version 0.1.0`, `-BuildNumber 260`, `-Python <python.exe>`, `-Vcvars <vcvars64.bat>`,
`-SkipGenerate` (reuse `<Out>\gen` after a failed compile).

Steps and where they write (all under `build\`):
1. `runtime\`: `cmake` builds the static libraries `xbox_kernel`, `xbox_d3d8`, `xbox_nv2a`, `xbox_apu`, `xbox_dsound`, `xbox_usb`,
   `xbox_video`, `xbox_input`, `recomp_exit_hook` (Release, MSVC).
2. `sdl3\`: SDL3 as a static library (gamepad input only; audio, render, GPU and video back ends are off).
3. `gen\generated\`: **generated C** (`recomp_0000.c` to `recomp_0016.c`, `recomp_dispatch.c`, `recomp_funcs.h`, ...).
   `ste\scripts\generate.py` converts `ste/gen/functions.tsv`, runs the generator
   (`xboxrecomp/tools/recomp`, with `ste/gen/ste_hooks.toml` and `ste/gen/exclude_manual.c`) on your XBE, makes unresolved-call
   stubs fatal (`[STUB HIT]`, exit code 4) and adds one missing declaration. **This directory is derived from the game: keep it out
   of version control and never publish it.**
4. `ste\scripts\verify_generated.py` runs the regression checks on the generated code (`PASS` lines; any `FAIL` stops the build).
5. `obj\`: the generated chunks are compiled with `/O2`, the hand-written files with `/Od` (the flags of the published binaries),
   then everything is linked to `build\ste-recomp.exe` together with the libraries above.

## Check that it worked
```
build\ste-recomp.exe --version          # ste-recomp 0.1.0 (STE_CANDIDATE_ID=260;)
build\ste-recomp.exe --game "D:\path\to\extracted"
.\ste\tests\smoke.ps1 -Exe build\ste-recomp.exe -Game "D:\path\to\extracted" -Seconds 60   # PASS = stayed alive, no [CRASH] / [ICALL FAIL] / [STUB HIT]
```
The window opens and the game boots. Settings come from the optional ini files; see `docs/LAUNCHER_INTERFACE.md`
(command line, environment variables, exit codes, log lines). Useful when something goes wrong:
`STE_NO_DIALOG=1` (errors to stderr only), `XBOX_LOG_FILE=<file>` (kernel layer log), `STE_DUMP_DIR=<dir>` (crash dumps).

### Reproducibility
The generator is deterministic: the same XBE and the same inputs give the same C, byte for byte.
`ste/gen/expected_generated.sha256` lists the SHA-256 of every generated file for the supported XBE;
`ste\scripts\build.ps1` compares them after generation and prints a warning for any difference.
The published binaries were built with `-BuildNumber 260` (0.1.2d); a different compiler version gives different machine
code, so the executable itself is not expected to be bit-identical.

## Layout of the inputs (all hand-written, no game data)
| File | Role |
|---|---|
| `ste/gen/functions.tsv` | address ranges of the functions found by analysing the XBE (start, end, section, how it was found) |
| `ste/gen/ste_hooks.toml` | instruction hooks: call a function of `recomp_manual.c` before, after or instead of one instruction (`docs/formats/RECOMP_HOOKS.md`) |
| `ste/gen/exclude_manual.c` | the XDK USB functions that `recomp_manual.c` replaces; the generator skips them |
| `ste/src/*.c` | the executable (see README) |

## Running the tests of the generator
`cd xboxrecomp && python -m pip install pytest && python -m pytest tools/recomp -q`. The C test programs and `d3d8_smoke` under `xboxrecomp/` come from upstream, are not part of the build above and are not maintained for this tree (the all-targets CMake build does not link; `build.ps1` builds only the libraries).