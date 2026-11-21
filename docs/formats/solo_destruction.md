<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Solo Destruction mod (0.3) - how it works and how to build it

Idea credit: ANILORAK's Godzilla Ultra research ("Solo Destruction" mode). The scripts here are our own edits of the game's retail scripts; nothing of his is copied. Based on ANILORAK's Godzilla Ultra research.

## What the player sees
Main menu: a new row **SOLO DESTRUCTION** (between MULTI-PLAYER and PURCHASE ITEMS). It opens its own select screen (the one-player layout the game uses for Action/Survival: title SOLO DESTRUCTION, no VS graphic, no second box, no COM step): pick YOUR monster (A), X = Rules, then the city. Rules shows two rows: **ROUND LENGTH 1-10 MIN** (exactly that many minutes, applied by the game as in Versus) and **NO TIME LIMIT NO/YES** (YES = the clock shows 99:59 and counts down, the round never ends by time). The match is the retail Demolition challenge on that monster and city: SCORE counter, "Destroyed: <building>" points, countdown; at 0:00 (or when the monster is down) CHALLENGE RESULTS shows SUCCESS with the points (the goal is 0, there is nothing to fail), CONTINUE returns to the select screen. Off unless the mod is enabled; with the mod disabled the menu and every mode are retail, and normal Versus is unchanged while the mod is on.

## How it works (reference for modders)
Three scripts are patched (edited on the user's own game files with our own script editor, tools\modssf_edit.py):
| file | change |
|---|---|
| `ShellData\mainmenu.bsf` | the hidden menu row `SystemLinkCheat` (retail: only with a cheat on non-US consoles) is shown and relabelled; selecting it calls `createGame(1, 0)` (Versus) with a transient flag. `createGame()` clears a stale flag first. The old System Link cheat row is replaced by this row while the mod is on |
| `ActionData\Game.bsf` | `setupConventionalGame`: if the flag is set, clear it and load `ChallengeDemolition.bsf` instead of `ConventionalGame.bsf` |
| `ActionData\ChallengeDemolition.bsf` | when the game type is 1 (Versus; retail never runs this script with type 1): level = `gi.getCityName()/getDaylight()` instead of the fixed `ChallengeDemolition` city, the chosen monster is kept (retail forces godzilla2k), no intro cinematic (it needs the retail level's camera path and building), goal 0, round time from the Rules screen (the game applies it; NO TIME LIMIT = YES sets 5999.9 s through shell cheat bit 1<<23, read and cleared by the script), the time-up test ignores a 0 round time |
The transient flag is **shell cheat bit 1<<22 (4194304)** in `ShellGameInfo.getShellCheats()`: the game info is rebuilt by `recreateGame` on the profile / monster-select / rules screens (which wipes mission, difficulty and the debug settings, all tried), but the cheat bits persist. `Game.bsf` clears the bit as soon as it reads it, and `createGame()` clears it, so it only lives from the menu click to the loading screen. Not tried: nothing else uses bit 22 (retail uses 1, 2, 4, 8, 4096, 8192 and the unlock bits).
Behaviour kept from retail: points per building, the 'Time' power-up (+10 s), pause screen, the summary screen and the rule that a monster at minimum hit points ends the match.

## Build it (the repo ships the recipe, never game data)
`python -I tools\modsuild_solo_destruction.py <game folder or .iso> <out mod folder>` applies the edits above to the three scripts with
`bsf_edit.py` (stops if the text does not match the retail Xbox scripts) and writes a **recipe mod** (manifest api 2, `format: "bsf-edit"`,
only edit lists + hashes, about 85 KB, no game bytes, safe to share; docsormats\mod_build_recipe.md). The launcher's Prepare mods step
(or `iso_to_mod.py prepare <mod> <game>`) builds `_built\` from the user's own files in under a second and verifies the result hashes.
No third-party tool is needed. Offline cross-check: the same edits made with GZBuildr give identical token streams (the only difference is
that GZBuildr rounds the number pi to 6 decimals; ours keeps the original value).
Compiler note: edited scripts are slightly smaller than the retail ones (the source-line markers are dropped); the game runs them without problems.

## Verified in game (c431 package exe, retail game folder)
Menu row; Tokyo + Godzilla 2000; Osaka North + MechaGodzilla 3 with ROUND LENGTH 10 MIN (clock 99:59 and counting); a 15 s test round to the CHALLENGE RESULTS screen (SUCCESS, 0 points, goal 0) and CONTINUE back to the Versus select. Not verified: points banner at a higher score, the Time power-up, 21:9 HUD placement, other cities and monsters beyond the two above.

## Solo screen and rules (monsterselect.bsf)
`ShellData\monsterselect.bsf` is edited too: while the solo flag (shell cheat bit 1<<22) is set the screen takes the existing one-player branch (`getMode`, `setMode`: `OnePlayer` shown, `TwoPlayer`/`FourPlayer` hidden, `NumPlayers = 1`, `sMinPlayers = 1`), the title is set to "SOLO DESTRUCTION" (written by the script, English in all languages: the localisation pools are inside ShellCommon.bdl, not in .LOC files), the Difficulty spinner is hidden, `usingRule` shows only rule 3 (ROUND LENGTH) and rule 4 (the YES/NO row of Reset arenas, relabelled NO TIME LIMIT, own storage key `rule_solo_notimelimit`). `startGame()` (the moment the lobby closes) clears and, if the NO TIME LIMIT row is YES, sets bit 1<<23 on both game-info objects it can see (`sgi` of the lobby and `gShell.getGameInfo()`); `ChallengeDemolition.bsf` reads and clears that bit and sets a 5999.9 s clock. Setting the bit earlier (in the Rules `writeSettings`, or on the shell game info only) did not arrive: the lobby's game info is copied over when the game starts, so the bit must be set on `sgi` at the start. `mainmenu.bsf createGame()` clears stale copies of both bits. Flag not set = every original branch.

## Round length: what was wrong
The first version overrode the clock when `gi.getRoundTime() >= 599` at script load (to make 10 MIN mean no limit). Probe without that line: ROUND LENGTH 7 MIN gives a 7:00 clock, 1/3/10 MIN give exactly that (tests y_m1, t_m3, t_m10: clock starts at 0:59-ish/2:51/9:51 after the 9 s of play before the first shot, counts down, 1 MIN ends at CHALLENGE RESULTS). The script no longer reads the round time. The old reading is not proven to be the whole cause of the report (the value seen at load may not be the Rules choice); it is gone either way. 10 MIN is now 10 minutes and NO TIME LIMIT is its own choice.
