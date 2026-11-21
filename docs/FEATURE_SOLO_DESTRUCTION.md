<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Feasibility: Solo Destruction (city destruction mode) on the Xbox recomp (2026-11-19, report only)

Question (Miko): ANILORAK's Godzilla Ultra mode "Solo Destruction" launches with `setSoloDestro(k)` + `createGame(6, 7700)` (his recipes 10 and 12); is that a retail feature in the Xbox build, and what would it take to offer it? Evidence below is from the Xbox game files (read-only), the decompiled Xbox scripts (his `bsf.py dec`, run on copies) and his recipes (reference only; none of his code is used). **Nothing was run in the game for this report** (see "Not tested").

## 1. What the Xbox build has
| Item | Finding | Evidence |
|---|---|---|
| native `setSoloDestro` | **Not a retail native.** It is absent from the Xbox `default.xbe` (no string "solo"/"setSoloDestro") and from the PS2 retail native table (`script-natives.tsv`, 1,541 rows, no such row); his recipe 12 says a script method no script calls gets a code hook ("look for an unused script method to carry a code hook"). So `setSoloDestro(k)` is **his own code patch** on the PS2 program, not something hidden in retail | xbe string scan; his program-map; recipe 12 |
| native `createGame(gameType, mission)` | **Retail** (`ShellScript.createGame`, native id 15 on both platforms) | `docs\FUNCTION_NAMES_FROM_PS2.tsv`; Xbox scripts call it everywhere |
| a retail "Demolition" challenge | **Present on Xbox, complete**: scripts `ChallengeDemolition.bsf`, `...Pause.bsf`, `...Summary.bsf`, `...Surface.bsf`, `...arena.bsf` (ActionData), city bundles `ChallengeDemolition.zip` / `_day.zip`, HUD `hudmission_Demolition.bdl`, a menu entry in `Challenges.bsf` (`if(item == "ChallengeDemolition") { createGame(8, 77); ... JumpToGame }`), goal value 15000 (`setDescriptionValue("ChallengeDemolition", 15000)`) | file listing, decompiled scripts |
| its rules (as the game defines them) | Godzilla 2000 alone (`setPlayers(1)`), one city (day/night random), **3:00 round** (`setRoundTime(60*3+0.9)`), a points counter fed by `getScore()` and a "Destroyed: <building>, Score: <n>" message per building, `POINTS_GOAL` (15000), the match ends when the clock reaches 0 or Godzilla's hit points reach the minimum, `gameWon = (gPoints >= POINTS_GOAL)`, then victory pose, `ChallengeDemolitionSummary` screen, `hud.enableClock()`, `setTrackPointsForBuildingDestruction(1)` and `setDisplayPointsBanner(1)` at the end | decompiled `ChallengeDemolition.bsf` (`processPlayingGame`, `processEndGame`, `SetEnvironmentParameters`) |
| same on PS2 | the Demolition scripts are among the **152 of 162 scripts that are byte-identical** between the PS2 and Xbox files | `compare_ps2_xbox.py` (COLLAB_GODZILLA_ULTRA.md) |
So the retail game already ships a complete, self-contained "solo city destruction" mode (the Demolition challenge); his "Solo Destruction" is a bigger mode built on the same machinery (more missions chosen by `setSoloDestro(k)`, a tactical map with a code patch, etc.).

## 2. Is it finished or partly implemented?
- The challenge is a normal retail feature: HUD, pause screen, summary screen, bundles and menu entry all exist; nothing in the scripts is stubbed. It is listed in the Challenges menu, which hides challenges until the player unlocks them (our overlay option **"Unlock all challenges"** already exists, 0.1.5+).
- Whether it **runs correctly in the recomp is not yet shown**: the recomp's fight/Survival/Challenge soaks did not specifically cover this challenge (not recorded in our reports). Risks to look at: loading of the `ChallengeDemolition*` bundles, the points banner/HUD surface (`hud.setSurface("ChallengeDemolitionSurface.ifc")`: a live GUI layer in a match), the end-of-round summary and save of the score, 21:9/16:9 HUD placement.

## 3. How a player reaches it
1. **Today, no code**: MAIN MENU -> CHALLENGES -> Demolition (after "Unlock all challenges" in the in-game menu). First task: prove it with a scripted pad pass (test-only, off by default).
2. **Our own entry ("Extras" in the overlay or a row in the game's menu)**: two ways: (a) a **script patch** through the mod system (`docs\formats\mod_patch.md`): add a row to the main menu's `.ifc` / script calling `createGame(8, 77)` as `Challenges.bsf` does; (b) an overlay button that sets a flag the patched script reads. (a) needs only data files (BSF, IFC), no code; (b) needs a small hook.
3. **Missions beyond the retail one** (his `setSoloDestro(k)` idea): new `createGame(8, <mission>)` numbers need entries in the game's mission/arena tables; these are **script-and-data**, but the mission-number -> arena mapping lives in the scripts and bundles, so extra missions mean new arena scripts + city data (see "cities" in COLLAB_GODZILLA_ULTRA.md); keep retail rules for scoring and end conditions (the game defines them: points per building, goal, time).

## 4. Effort estimate
| Step | Effort | Notes |
|---|---|---|
| Prove the retail Demolition challenge in the recomp (scripted pad run, video, log, fix what breaks) | S (1-3 days) | test-only trigger = pad script through the Challenges menu |
| Menu row / "Extras" entry via a script mod, with credit and original behaviour untouched | S-M (2-4 days) | uses the mod patch system; 16:9/21:9 layout check |
| Retail-like extra missions (other cities / goals / time limits) using the same scripts with different parameters | M | data and script work; test per city |
| His tactical map panel / army queries | L | needs a code hook (his recipe 12) and city map textures; not required |
| A "solo destruction" mode with new rules (his Ultra version) | L | belongs with the Godzilla Ultra port (rebuilt cities, object limits) |

## 5. Not tested / open
- No game run was made for this report; the Demolition challenge has not been launched in the recomp yet. The menu path (list position of Demolition) needs a screenshot-guided pad script.
- Whether the Xbox Challenges list shows Demolition first/with its locked state is not confirmed.
- The meaning of his mission numbers (`createGame(6, 7700)`) is his; game type 6 is not a retail challenge type in the Xbox scripts I read (retail challenges use types 7/8 with missions 0..77).

## 6. Decision wanted from Miko
Approve step 1 (prove the retail Demolition challenge in the recomp, test-only trigger) and step 2 (a menu entry through the mod system) as the first, cheap version of "Solo Destruction"; the bigger Ultra-style mode waits for the Ultra port work.

## 7. Step 1 proven in game (2026-11-20, c432)
Retail Demolition challenge, reached through MAIN MENU > CHALLENGES (fifth entry: Battleships, Vorillium Basketball, Osaka Crunch, Monster Bowling, **Demolition**; all listed with "Unlock all challenges") with a scripted pad (`mission-l6\tests\pads\demolition_probe.pad.txt`, pad times are process-age seconds; run `F:\GodzillaSTE_work\soak\q1b\demo_run2`, screenshots every 8 s):
- Briefing/pause screen: "DEMOLITION: Destroy the city. Earn 15,000 destruction points before the timer runs out. Game tip: a club can be a useful tool for destruction." (Press START to continue).
- Play: San Francisco at night, Godzilla 2000, HUD with health bar, **countdown timer** (1:41 ... 0:06 in the sampled shots) and a **SCORE** counter (2175 -> 2450 -> 2775 -> 2825) with floating "325" points popups while buildings fall.
- End: at 0:00 the CHALLENGE RESULTS screen shows **FAILED!**, DESTRUCTION POINTS 2825, GOAL 15000, CONTINUE. (The scripted pad was a blunt button masher; a win needs 15,000.)
- No [ICALL FAIL] / [STUB HIT] in the log. So scoring, HUD, timer, end condition and results screen all work in the recomp. Not checked: the win path (>= 15,000: victory pose and PASSED text), the Continue flow back to the menu, 16:9/21:9 HUD placement of the Demolition surface.
Steps 2 and 3 (free "Solo Destruction": any monster, any city, timer options, menu row) need an edited script (`ChallengeDemolition.bsf` hard-codes `godzilla2k`, the level `ChallengeDemolition`, `setRoundTime(180.9)`), i.e. a BSF patch applied through the mod-patch system; next: BSF assembler/patch tooling and licence check.

## 8. Free "Solo Destruction" built (2026-11-20, 0.3)
Done as a patch mod (menu row, any monster, any city, Rules ROUND LENGTH 1-10 MIN plus NO TIME LIMIT, own solo select screen without COM): see `docs\formats\solo_destruction.md` for the design, the flag trick and the proof, `tools\mods\build_solo_destruction.py` to build it from the user's own files. Credit: "Based on ANILORAK's Godzilla Ultra research." Open: how the launcher builds it (needs GZBuildr at Prepare time, or the user runs the tool), 21:9 HUD check, win/score banner checks.
