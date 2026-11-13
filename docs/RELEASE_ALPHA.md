<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Replay Revival - Godzilla: Save the Earth (Xbox) recomp - ALPHA 0.1.3 (draft)

**Early alpha. Work in progress, development continues.** Not affiliated with or endorsed by Toho, Atari or Pipeworks. You must own the game; no game data is included or distributed. Windows only for now.
Draft prepared 2026-11-10. Nothing here is published; the owner publishes.

## What changed since 0.1.2d
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
- Mechagodzilla 3's back missiles are invisible in flight (launch fireball and impacts show). Cause not found after probes through c366 (docs\ITEM_D_MISSILES.md): the particle step is a constant 1/60, so it is not frame-rate dependent; the head/trail art is not in the scene list. Investigation continues in 0.1.4. Mechagodzilla 2's finger rockets are untested.
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
7. Version strings 0.1.3 (exe resource: done in c367; launcher, README: pending); changelog from the table above.
8. A build from the **published recipe** (not the dev tree) must show a picture and match the dev build's behaviour.

## Test plan for 2-3 PCs
| PC | What |
|---|---|
| Dev PC (RTX 2080 SUPER, Windows 11) | done: 1765x993 and 3840x2160, original / x4 / x8, Tokyo + New York, 3 x 19-min x8 + 1 NY + 1 x4 soaks |
| Black Mamba (owner) | `play_dist_original.bat`, `_4`, `_8` at 1080p and its native resolution; report fps in the title bar, pop-in, any stutter; send `data\logs\play_dist_*.log`; two-monitor window test when asked |
| Low-end 4-core / iGPU | `play_dist_original.bat` at 1280x720 and 1920x1080 (the 60 fps floor), then `_4`; the governor should hold the frame rate by shortening the distance |
Record for each run: exe SHA-256, resolution, mode, median fps, worst frame, `[QGUARD]` cap range, crash/replay lines (`[CRASH]`, `[PB-REPLAY]`, `[KELVIN-OOB]`).
