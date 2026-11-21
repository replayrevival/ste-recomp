<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Assessment: Biollante as a playable monster (restored from unused retail content) - 2026-11-20, report only

## 1. What is on the retail Xbox disc (read-only listing of the user's own files)
`bundle\Biollante.cmf` + `Biollante.zip` (model/animation bundle), `bundle\hudbiollante.bdl` (HUD), `SoundData\Biollante.wxv` (voice/music stream), `ActionData\AMBiollante.bsf` (Action-mode script) and `ActionData\SMBiollante.bsf` (Versus/Melee script). Everything the game needs to run her as a monster exists; what retail lacks is the roster entry (the select grid, its bitmap names and the unlock). So the work is script + roster data, not new assets.

## 2. What already works (earlier rounds, c124-c195; evidence in .agent-state\CHECKPOINT.md, Biollante rows)
- The pack `biollante_slot` (api 1 whole-file overrides: the select-screen bundle `ControlRoom.bdl` with a 10th cell, names appended at the end of the sorted name table with the `Zz_` prefix, flash/portrait bitmaps) plus the ini switch `unlock_biollante` (`[enhance]`, shell bit 0x002 at 0x42D310) give: Biollante selectable as P1 and as COM (AI active), Versus in Tokyo and Seattle, Melee FFA with two COM and with three COM at 4 participants, results screen, HUD name, no crashes in 440-540 s fights; the white-rectangle polish bug was fixed in the pack.
- Cheats and the expanded save profile do not touch the original save (seeded copy tests).

## 3. What is still open
| item | status |
|---|---|
| human P2 | blocked by the single XInput port (the game only opens port 0): same blocker as any 2-pad play; needs multi-pad XAPI support (separate approval) |
| Action mode (AMBiollante.bsf) as a campaign | not tested: the script exists but nothing in the retail Action flow selects her; would need a roster/script entry in the Action monster list |
| Melee 4 participants, empty-profile flow | not done |
| intro/outro and per-monster banners | not checked (HUD and the arena intro show the name only) |
| shipping form | the pack uses whole-file overrides of game files (ControlRoom.bdl etc.), which breaks the rule "mods never ship game data" for the release: it must become a patch mod (manifest api 2, `tools\mods\iso_to_mod.py make` from a modified copy of the user's own files, base = retail `ControlRoom.bdl`/name tables, hash-verified), or a recipe tool like `build_solo_destruction.py` that edits the user's own files locally |

## 4. Plan
1. Re-express `biollante_slot` as a patch mod built locally from the user's files (the pack builder `tools\mods\build_biollante_ui.py` already produces the modified `ControlRoom.bdl` from the original; use `iso_to_mod.py make` on the result), keep `unlock_biollante` as the toggle (off by default).
2. Action-mode entry (optional): add her to the Action monster list in the select script; test the AM script end to end.
3. Multi-pad support is a separate item (needed for human P2 for every mode).
4. Credits: CREDITS.md lists DylanRocket's Xbox Biollante tutorial for the restoration technique; confirm wording before any release text.
Effort: step 1 S-M (1-2 days incl. test matrix rerun), step 2 M, step 3 L (separate).
