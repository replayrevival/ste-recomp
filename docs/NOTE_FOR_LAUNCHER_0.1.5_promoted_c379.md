<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Note for the launcher session: 0.1.5 promoted as c379 (2026-11-15)

**PROMOTED: c379, SHA-256 CB001FD0AB28E90F421C0758DBFB2AFD5A3C4F4A083061FC1556D1685EB11A6A** (artifacts\ste-recomp.exe, 66,032,640 bytes, FileVersion 0.1.5.379 / FILEVERSION 0,1,5,379, ProductVersion "0.1.5+c379", `--version` 0.1.5). Replaces c377 (kept as artifacts\ste-recomp_c377.exe). capabilities.json unchanged (no "overlay" feature name; package the DLL with `-Overlay` when `ste_overlay.dll` sits next to the exe). Imported DLLs are identical to c377 (only VCRUNTIME140 beyond the system and CRT DLLs; checked with dumpbin /dependents).

## What is in it
- **In-game overlay** through your `ste_overlay.dll` (plain C interface, `ste_overlay_c.h`, no static link). Lazy load from the exe folder; log lines `[OVERLAY] ready (...)` or `[OVERLAY] no ste_overlay.dll next to the game: no in-game overlay`. Opens with Back+Start held ~1 s (any pad), F1 or Guide; the game is paused and gets no input while open; drawn last, before Present. Return to launcher = exit code 20 (after a second confirm), Quit = 0. `master_volume` and `hide_cursor` apply at once (`audio.master_volume`, `window.hide_cursor` answer Now; `input.button_prompts` and `enhance.*` NextMatch; the rest AfterRestart). `STE_OVERLAY=0` disables. Your smoke test (launcher\scripts\smoke_overlay.ps1) confirmed open, volume row and exit code 20 on a c379 dry-run package.
- **Text wording** per button style (xbox / playstation: PRESS START -> PRESS MENU, "Xbox console" -> "this computer", ...), edited in memory after the game has loaded the text; `xbox_original` leaves the text untouched and never starts the scanner; `STE_TEXT_PATCH=0` disables. PlayStation shows PRESS MENU where "PRESS OPTIONS" does not fit the string (fallback by design; longer strings do say OPTIONS). Table in docs\REPORT_0.1.5.md.
- Back+Start is the overlay chord and is hidden from the game.
- Not done (known issues for RELEASE_ALPHA): keyboard typing on the profile name screen (slipped to 0.1.6), Mechagodzilla 3 missiles (item D: rocket head sprite never drawn, trail emitter is spawned; analysis in REPORT_0.1.5).

## Verification
- Validator `verify_recomp_regressions.py --root candidate86`: all PASS.
- Input self-test (`tests\input_selftest.ps1` on c379): case 1 (defaults) PASS, case 2 (launcher mapping) PASS, case 3 (A/X swapped) FAIL as intended.
- Soak subset (720 s, 4K, `tests\soak.ps1 -Candidate 379`): versus_m1_tokyo, melee_ffa, action_m1, idle_virtual: 4/4 alive, 0 crash dumps, 0 ICALL/STUB. One action_m1 run showed a stall at 420-490 s in a water stage (48 audio underruns, 317 frames > 50 ms, worst 1.77 s). It did not repeat: a second c379 run was clean (0 underruns, 16 frames > 50 ms, worst 168 ms) and c377 on the same script was clean too (0, 12, 134 ms). Treated as a one-off host hiccup, noted in REPORT_0.1.5; watch for it on other PCs.
- Text styles checked on the packaged c379 (title screen): xbox and playstation say PRESS MENU, xbox_original says PRESS START.
- Effects sweep of 18 roster cells vs Gigan at 1920x1080: no new rendering defect in the sampled frames (cells 6, 8, 11, 13, 17 have too few attack frames; second pass and the other aspect mode not done).

## Screenshots
The 0.1.5 set is in publish\ste-recomp\docs\screenshots (local commit, not pushed); captions in docs\NOTE_FOR_LAUNCHER_0.1.5_screenshots.md. The 0.1.4 set moved to docs\screenshots\archive\0.1.4\.
