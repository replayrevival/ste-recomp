<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Replay Revival - Godzilla: Save the Earth (Xbox) recomp - ALPHA 0.1.5 (draft)

**Early alpha. Work in progress, development continues.** Not affiliated with or endorsed by Toho, Atari or Pipeworks. You must own the game; no game data is included or distributed. Windows only for now.
Draft prepared 2026-11-10, 0.1.4 section added 2026-11-14, 0.1.5 section 2026-11-15. Nothing here is published; the owner publishes.

## What changed since 0.1.4
| Area | Change | Status |
|---|---|---|
| In-game overlay | press **Back+Start for one second** (or F1, or the Guide button) to open the launcher's settings over the game: Video, Enhancements, Controllers (incl. remapping), Return to launcher, Quit. The game is paused while it is open and nothing reaches it; pad, keyboard and mouse work. Return to launcher exits with code 20. It needs `ste_overlay.dll` (with its `ui`, `assets` and `config` folders) next to the game, which the launcher package provides; without it the game runs as before. `master_volume` and `hide_cursor` apply at once. | in |
| Button words and PC wording | with the Xbox or PlayStation button style, "PRESS START" reads PRESS MENU / PRESS OPTIONS, "Press START to join" and friends follow, the dodge tutorial says LT/RT or L2/R2, and Xbox-only save texts say "Computer" / "this computer". The original text is untouched with `button_prompts = xbox_original`. `STE_TEXT_PATCH=0` turns it off. The text is changed in memory after the game has loaded it (about 1-2 % of one core for the scan). | in |
| Back+Start | held together they are now the overlay chord and are hidden from the game (so Start does not also open the game's pause menu) | in |
| Version | exe resource and `--version` say 0.1.5 | in |

## What changed since 0.1.3a
| Area | Change | Status |
|---|---|---|
| Start-up speed | **about 20 s faster every launch.** The game's network start-up waited ~20 s for an Ethernet link that does not exist (we reported "cable connected"). The runtime now answers "no cable" like a console without one, so the first game file is opened after 1.7 s instead of 22 s and the intro movies start 20 s sooner. `STE_ETHERNET_LINK=up` restores the old answer; a future online path (for example an Insignia-style service) would use that override. Online play never worked and still does not. | in |
| Start-up diagnostics | a `[BOOT]` timeline in the log (milliseconds since the process started, one line per step, plus the first file opens); `STE_BOOT_SAMPLE=1` adds a 2 s sampler. Symbol loading for crash reports moved to a background thread (it cost 1-3 s on a cold first launch). | in |
| Mods that are patches | the game reads an enabled patch mod's `mods\<id>\_built\<file>` (built by the launcher or by `tools\step-diff\step_diff.py`, a small GPL tool that makes and applies `step1` patches) only when its SHA-256 equals the manifest's `result_sha256`; a missing file or a wrong hash falls back to the original and says so in the log (`[MODS] ... not built` / `RESULT MISMATCH`). Mods never contain game data. A self-test mod is in `tests\mods\ste.test-patch`. | in |
| Settings | `[audio] master_volume` (0-100, default 100) and `[window] hide_cursor` (`auto` hides the pointer after 3 s without movement, `always`, `never`) in the settings ini; both are also what the in-game overlay will change live | in |
| Crash reports | the report lists the last 48 memory-protection calls (who made the faulting page read-only); `STE_PROTECT_LOG=1` prints them live; `STE_DISK_SLOW_MS` / `STE_DISK_SLOW_JITTER` simulate a slow disk for tests | in, opt-in |
| In-game overlay (moved to 0.1.5) | the launcher's overlay (Video, Enhancements, Controllers, return to launcher = exit code 20) is written but **not in this build**: inside the game process its start-up still crashes in the launcher library (the launcher session is looking at it), and linking it needs a newer Visual C++ runtime than the game needs today. It is built only with `candidate376\relink376b_overlay.bat` | not in |
| Text wording | START/BACK and "Xbox console" re-wording per button style is not in this release: the game looks its text up by the English string, so editing the strings breaks loading (docs\REPORT_0.1.4.md). The code is in, off unless `STE_TEXT_PATCH=1` | not in |
| Controller test | `STE_INPUT_SELFTEST=1` runs a virtual Xbox One pad through the real mapping (every face button, Black/White, triggers, Start/Back, D-pad) and prints PASS/FAIL; the log now shows each pad's effective bindings and warns about custom face-button bindings (docs\CONTROLLER_MAPPING_TRACE.md; an A/X swap was reported, no defect was found in the game's chain) | in |
| Version | exe resource and `--version` say 0.1.4 | in |

## What changed since 0.1.2d (carried over from 0.1.3)
| Area | Change | Status |
|---|---|---|
| Input / window | mouse pointer auto-hides while the game has focus; window client size corrected on monitors with a different DPI | in |
| Capture | opt-in frame ring (`STE_CAPTURE_RING=1`, F12 saves the last 60 frames) | in, off by default |
| Widescreen | hit/impact screen flash covers the whole widened view (it was an inset rectangle on ultrawide) | in |
| Draw distance | `draw_distance=unlimited` fog x8 and the scenery query volume `query_scale` (1..4, default 1.5): the Tokyo skyline no longer pops in at ~1360 units | in, optional |
| Performance | game code is built optimised (four hot generated chunks were not), faster vertex path, precise 60 Hz vblank; governor keeps the frame rate when the distance is wide (`query_scale_auto`) | in |
| Stability | frames are consumed from a private copy of the pushbuffer ring (the faster game could overwrite unconsumed commands), vertex reads beyond RAM are zero-filled instead of crashing; x4/x8 sessions of ~19 min ran clean | in |
| Build recipe | the `D3DDevice_Swap` frame hook is now a normal `ste_hooks.toml` hook; before this a build from the published recipe produced no picture | in (**must be in the public tree**) |
| Test tools | `STE_PERF`, `STE_PERF_SAMPLE`, `STE_TITLE_FPS` (fps in the title bar), `[PBLAP]`, `[QGUARD]` log lines | in, opt-in |
| Button prompts | `[input] button_prompts` / `STE_BUTTON_PROMPTS` = `auto` (PlayStation pad -> PlayStation symbols, any other pad -> Xbox symbols, no pad -> original art) / `xbox` / `playstation` / `xbox_original` (byte-identical to the shipped art). Swaps the A, B, X, Y face-button pictures through a texture pack (art: Kenney Input Prompts, CC0). START / BACK / Black / White are words in the font and are not changed yet (0.1.4) | in |
| No console window | the exe is a Windows-subsystem program: no black console behind the game. The log goes to `<logs>\ste_<date-time>.log` (next to the settings folder, else `data\logs`); `--console` or `STE_CONSOLE=1` opens a console that mirrors it; a launcher that redirects stdout/stderr keeps working as before. Exit codes and the `[CRASH]` dump are unchanged | in |
| Out-of-RAM guard | texture reads that would run past the 64 MB of Xbox RAM are refused (vertex reads already were) | in |
| Version | exe resource and `--version` say 0.1.3 | in |
| Point sprites | point-list draws (the title's missile bodies and smoke trails) are drawn as screen-space quads instead of one pixel (`STE_POINT_SPRITES=0` restores the old behaviour). Missiles are still not fully visible, see Known issues | in |
| Governor | the query-distance governor now moves its applied value slowly (`query_ramp_pct`, default 5 % per second) so the distance does not pump | in |
| Debug options | `STE_RING_AUTO_GAP_MS`, `STE_RING_FIGHT_FROM`, draw-log windows and the OOB report are opt-in test switches, off by default | in, opt-in |

## Known issues
- No keyboard typing on the profile name screen yet (use a pad, or map the keyboard as a pad); planned for 0.1.6.
- The footer word BACK next to the B picture is not re-worded (it means "go back"); only START and the Xbox-only save texts are.
- No keyboard typing on the profile name screen (the on-screen keyboard still needs a pad or the keyboard mapped as a pad).
- Mechagodzilla 3's back missiles are invisible in flight (launch fireball and impacts show). Cause not found after probes through c366 (docs\ITEM_D_MISSILES.md): the particle step is a constant 1/60, so it is not frame-rate dependent; the head/trail art is not in the scene list. Investigation continues in 0.1.4. Mechagodzilla 2's finger rockets are untested.
- A rare freeze (no more frames, window stays open) was fixed in 0.1.3a (c370): the emulated vblank could wait for ever on an interrupt-status bit when another thread queued work at the same moment. It was seen three times in test runs, never in normal play reports.
- Possible rare stall in the sound code: in two test runs of an earlier build (c367, debug frame-sleep on) the game stopped producing frames while its main thread sat in the DirectSound buffer routine (the game's audio set-up). Not reproduced since and not fixed in c370; if the game freezes, please send the log from `<logs>`.
- Black Mamba (the owner's other PC) feedback is pending: two-monitor window test (c261) and the distance/performance test zip `ste-recomp-distance-perf.zip`.
- 1%-low frame rate reads ~52 fps in every mode (frame-pacing jitter, about 18-19 ms p99); average is ~59 fps.
- `query_scale` above 1 draws far objects the game never draws at that distance; some may look odd (stale vertex data is zero-filled, so they vanish rather than glitch). The governor lowers the distance in heavy fights.
- Linux/macOS are not supported yet (see `PORTABILITY_AUDIT.md`).

## Release checklist (before anyone publishes)
1. Licence audit of the merged tree file by file (GPL-2.0-or-later for our code; compiled-in third-party code GPL-compatible; Apache-2.0 compiled in => binaries ship as GPL-3.0). Reports: `F:\GodzillaFamily_Assets\reports\hermes2\license_audit.md`.
2. No game data anywhere: no default.xbe, no extracted files, no generated C in the repo (`.gitignore` covers `generated/`); screenshots with game imagery stay out. Run the repo scan.
3. GPL notices, `THIRD_PARTY_NOTICES.md`, `CREDITS.md` current; `GAME_DATA_NOTICE.md` in the package.
4. Disclaimer in README, About and the launcher: "Not affiliated with or endorsed by Toho, Atari or Pipeworks. You must own the game."
5. Hash checks: `ste/gen/xbe.sha256` verifies the user's default.xbe; `expected_generated.sha256` (includes the new `recomp_0013.c`) verifies a local regeneration; record the SHA-256 of every shipped binary.
6. Clean-directory run: unzip into an empty folder, point at a legitimate copy, play Versus Tokyo + New York for 15 minutes.
7. Version strings 0.1.3 (exe resource: done in c369; launcher, README: pending); changelog from the table above.
8. A build from the **published recipe** (not the dev tree) must show a picture and match the dev build's behaviour.

## Test plan for 2-3 PCs
| PC | What |
|---|---|
| Dev PC (RTX 2080 SUPER, Windows 11) | done: 1765x993 and 3840x2160, original / x4 / x8, Tokyo + New York, 3 x 19-min x8 + 1 NY + 1 x4 soaks |
| Black Mamba (owner) | `play_dist_original.bat`, `_4`, `_8` at 1080p and its native resolution; report fps in the title bar, pop-in, any stutter; send `data\logs\play_dist_*.log`; two-monitor window test when asked |
| Low-end 4-core / iGPU | `play_dist_original.bat` at 1280x720 and 1920x1080 (the 60 fps floor), then `_4`; the governor should hold the frame rate by shortening the distance |
Record for each run: exe SHA-256, resolution, mode, median fps, worst frame, `[QGUARD]` cap range, crash/replay lines (`[CRASH]`, `[PB-REPLAY]`, `[KELVIN-OOB]`).
