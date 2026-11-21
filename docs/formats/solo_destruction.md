<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Solo Destruction mod (0.3) - how it works and how to build it

Idea credit: ANILORAK's Godzilla Ultra research ("Solo Destruction" mode). The scripts here are our own edits of the game's retail scripts; nothing of his is copied. Based on ANILORAK's Godzilla Ultra research.

## What the player sees
Main menu: a new row **SOLO DESTRUCTION** (between MULTI-PLAYER and PURCHASE ITEMS). It opens the normal Versus screens: pick YOUR monster, then press START to add a COM and pick any monster for it (Versus needs two slots; the mode ignores the COM and plays alone), X = Rules (ROUND LENGTH 1-10 MIN; **10 MIN = no time limit**, the clock shows 99:59 and counts down), pick the city. The match is the retail Demolition challenge on that monster and city: SCORE counter, "Destroyed: <building>" points, countdown; at 0:00 (or when the monster is down) CHALLENGE RESULTS shows SUCCESS with the points (the goal is 0, there is nothing to fail), CONTINUE returns to the Versus select. Off unless the mod is enabled; with the mod disabled the menu and every mode are retail.

## How it works (reference for modders)
Three scripts are patched (all from the user's own game, compiled with the GZBuildr script tool):
| file | change |
|---|---|
| `ShellData\mainmenu.bsf` | the hidden menu row `SystemLinkCheat` (retail: only with a cheat on non-US consoles) is shown and relabelled; selecting it calls `createGame(1, 0)` (Versus) with a transient flag. `createGame()` clears a stale flag first. The old System Link cheat row is replaced by this row while the mod is on |
| `ActionData\Game.bsf` | `setupConventionalGame`: if the flag is set, clear it and load `ChallengeDemolition.bsf` instead of `ConventionalGame.bsf` |
| `ActionData\ChallengeDemolition.bsf` | when the game type is 1 (Versus; retail never runs this script with type 1): level = `gi.getCityName()/getDaylight()` instead of the fixed `ChallengeDemolition` city, the chosen monster is kept (retail forces godzilla2k), no intro cinematic (it needs the retail level's camera path and building), goal 0, round time from the Rules screen (>= 10 min -> 5999.9 s), the time-up test ignores a 0 round time |
The transient flag is **shell cheat bit 1<<22 (4194304)** in `ShellGameInfo.getShellCheats()`: the game info is rebuilt by `recreateGame` on the profile / monster-select / rules screens (which wipes mission, difficulty and the debug settings, all tried), but the cheat bits persist. `Game.bsf` clears the bit as soon as it reads it, and `createGame()` clears it, so it only lives from the menu click to the loading screen. Not tried: nothing else uses bit 22 (retail uses 1, 2, 4, 8, 4096, 8192 and the unlock bits).
Behaviour kept from retail: points per building, the 'Time' power-up (+10 s), pause screen, the summary screen and the rule that a monster at minimum hit points ends the match.

## Build it (the repo ships the recipe, never game data)
`python -I tools\mods\build_solo_destruction.py <game folder or .iso> <out mod folder> --gzbuildr <gste_script_tool_v3.py>`
GZBuildr (Digitzaki, GPL-3.0, separate download) turns the three retail scripts into text, the tool applies the edits above (and stops if the text does not match the retail Xbox scripts), GZBuildr compiles them, and `iso_to_mod.py make` writes a manifest-api-2 patch mod (3 step1 patches). The launcher's Prepare mods step (or `iso_to_mod.py prepare`) builds `_built\` from the user's files. The patch data contain compiled script bytes derived from the game's scripts: **do not publish the built mod folder**; publish this tool and build locally.
Compiler note: GZBuildr's compiled scripts are smaller than the retail ones (it drops the source-line metadata markers); the game ran them without any problem (menus, Versus, Demolition, results; 0 errors in the soak runs).

## Verified in game (c431 package exe, retail game folder)
Menu row; Tokyo + Godzilla 2000; Osaka North + MechaGodzilla 3 with ROUND LENGTH 10 MIN (clock 99:59 and counting); a 15 s test round to the CHALLENGE RESULTS screen (SUCCESS, 0 points, goal 0) and CONTINUE back to the Versus select. Not verified: points banner at a higher score, the Time power-up, 21:9 HUD placement, other cities and monsters beyond the two above.
