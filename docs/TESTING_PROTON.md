<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Proton / Wine check (phase 0 of goal 7)

Purpose: find out how far the **current Windows build** runs under Wine/Proton (DXVK for D3D11, Wine's XAudio2) on a Linux machine, before any porting. No code change is expected; the result and logs decide how much of Windows we lean on. See `docs/PORTABILITY_AUDIT.md`. Not affiliated with or endorsed by Toho, Atari or Pipeworks. The tester must own the game and supplies their own copy; nothing of the game is in the test package.

## What the tester needs
1. **The Windows build** (a release zip, e.g. `ReplayRevival-STE-v0.1.2d-alpha-win64.zip`, or a test zip). Files that matter: `ste-recomp.exe`, `vcruntime140.dll`, `fsr1\` (3 files; FSR shaders are compiled at run time), optionally `launcher.exe` (try it second, see below). Unzip to a normal folder on a Linux filesystem.
2. **Their own game folder**: an extracted dump that contains `default.xbe` (SHA-256 of the supported XBE: `6966cc8a3a22d420d05362f24f3e68d7a462c481861189855f05583c0d6de500`). The game does not read ISO files yet (exit code 3).
3. **Wine or Proton**, 64-bit prefix, Windows 10 mode:
   - Steam Deck / Steam: Proton 9 or newer (Proton-GE is fine), run through `protontricks-launch` or add `ste-recomp.exe` as a non-Steam game.
   - Plain Wine: Wine 9+ with DXVK 2.x installed into the prefix (`winetricks dxvk`), `wine --version` recorded.
   - A Vulkan-capable GPU/driver (Mesa 24+ or recent NVIDIA). Record `vulkaninfo --summary`.
4. About **2 GB free RAM** beyond the game (it maps 64 MB of guest RAM plus renderer buffers) and a few GB of disk for logs/screenshots.

## Run (command line first, with logs)
Always start the game exe **directly**, not through the launcher, so the log is complete. The Windows path of a Linux folder is `Z:\home\<user>\...`.

```sh
export WINEPREFIX=$HOME/.wine-ste        # or the Proton prefix
cd /path/to/release-folder
export STE_GAME_PATH='Z:\home\USER\games\godzilla-ste'     # folder that contains default.xbe
export STE_NO_DIALOG=1                  # startup errors go to stderr, no message box
export RECOMP_AC97_READY=1 RECOMP_VBLANK=1 RECOMP_KERNEL_LOG_BUDGET=0 STE_SHOT_SEC=0
export RECOMP_SAVE_DIR='Z:\tmp\ste-saves'                   # keeps saves out of the game folder
export STE_VIDEO_WIDTH=1280 STE_VIDEO_HEIGHT=720 STE_VIDEO_ASPECT=native
export STE_VIDEO_VSYNC=1 STE_DUMP_DIR='Z:\tmp\ste-dumps'
# optional DXVK/Wine diagnostics:
export DXVK_LOG_LEVEL=info DXVK_LOG_PATH=/tmp/ste-dxvk WINEDEBUG=-all
wine ste-recomp.exe 2> /tmp/ste-stderr.log ; echo "exit=$?"
```
Under Proton: `STEAM_COMPAT_DATA_PATH=... STEAM_COMPAT_CLIENT_INSTALL_PATH=... proton run ste-recomp.exe` with the same variables.

Variables that change behaviour (all optional): `STE_VIDEO_VRR=off` (turns the tearing/flip-model swap chain off, try this if the window is black or the present fails), `STE_VIDEO_CLAMP=0`, `STE_VIDEO_MODE=window|borderless|fullscreen` (use `window` under Wine), `XBOX_LOG_FILE=/tmp/xbox_kernel.log` (full kernel log, large), `STE_SHOT_SEC=30` plus `STE_SHOT_DIR=Z:\tmp\ste-shots` for periodic screenshots (BMP of the render target), `STE_SWAPCHAIN_DUMP=Z:\tmp\ste-sc` for what the swap chain really shows, `STE_CAPTURE_RING=1` (frame ring, F12 saves; needs a test build that has it).

### Optional scripted fight (no controller needed)
Create `pad.txt` (seconds from start : button : hold) and set `STE_VIRTUAL_PAD=1 STE_PAD_SCRIPT_FILE='Z:\tmp\pad.txt' RECOMP_ASYNC_UAF=1 RECOMP_SAVE_SEED=1`. This is the soak script of a Tokyo Versus fight with the first monster; the fight starts after about 340 s and runs on its own:
```
120:S
138:D:0.3
156:A
171:A
186:A
232:A
247:S
262:A
277:A
325:A
340:A
355:A
```
Run for 420 s, then stop the game (`pkill -f ste-recomp.exe` or close the window).

## Expected log lines (stderr)
Success path, in this order (values differ per machine):
```
[VIDEO] DPI awareness: per-monitor
[CONFIG] game=Z:\home\USER\games\godzilla-ste (env STE_GAME_PATH)
xbox_MemoryLayoutInit: mapped 65536 KB at 0x0000000000010000 (offset +65536 from Xbox base)
[XA2] XAudio2 initialized (48000 Hz stereo 16-bit, ...)
[APU] Using XAudio2 audio backend ...
[SESSION] desktop=... remote_session=0 monitors=N
[VIDEO] ini=(none) scale=... out=WxH render=WxH mode=0 aspect=2 ...
[VIDEO] present: flip_model=... vsync=on ... swap_effect=...
[PB-ATTACH] tid=... dev=... ring 80002000..80202000 ...
[SPIKE] frame=... draws=...            (once the game renders)
```
After about 60-100 s the game reaches the Pipeworks logo and menus; kernel frame counts in `[SPIKE] frame=N` keep rising. The process stays alive until closed (exit code 0 on a normal close).

## Failure signatures and what they mean
| Log line or exit code | Meaning |
|---|---|
| exit 2 and `no game folder given: use --game ...` / exit 3 (unusable path, or `... is a disc image. Mounting XISO images is not implemented yet`) | `STE_GAME_PATH` wrong (use the `Z:\` form) or an ISO was given |
| `Failed to initialize Xbox memory layout` (exit 1) | The 4 GB guest address window / fixed base 0x10000 is not available under this Wine. **Most important thing to report** (also `xbox_MemoryLayoutInit` lines above it) |
| `[PB-ATTACH] D3D8 CreateDevice failed 0x...` or no `[PB-ATTACH]` line | D3D11 device/swap chain creation failed under DXVK (report the HRESULT, DXVK log, `vulkaninfo`) |
| `[VIDEO] Present(sync=..., flags=...) failed: 0x...` followed by `[VIDEO] present failed: switching to the safe path` / window black but `[SPIKE]` frames rise | Flip-model/tearing present problem: retry with `STE_VIDEO_VRR=off`, `STE_VIDEO_VSYNC=0/1` |
| `[XA2] XAudio2Create failed` | Wine's XAudio2 missing: `winetricks xact` / `xaudio29`; the game continues without sound? (report what happens) |
| `[CRASH]` line, exit code `0xC0000005` etc., a `.dmp` in `STE_DUMP_DIR` | Crash (the exit code is the Windows exception code); attach the dump and the 40 lines before it |
| `[ICALL FAIL]`, `[STUB HIT]` (exit 6 / 4) | A recompiled indirect call or stub was reached; it means a code path never seen on Windows: attach the log |
| `[WATCHDOG]` | Game thread stopped advancing for 120 s |
| `DEVICE_REMOVED`, exit 10 | GPU device lost under DXVK |
| `[KVP]`/`[PSH]` shader compile failure lines, or heavily broken/black 3D but UI visible | HLSL compile problem under DXVK (D3DCompile comes from Wine's `d3dcompiler_47`; run `winetricks d3dcompiler_47` if missing) |

A shader cache is created next to the exe (`shadercache\`); the first run compiles many shaders and has long frame spikes (several seconds in total), which is normal.

## What to send back
1. `/tmp/ste-stderr.log` (the whole file), the exit code, the DXVK log if any, and any `.dmp`.
2. Wine/Proton version, DXVK version, kernel, distro, GPU and driver (`vulkaninfo --summary`), desktop session (X11/Wayland), display resolution/refresh.
3. A screenshot of the title/menu and, if reached, a fight (BMP from `STE_SHOT_DIR` or a normal screenshot).
4. Answers to: Does the logo movie play and have sound? Does it reach the main menu? Does a pad work (and which)? Does the fight run at a steady frame rate (note the `[RFT]` lines every 10 s: `p50/p99` frame times)? Any visual breakage that the Windows build does not show.
5. Optional second test: `launcher.exe` under the same prefix (the launcher is SDL3 + OpenGL 3.3 and should run natively under Wine; report whether it opens, finds the game folder, and starts the game).

Send results to Miko; do not post logs that contain your home path publicly without checking them.
