<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Launcher interface (Godzilla: Save the Earth recomp)

Contract between the game executable and the launcher. Keep it current; the launcher reads it.
Last updated 2026-11-06 for build c260 (version 0.1.0). Items marked **planned** are not in the exe yet.
This closes the mismatches M1-M6 listed in the launcher's interface-assumptions document (see the last section).

## 1. Executable, version, working directory
| | |
|---|---|
| Stable name | `ste-recomp.exe` (c247 and later release builds; copied from the promoted candidate, same bytes). Development builds keep `ste_recomp_mission_l6_candidate<N>.exe`. |
| Folder | the folder of `ste-recomp.exe`; in a release, the exe folder holds `fsr1\`, `texturepacks\`, `mods\`, `saves\` (see section 6). |
| Working directory | the exe's folder. Every relative lookup (`fsr1`, `texturepacks`, `mods`, `saves`, `ste_video.ini`, `ste_enhance.ini` defaults) is relative to the **exe**, not the working directory. |
| Version | Windows VERSIONINFO resource: `FileVersion` = `x.y.z.<build>` (c247 = `0.1.0.247`), `ProductVersion` = `x.y.z+c<build>` (`0.1.0+c247`), `InternalName` = `ste-recomp`, `OriginalFilename` = `ste-recomp.exe`. Builds before c246 have no version resource. |
| `--version` | prints `ste-recomp 0.1.0 (STE_CANDIDATE_ID=247;)` on stdout and exits 0 before anything else starts. |
| `--help`, `-h` | prints usage and the exit codes, exits 0. |
| Subsystem | console exe. stdout and stderr are unbuffered; the caller redirects them. |

## 2. Command line
| Argument | Meaning |
|---|---|
| `--game <path>` or `--game=<path>` | the user's game: a folder that contains `default.xbe`. Wins over `STE_GAME_PATH`. |
| `--version`, `--help` | see above. |

Unknown arguments are ignored (not an error).

## 3. Game path
Order: `--game`, then env `STE_GAME_PATH`, then a `default.xbe` next to the exe. There is no built-in development path any more (c258): with none of the three, the game exits with code 2 and "no game folder given".

* The path is made absolute; trailing separators are ignored; paths with spaces and non-ASCII characters are supported (wide-character API, UTF-8 internally).
* The folder must contain `default.xbe`. Its SHA-256 is the launcher's job (STE retail: `6966cc8a3a22d420d05362f24f3e68d7a462c481861189855f05583c0d6de500`); the game does not verify it.
* The game never writes under the game folder: the file layer's write guard (`[GAME-OWNED-WRITE-BLOCKED]` in the log) is pointed at it automatically (env `RECOMP_GAME_OWNED_ROOT` overrides; `-` disables).
* **Disc images (.iso / .xiso): recognised, not mounted yet.** The game prints `[CONFIG] <file> is a disc image. Mounting XISO images is not implemented yet ...` and exits 3. Read-only XISO mounting in the file layer is the next step; when it exists, `STE_GAME_PATH=<.iso>` will work unchanged and this section will say so.
* Log line on success: `[CONFIG] game=<folder> (--game | env STE_GAME_PATH | exe folder)`.

## 4. Environment variables
Settings in the ini files are the normal route; environment variables override them.

| Variable | Meaning |
|---|---|
| `STE_GAME_PATH` | game folder (section 3). |
| `STE_VIDEO_INI` | absolute path of the video ini. Else `ste_video.ini` next to the exe (optional; absent = built-in defaults). Logged as `[VIDEO] ini=`. |
| `STE_ENHANCE_INI` | absolute path of the enhancement/cheat/input-port ini. Else `ste_enhance.ini` next to the exe. Honoured by every reader (since c243, including `[saves]`). Logged once as `[CONFIG] ste_enhance.ini=<path> (env STE_ENHANCE_INI \| exe dir) exists=0\|1`. |
| `STE_INPUT_MAP` | absolute path of the launcher's `input_mapping.json` (format 1, `launcher\docs\INPUT_MAPPING.md`); read once at start; missing/damaged = built-in defaults. See section 8. |
| `STE_INPUT_BACKEND` | `sdl` \| `xinput` \| `auto` (default): overrides `[input] backend`. `auto` = SDL3 gamepads, XInput if SDL cannot start. |
| `STE_NO_DIALOG` | `1`: startup errors go to stderr and the exit code only; no message box. The launcher should set it. |
| `STE_VIDEO_<KEY>` | overrides the `[video]` key of that name (verified names: `STE_VIDEO_SCALE`, `_WIDTH`, `_HEIGHT`, `_MODE`, `_ASPECT`, `_DRAW_DISTANCE`, `_FOV_SCALE`, `_FOV_MIN_ZOOM`, `_QUERY_SCALE`, `_CULL_MARGIN`, `_ANISOTROPY`, `_MIP_BIAS`, `_PILLAR_CAMFIX`; other keys: see the .sample). |
| `STE_ENHANCE_<KEY>` | overrides the `[enhance]` key of that name (`STE_ENHANCE_UNLOCK_ALL_MONSTERS`, ...). `STE_INPUT_PORT1..4` override `[input] port1..4`. |
| `STE_VIDEO_CLAMP` | `0`: keep a requested window size larger than the usable desktop (default: fitted to the desktop). |
| `STE_KELVIN_CULL` | NV2A face culling: `2` (default) everywhere; the Low preset also sets it explicitly. |
| `STE_TEXPACKS` | `0`: do not scan or load texture packs (Low preset). `STE_TEXPACKS_ENABLE=<id,id>` enables only the listed packs; `STE_TEXTUREPACKS=<dir>` sets the pack folder. |
| `STE_MODS`, `STE_MODS_DIR`, `STE_MODS_ENABLE` | mods: `0` disables; folder (default `mods\` next to the exe); comma-separated ids enabled exclusively. Format: `docs\formats\mod_manifest.md`. |
| `RECOMP_SAVE_DIR` | root of the title's saves (default `<exe folder>\saves`). |
| baseline set by every launcher script | `RECOMP_AC97_READY=1`, `RECOMP_VBLANK=1`, `RECOMP_KERNEL_LOG_BUDGET=0`, `STE_SHOT_SEC=0`, `STE_PB_LOG_EVERY=3000`, `RECOMP_FB_WINDOW=` and `RECOMP_APU_DSP_ACK=` (both empty). |

Test-only (do not set in a launcher): `STE_PAD_SCRIPT`, `STE_PAD_SCRIPT_FILE`, `STE_VIRTUAL_PAD(S)`, `STE_POPLOG*`, `STE_DD_*`, `STE_DEBUG_FREE_CAM`, `RECOMP_PB_EXEC` (blocked).

## 5. Ini files
Writer rules for the launcher: preserve comments and unknown keys, change only managed keys, atomic write. The commented references are `artifacts\ste_video.ini.sample` and `artifacts\ste_enhance.ini.sample`.

### 5.1 `ste_video.ini` `[video]` (defaults = original game)
| Key | Type / allowed | Default | Notes |
|---|---|---|---|
| `width`, `height` | int px | 640*scale x 480*scale | window size; empty = derived from `scale`; borderless/fullscreen use the monitor. |
| `scale` | float | height/480 | optional. |
| `mode` | `window` \| `borderless` \| `fullscreen` | `window` | `fullscreen` is exclusive fullscreen at exactly `width` x `height`; if the monitor does not report that mode, or Windows refuses fullscreen, the game logs `[VIDEO] fullscreen mode WxH not supported, using borderless` (or `fullscreen WxH failed (...), using borderless`) and carries on as a borderless window of that size, centred on the monitor. It never exits for this reason. |
| `display` | int, 0-based | not set (primary monitor) | monitor to open on, in SDL3 `SDL_GetDisplays()` order (what the launcher writes). Out of range -> `[VIDEO] display=N is not available (K displays found): using the primary monitor`. Window mode opens 100 px in from the monitor's corner, borderless / fullscreen are centred on it. Env `STE_VIDEO_DISPLAY`. The game is per-monitor DPI aware (sizes are real pixels). |
| `aspect` | `original` \| `native` | `original` | `original` = 4:3 picture with bars (pillarbox) in a wider window. `native` = fill the window: the 3D view widens (Hor+, same vertical view) and the HUD stays a centred, original-size 4:3 block. Hidden aliases, read as `native`: `wide`, `stretch`. `4:3-pillar` / `pillar` = `original`. Anything else is logged and read as `original`. |
| `menu_aspect` | `pillar` \| `stretch` \| `wide` | `pillar` | hidden debug key. In `native` the menus are always the 4:3 block with the wider 3D background; the launcher writes `pillar`. |
| `fps_cap` | `60` \| `30` \| `0` | `60` | game speed stays correct at 30 and 60. |
| `vsync` | `on` \| `off` | `on` | `on` = `Present(SyncInterval 1)`, never the tearing flag (no tearing; variable-refresh displays still sync inside the driver's range). `off` = lowest latency, may tear. A missing key means `on`. |
| `vrr` | `auto` \| `off` | `auto` | `auto` = flip-model swap chain created with tearing support when the system reports `DXGI_FEATURE_PRESENT_ALLOW_TEARING` (windowed / borderless only). `DXGI_PRESENT_ALLOW_TEARING` is used only when `vsync=off` AND tearing is supported AND the mode is not fullscreen. `off` = a plain swap chain at a fixed 60 Hz clock. See section 7b. |
| `render_ahead` | `1` \| `2` | `1` | |
| `render_scale` | 0.25..1.0 | off | internal height as a fraction of the window height. |
| `upscale` | `bilinear` \| `fsr1` | `fsr1` when `render_scale < 1` | needs `fsr1\` next to the exe. |
| `fsr_sharpness` | float (stops) | `0.25` | 0 = sharpest. |
| `draw_distance` | `original` \| number 0.25..16 \| `unlimited` | `original` (= `1`) | number N: far plane x N and the per-object cull range x N (radial distance) and small-object threshold / N; `unlimited`: far plane x4 and the per-object culls bypassed (draw only; update/AI/physics unchanged). |
| `fog_scale` | 0.25..16 | `1` | |
| `query_scale` | 1..6 | `1.5` | only with `draw_distance=unlimited`: widens the per-frame scene-object query volume (the far slab bound and its box, restored right after each query) by this factor, so scenery stays in the draw list past the game's own ~1250-1360 unit limit (the Tokyo skyline pop-in). `1` = the game's own volume. Draw list only; update/AI/physics unchanged. Cost grows with the factor (dev PC, Tokyo fight, 1765x993: 1.5 about -3 fps, 2 about -8, 4 about -30), see `docs/community/Q1_CAMERA_DRAW_DISTANCE.md`. |
| `fov_scale` | 0.5..2 | `1` | world cameras only. |
| `fov_min_zoom` | radians, `0` = off | `0` | `0.7854` = the camera's default FOV (no zoom-in). |
| `cull_margin` | 0.25..16 | `1` | widens the culling frustum. |
| `anisotropy` | `0` \| `2` \| `4` \| `8` \| `16` | `0` | world textures. |
| `mip_bias` | -2..2 | `0` | |
| `render_threads` | int, `0` = auto (2) | `0` | |
| `pillar_camfix` | `0` \| `1` | `1` | |
| `texturepacks` | folder | `texturepacks\` next to the exe | |
| `texture_dump` | folder | off | large; debugging. |
`[input] keyboard` = `1` (default) \| `0`; env `RECOMP_KEYBOARD`.

### 5.2 `ste_enhance.ini` (all `0` = original STE)
`[enhance]` (each also as env `STE_ENHANCE_<KEY>`):
`unlock_all_monsters`, `unlock_all_cities` (changes the city order), `unlock_all_challenges`, `unlock_gallery`, `unlock_final_wars`, `unlock_biollante` (needs the biollante content pack) - 0/1.
`cheat_invulnerable_p1`, `cheat_invulnerable_com`, `cheat_super_damage_com`, `cheat_red_p1`, `cheat_red_com` - 0/1.
`stem_no_juggle_limit`, `stem_no_knockback_scaling`, `stem_no_min_health`, `stem_no_results_dim`, `stem_more_ai_vehicles`, `stem_weapon_vertical_knockback`, `stem_footstep_camera_shake`, `stem_rule_empty_arena` - 0/1.
`stem_rule_player_size` - 0 (off) or 1..1000 (%), `stem_rule_player_size_slots` - bit mask P1..P4 (default 15).
`stem_rule_rage_meter` - 0..5 (0 off, 1 damage received, 2 dealt, 3 both, 4 over time, 5 all).
`[saves] seed_from_game_dir` - 0/1 (default 0: a new save folder starts empty). Env `RECOMP_SAVE_SEED`.
`[input] port1..port4` - `auto` \| `xinput0`..`xinput3` \| `keyboard` \| `none` (default `auto`); env `STE_INPUT_PORT1..4`. With the SDL3 backend `xinputN` means "the N-th connected pad" and a mapping file's per-port `device` wins over these keys.
`[input] backend` - `auto` \| `sdl` \| `xinput` (default `auto`). `overlay_key` - key name that opens the overlay (default `F1`, `none` = off). `overlay_guide` - 0/1 (default 1): Guide/Home/PS button. `overlay_chord` - 0/1 (default 1): hold Back+Start. `overlay_chord_ms` - hold time (default 1000).

## 6. Presets
| Preset | Files | Notes |
|---|---|---|
| Original | none (no ini, or empty ones) | built-in defaults: the original game; all enhancements off. |
| Max | `artifacts\max\ste_video.ini`, `ste_enhance.ini` | 1080p window, wide, `draw_distance=unlimited`, `fog_scale=4`, `anisotropy=16`, `fov_min_zoom=0.7854`; unlocks on. |
| Low | `artifacts\low\ste_video.ini`, `ste_enhance.ini` | 1080p window, `render_scale=0.667` + FSR 1, `draw_distance=2`, `render_threads=2`; launcher also sets `STE_KELVIN_CULL=2` and `STE_TEXPACKS=0`. |

Folders next to the exe: `fsr1\` (FSR shaders), `texturepacks\` (packs, all disabled by default), `mods\`, `saves\`, `shadercache\`.

## 7. Exit codes and logs
| Code | Meaning |
|---|---|
| 0 | normal exit (window closed), or `--version` / `--help`. |
| 1 | startup failure after the path check (XBE load, memory layout). |
| 2 | bad command line (`--game` without a path, empty path). |
| 3 | game path unusable: not found, not a folder, no `default.xbe`, or a disc image (not mountable yet). |
| 6 | internal error in the recompiled code (`[ICALL FAIL]` in the log: an indirect call to an unknown address). Not caused by the user's settings. |
| 10 | the graphics device was removed or reset (driver crash / reset, GPU unplugged, `DXGI_ERROR_DEVICE_REMOVED` or `_RESET`). Log line: `[VIDEO] graphics device lost (Present 0x..., reason 0x...): exiting with code 10`. |
| `0xC0000000` range (e.g. `0xC0000005` access violation, `0xC0000094` integer divide by zero, `0xC00000FD` stack overflow) | the game crashed. The exit code is the Windows exception code. |

**Crash trace and dump (c256).** On an unhandled exception the game writes to stderr
`[CRASH] code=0x... addr=0x... <module>+0x<offset> thread=N access=read|write (name)`, then `[CRASHDUMP] exception ... -> <file>.dmp`, `[CRASHDUMP] written`, and
`[CRASH] exiting with code 0x...`; then it ends the process with the exception code as exit code (no Windows error-report dialog). `<module>+0x<offset>` is the faulting address relative to its module (`ste-recomp.exe+0x...` for recompiled game code). The minidump
(data segments + referenced memory, a few MB; `STE_DUMP_FULL=1` = full memory) goes to, in this order: `STE_DUMP_DIR` (**the launcher should set it to `<data>\logs\dumps`**) -> `<data>\logs\dumps` derived from `STE_VIDEO_INI` (`<data>\<settings folder>\ste_video.ini`, when `<data>\logs` exists) -> `<exe dir>\data\logs\dumps` (portable layout) -> `..\logs\dumps` -> `.\dumps`. `STE_NO_CRASHDUMP=1` turns the handler off; `STE_CRASH_CONTINUE=1` hands the crash to Windows error reporting / a debugger instead of exiting.
The game never writes `ste_video.ini`, `ste_enhance.ini` or the input mapping file (checked in the source and by file hash and timestamp after a full run, c257); it only reads them.

Logs: the game writes to stdout/stderr only; the caller redirects them (for example to one `.out`/`.err` pair per run). Useful lines: `[CONFIG] game=`, `[CONFIG] ste_enhance.ini=`, `[VIDEO] ini=`, `[SESSION]` (desktop size, DPI, RDP), `[DRAWDIST]`, `[ENHANCE]`, `[GAME-OWNED-WRITE-BLOCKED]` (must stay absent), `[XA2] underrun`, `[ICALL FAIL]` / `[STUB HIT]` (fatal). `mods\last_report.txt` is rewritten at each start.

## 7b. Video start-up log and present rules (c251-c257)
Every start logs, in this order: `[VIDEO] DPI awareness: per-monitor`, `[VIDEO] display request=N -> monitor \\.\DISPLAYk at x,y size WxH (SDL3 order|primary)`, `[VIDEO] ini=... out=WxH render=WxH mode=0|1|2 aspect=0|2 ... vsync=0|1`, `[VIDEO] vrr=...`, and
`[VIDEO] present: flip_model=yes|no tearing_supported=yes|no tearing_used=yes|no vsync=on|off vrr=auto|off window_mode=window|borderless|fullscreen swap_effect=... buffers=N output=\\.\DISPLAYk display_mode=WxH@Hz`
(`display_mode` is the real current mode of the monitor the window is on). In fullscreen also `SetFullscreenState(TRUE) = 0x...` and `fullscreen WxH: ResizeTarget = ..., ResizeBuffers = ...`.
Present rules: `vsync=on` -> `Present(1, 0)`; `vsync=off` -> `Present(0, ALLOW_TEARING)` only on the tearing flip-model chain, otherwise `Present(0, 0)`. Every `Present`, `ResizeBuffers` and `SetFullscreenState` result is checked. A failed `Present` logs
`[VIDEO] Present(sync=N, flags=0x..) failed: 0x...` and drops once to the safe path (interval 1, no tearing flag) and continues; device removed / reset exits with code 10. If Windows answers `0x887A0022` (not currently available) to the fullscreen request the game takes the foreground and retries up to 5 times before falling back to borderless.
Fixed in c251: `vsync=on` raised an integer divide by zero (`0xC0000094`) on the scan-out thread right after the first frames (the pacing period was 0). It was not the DXGI tearing rule. Any earlier build crashes with `vsync=on`, whatever `vrr` says.

## 8. Input (SDL3, c250) and overlay requests
* **Backend**: SDL3 gamepads (Xbox, PlayStation, Switch, generic; SDL 3.4.18, zlib, static) are the default host pad source. XInput stays as the fallback (`STE_INPUT_BACKEND=xinput`, or SDL failing to start). Pads are assigned to Xbox ports in connection order; hot-plug is noticed within about 0.25 s and shows the game's own "controller removed" screen. The first `[INPUT]` lines in the log name every pad (name, GUID, type).
* **Mapping file** (`STE_INPUT_MAP`, passed at every launch, read once): per port `device` (`auto` = next unclaimed pad in connection order, `keyboard`, `none`, or a pad GUID), then `settings` (`deadzone` 0..0.5 radial, `rumble` 0..100 %, `bindings`). If the pad that ends up in a port has another GUID than the file recorded, the game uses `profiles[<its GUID>]`, then `profiles["default"]`, then built-in defaults. Without a file: Xbox layout, Black = LB, White = RB, no deadzone (the game applies its own), full rumble. Unknown keys are ignored; a file that is not `format: 1` is ignored with a log line.
* **Bindings**: sources are SDL names (`a b x y back start leftshoulder rightshoulder leftstick rightstick dpup dpdown dpleft dpright lefttrigger righttrigger`, positions as in the launcher); actions are the Xbox controls. A trigger bound to a button action counts as pressed above 50 %; a button bound to a trigger action is a full press. `move` / `look` take `leftstick` / `rightstick`; the stick Y axis is converted to Xbox orientation. `menu_swap_confirm_back` is for the launcher/overlay menus only.
* **Keyboard**: `key:<SDL key name>` bindings in the keyboard profile are applied when the keyboard is the port's device (letters, digits, arrows, Return, Space, Tab, Backspace, F1-F12, Left/Right Shift/Ctrl/Alt, Insert/Delete/Home/End/PageUp/PageDown, Keypad 0-9, punctuation). Unsupported names are ignored. The move/look keys stay W A S D / I J K L. Keys reach the game only while its window has focus.
* **Overlay request** (the overlay itself is item 6, not in the exe yet): Guide/Home/PS button on any pad (edge), the `overlay_key` (F1), or Back+Start held `overlay_chord_ms` on one port. Each logs `[OVERLAY] request: ...`. Windows Game Bar (Settings > Gaming > Game Bar > "open Game Bar using this button on a controller") and Steam can capture the Xbox button before SDL sees it; the key and the chord are the fallbacks.
* **Test switches** (not for launchers): `STE_SDL_VIRTUAL=<n>` attaches n virtual pads, `STE_SDL_VIRTUAL_CHURN=1` attaches/detaches one every 4 s, `STE_INPUT_TRACE=1`, `tests\sdl_input_selftest.c` (32 checks, run with `tests\sdl_input_map_test.json`).
* The launcher may set `launch.input_map.supported=true` for builds that print `[INPUT] backend=sdl3` (c250 and later).

## 8b. Planned (not in the exe yet)
* **XISO**: `STE_GAME_PATH=<.iso>` (read-only mount in the file layer).
* **In-game overlay** per `launcher\docs\OVERLAY.md` (pause, Resume, live settings, Return to launcher, Quit).
## 9. Mismatches closed (INTERFACE_ASSUMPTIONS.md)
M1 `play_low.bat` newline artefact fixed (34 launcher files; the old text had no effect: `atoi` of the value still gave 2, and both texture packs are `enabled:false`). M2 game path: section 3. M4 `STE_VIDEO_INI` / `STE_ENHANCE_INI`: section 4, both logged. M5 build id: VERSIONINFO + `--version`, stable name. M3 input: section 8 (SDL3 + mapping file). M6 licence: GPL-2.0-or-later.

## 9. Files the runtime writes (c258 audit)

A release run writes only: saves\ (the Xbox partition images and caches, under RECOMP_SAVE_DIR or next to the exe), shadercache\ next to the exe (compiled shaders, safe to delete), and, on a crash or a 120 s stall, <data>\logs\dumps\*.dmp (see section 7). Nothing is written to the working directory, to the game folder or to a development path. Debug output is opt-in:

| variable | effect |
|---|---|
| XBOX_LOG_FILE=<path> | kernel-layer log to that file (INFO level). Without it only warnings and errors go to stderr. (Before c258 this was always xbox_kernel.log in the working directory.) |
| STE_STARTUP_TRACE=<path> | enter/exit trace of one start-up function. |
| STE_WATCHDOG_GUEST=1 | the stall watchdog also writes the 128 MB guest-RAM images next to its minidump (default: minidump and log lines only). |
| STE_SHOT_SEC=<n>, STE_SHOT_FIGHT, STE_SHOT_DIR=<dir> | periodic / fight screenshots (default off; default directory <data>\logs\dumps\shots). |
