<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Report 0.3 (2026-11-20 and following)

Decision log, newest facts first within each section. Evidence paths are private work folders (F:\GodzillaSTE_work\solo, F:\GodzillaSTE_work\runs433) unless a repo path is given.

## Policy (2026-11-20)
ANILORAK's research: ideas only, rebuilt in our own code, credited "Based on ANILORAK's Godzilla Ultra research". Third-party tools (GZBuildr, the Roster Editor) are used offline for research or cross-checks only, never bundled or required at install time.

## 1. Own BSF editor (done)
`tools\mods\bsf_edit.py` (stdlib, GPL-2.0-or-later, format in `docs\formats\bsf_script.md`). Token-level: parse, replace a token sequence that must match exactly once, emit. Unedited tokens keep their original opcodes.
Cross-check against GZBuildr output (offline): ChallengeDemolition.bsf and Game.bsf byte-identical; mainmenu.bsf differs in one number: GZBuildr rounds pi to 3.141593, ours keeps the original float32 3.1415927 (ours is the more faithful).
Delivery changed from "built mod holds compiled script bytes, must not be published" to a **recipe mod** (`format: "bsf-edit"`, `docs\formats\mod_build_recipe.md`): only edit lists + hashes (~85 KB), no game bytes, safe to share; `iso_to_mod.py prepare` builds `_built\` from the player's own files in under a second. GZBuildr is no longer needed anywhere. The launcher can now build it with the Prepare step (python stdlib only).

## 2. Solo Destruction checks (done)
21:9 (2560x1080): HUD, clock and score render in the same place as in 16:9 (the 16:9-safe HUD area); score 1475 displayed in a Tokyo round (4-digit score fits; 5 digits not reached). 0 ICALL/STUB in the run.

## 3. Disk space (done)
E: had 0.3 GB free. Moved 444 old candidate artifacts (c<425 exe/ilk/pdb/lib, ~19.9 GB) from `mission-l6\artifacts` to `F:\GodzillaSTE_work\archive_artifacts\` (moved, nothing deleted). E: now ~21 GB free. Remaining large items: artifacts\saves (5.9 GB, run saves), logs (15 GB).

## 4. Solo screen polish (done)
Cause of the plain Versus look: the screen is chosen by game type. Solo keeps game type 1, so `monsterselect.bsf` now uses the existing one-player layout (the one Action/Survival use) while the solo flag is set: no VS graphic, no second box, own bios panel, only one monster to pick, no COM step (the COM was never needed: the Demolition script uses one player). Title text "SOLO DESTRUCTION" is written by the script (English for all languages): the localisation strings live in packed pools inside ShellData\ShellCommon.bdl (no .LOC files on the Xbox disc), so adding a key would need a bundle rewrite; not done, listed as a later option. Rules screen shows only the solo rules. Normal Versus uses the original branches (flag clear).

## 5. Round length bug (fixed)
Reported: the clock always showed 99:59. The first version made 10 MIN mean "no limit" by testing `gi.getRoundTime() >= 599` when the Demolition script loads. With that line removed the chosen length is honoured (7 MIN gave a 7:00 clock, probe run p2), so that line was the only thing touching the clock; I did not prove what value it read at load. Fix: the script never reads the round time. In solo mode the Rules screen has two rows: ROUND LENGTH 1-10 MIN (applied by the game exactly as in Versus) and NO TIME LIMIT NO/YES (the YES/NO spinner of the unused Reset-arenas row, relabelled; Versus's Reset arenas is untouched). YES makes `startGame()` set a transient cheat bit (1<<23) that ChallengeDemolition.bsf consumes and clears, turning it into a 5999.9 s clock. Results: section 6.

## 6. Time-limit tests (scripted, package exe c433 = 0.3.0, Tokyo, MechaGodzilla 3, 1920x1080)
First shot is ~9 s into the round, so a correct clock starts a few seconds under the chosen time.
| Round Length | NO TIME LIMIT | clock at first shot | later | end |
|---|---|---|---|---|
| 1 MIN | NO | 0:51 | 0:15 | CHALLENGE RESULTS "SUCCESS" (run y_m1) |
| 3 MIN | NO | 2:51 | 0:39 | counting (run u_m3) |
| 10 MIN | NO | 9:51 | 7:34 | counting (runs t_m10, u_nolim before the fix) |
| 10 MIN | YES | 99:50 | 97:33, keeps counting | no end by time (run x_nolim) |
Normal Versus with the mod enabled (y_versus): title VERSUS, VS graphic, COM box, COM step, Rules with DIFFICULTY / ROUNDS / ROUND LENGTH / RESET ARENAS: unchanged. Note: the solo ROUND LENGTH row and Versus's use the same game setting (the game reads one stored value); NO TIME LIMIT has its own.
How it was found: the first NO TIME LIMIT version set the bit in the Rules `writeSettings`; the run showed a normal 10:00 clock, a forced-on diagnostic with the bit set on the shell game info only also failed. The lobby's game info is copied when the game starts, so the bit is now set in `startGame()` on the lobby info as well (run x_nolim shows 99:50).
Wording about the cause: with the line `if getRoundTime() >= 599` removed, 7 MIN gave a 7-minute clock; the script no longer reads the round time. I did not prove that the value read at load was always >= 599; the report of "always 99:59" is explained by that test being the only thing that touched the clock.

## 7. Not done / open
- Title text is English in every language (script text); a real localisation key would need ShellCommon.bdl pool surgery.
- Clip: no ffmpeg.exe found on this PC (asked the launcher session); recording needs STE_FFMPEG.
- Mothra in solo (the one-player layout offers larva/moth toggling) is not tested.
