<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Note for the launcher session: 0.1.5a promoted as c381 (2026-11-15) - replaces 0.1.5 (c379), do not publish c379

**PROMOTED: c381, SHA-256 966EB7515E2EFC78871940175BAA95304FBA6492894D8508582C52713B571D66** (artifacts\ste-recomp.exe, 66,033,664 bytes, FileVersion 0.1.5.381 / FILEVERSION 0,1,5,381, ProductVersion "0.1.5a+c381", `--version` 0.1.5a). Replaces c379 (kept as artifacts\ste-recomp_c379.exe). capabilities.json unchanged. Imported DLLs identical to c379/c377. Same `ste_overlay.dll` as before (no change on your side except the points below). Miko re-tests on Black Mamba before anything is published.

## Why
Miko's 0.1.5 test on her other PC: opening the in-game menu (Guide or Back+Start) froze the game (true fullscreen, V-Sync on): window greyed out, close button and Alt+F4 dead, pointer invisible after Alt+Tab, reboot needed. Reproduced on the Stream PC with c379 (window hung for the whole test, WM_CLOSE ignored). Root cause: the game window belongs to the render thread, which only handled window messages while it ran game frames; the menu pauses the game, so no frames, so the window stopped answering Windows (and in true fullscreen the display driver waits on it: threads blocked in the NVIDIA driver in her dump). Full analysis: docs\REPORT_0.1.6.md (section 0.1.5a).

## What changed (c381 = c379 + this)
- Render thread idle wait = MsgWaitForMultipleObjects + message dispatch; the overlay's pause wait pumps too. The window answers Windows while paused; WM_CLOSE / Alt+F4 / the close button end the game (exit code 0) with the menu open.
- Cursor forced visible while the overlay is open.
- Hang watchdog level: no pushbuffer frame for 3 s while the overlay is NOT open -> `[HANG]` log line, pointer restored (class cursor, ClipCursor(NULL), hide count), thread stacks and a minidump (max 3 per run); `STE_HANG_SECS` (0 = off), `STE_HANG_FORCE=<secs>` to test it.
- Guide is no longer an overlay trigger by default: `[input] overlay_guide` defaults to 0 (opt-in row if you want one); Back+Start (1 s) and F1 unchanged.
- No code sets HWND_TOPMOST (grep of our sources), none uses ClipCursor/SetCapture outside the new restore path.

## Verification (Stream PC, packaged layout with ste_overlay.dll)
- tests\overlay_matrix.ps1: window / borderless / fullscreen x V-Sync on/off x VRR on/off at 1920x1080, plus 2560x1080 (21:9) for each mode: 15/15 PASS (overlay opened; window answered WM_NULL for 12 s; a posted WM_CLOSE ended the game with code 0).
- True exclusive fullscreen, V-Sync on, overlay held open 60 s: 0 hung samples (c379 in the same setup: 29 of 30 samples hung, WM_CLOSE ignored). Alt+Tab out and back with the overlay open: the window kept answering and the game kept running (the game window was not the foreground window at the start of that test, so this is a weaker check than a real Alt+Tab from the game).
- Forced hang path (STE_HANG_FORCE=30): [HANG] line, 8 thread stacks, 52 MB minidump written.
- Validator: all PASS. Input self-test: case 1 PASS, case 2 PASS, case 3 FAIL as intended. Soak subset 450 s (versus Tokyo, melee, idle): 3/3 alive, 0 dumps, 0 ICALL/STUB, no audio starvation.
- NOT verified: the owner's own setup (true fullscreen at a high refresh rate), a real pad's Guide button, manual keyboard F1. Her dump and logs had not arrived in docs\reference\miko\ when this was written; if they show something besides the pump (e.g. a cross-thread D3D11 call), it is a second problem. By code reading the overlay draws only on the scanout thread with the scanout device (at every vblank, not tied to game frames).

## For you
- Please add to smoke_overlay.ps1: after the overlay opens, check the window answers (IsHungAppWindow false / WM_NULL answered) and that a WM_CLOSE ends the process; tests\overlay_responsive.ps1 shows how.
- Notes interim workaround for anyone on plain 0.1.5: rename ste_overlay.dll to disable the menu.
- No new screenshots (no visual change); the 0.1.5 set stays.
