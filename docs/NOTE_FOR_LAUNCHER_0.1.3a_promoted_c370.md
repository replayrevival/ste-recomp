<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Note for the launcher session: 0.1.3a hotfix promoted as c370 (2026-11-13)

**PROMOTED: c370, SHA-256 3072234461DCD3BB53701701F8A169F1ACC8EFDE30C7C76A98D5993468D73342** (artifacts\ste-recomp.exe, FileVersion 0.1.3.370 / FILEVERSION 0,1,3,370, ProductVersion "0.1.3a+c370", `--version` string 0.1.3a). Replaces c369 (kept as artifacts\ste-recomp_c369.exe, the published 0.1.3). capabilities.json unchanged. Validator: 0 FAIL.

What changed since c369 (one fix): a freeze. The emulated vblank interrupt keeps the NV2A "vblank pending" bit set until the game's vblank handler has started; a DPC another thread queued at the same instant could make the emulation miss that handler, so the bit was never released and the game's own "clear it and wait" loop spun for ever (no more frames, window stays open, the lock the game thread needs held by the timer thread). The flag is now per thread and a 100 ms valve releases the bit. Cause shown by a watchdog dump of a c369 test run (docs: CHECKPOINT ledger 2026-11-13). Seen 3 times in test runs with a debug frame-sleep, never reported in normal play.

Verification: mixed soak on c370 (front end / attract idle, versus fights in 4 cities, melee, an action mission load, scripted pause and return): all alive, 0 crash dumps, 0 ICALL/STUB, 0 [DISPATCH] stall lines, vblank 60.0 Hz. Frame time p99 equal to c369 within noise (c370 20.9-23.3 ms vs c369 21.3-21.9 ms over repeated runs on the same scripted fights). The freeze itself did not recur in c365-c369 test loops either, so the fix rests on the dump evidence.

Known issues: Mechagodzilla 3's back missiles invisible in flight. A possible rare stall in the DirectSound buffer code (two test dumps of c367 with debug sleep; not reproduced, not fixed) is listed in RELEASE_ALPHA.md.

Screenshots (rendered captures, no game files): see the file list the next note gives once the set is committed.
