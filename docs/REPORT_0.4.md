<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Report 0.4 (Biollante): scoping and decisions, 2026-11-21 (work not finished; this is the plan and what was found)

## What exists
Retail disc has all her files (docs\FEATURE_BIOLLANTE.md). The earlier pack `biollante_slot` (whole-file api 1 overrides, built by `tools\mods\build_biollante_ui.py` from the retail `ShellData\ControlRoom.bdl`) + `unlock_biollante` gives P1 and COM in Versus, Melee and a Seattle match, results screens, HUD and sounds.

## Findings today
1. **The shipping form is the real work.** `build_biollante_ui.py` (and bio_extract.py, bio_tiles.py) use a third-party library (GZBuildr) to read and rebuild the Pipeworks bundle, plus ad-hoc post-patches (text entry sizes, the nine pool records, appended tile resources, names appended at the end of the sorted name table with the `Zz_` prefix because inserting names in the middle crashed the title). By the 2026-11-20 policy a third-party tool may not be needed at install time, and the result contains art derived from the player's own tiles, so it can only be shipped as a *recipe* built on the player's PC (like Solo Destruction, `docs\formats\mod_build_recipe.md`). Needed: our own Pipeworks bundle library (`pw_bundle.py`: parse, extract, repack with insert/replace, name-table and pool-record maintenance; first test = byte-identical round trip of ControlRoom.bdl and the character bundles), then a `bundle-edit` recipe format (clone entry A as B with new name, replace text entry with edit list, set generated resources) and the Biollante recipe on top. The format notes and the crash lessons are in docs\formats\pipeworks_bundle.md ("Editing rules"). Estimate: library + round trip 1-2 days, recipe and in-game re-test matrix 1-2 days.
2. **Human P2 is probably no longer blocked.** The earlier statement "the game only opens port 0" predates the SDL multi-pad work (c248+): `recomp_manual.c sub_002D8D7E` already maps up to four ports (`g_port_src`, virtual ports for scripted pads). To check first (no code change expected): a Versus match with a virtual pad on port 1 picking Biollante. If it works the "controller port 0 only" blocker is stale; if the select screen only offers P2 a controller that is not mapped, the fix is in our input layer (not game logic), which is within the rules. I did not run this yet.
3. **Action mode**: the Action monster list is in the select script `ShellData\monsterselect.bsf` (the one-player branch used by Solo Destruction is the same layout); our `bsf_edit.py` can add her entry; `AMBiollante.bsf` exists. Not started.

## Decisions
- Biollante ships as a recipe mod (like Solo Destruction), built on the player's PC from the player's own disc; GZBuildr stays an offline cross-check only.
- No gameplay-behaviour change is planned for the roster work itself (new slot, same scripts). Anything that alters fight rules for her (AI, damage) is out of scope unless it is a retail script bug, and would be reported first.

## Next steps (order)
1. P2 check (Versus with a port-1 virtual pad), 1 run.
2. `tools\mods\pw_bundle.py` + round-trip test; document in docs\formats\pipeworks_bundle.md (own reading, credit Digitzaki's GZBuildr only as the original reference for the format notes).
3. `bundle-edit` recipe entry + Biollante recipe; Prepare-mods integration (same launcher note pattern).
4. Action mode entry; effects check of her attacks; short clip when the launcher asks.
5. The generic image-to-mod item (already shipped as `iso_to_mod.py`; the guide for MODDING.md still to write).

## Progress 2026-11-21
- Step 2 done: `tools\mods\pw_bundle.py` + `test_pw_bundle.py`. Round trip byte-identical on all 99 retail `.bdl` files; synthetic edit/add/name-order tests pass. Format note updated. Own implementation (no GZBuildr code). Add-entry in game not yet tested.
- Step 1 (human P2 check) waiting: launcher had the game busy for 0.3.1 smoke tests.
