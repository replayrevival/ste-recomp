<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Note for the launcher session: Biollante recipe mod (0.4, in progress)

What: a new recipe mod `ste.biollante` (built like Solo Destruction: manifest api 2, edit lists + hashes, no game data). Build on the player's PC: `python -I tools\mods\iso_to_mod.py prepare <mod folder> <game folder|.iso>`; the four targets are `ShellData\ControlRoom.bdl` (new format `builtin:bio_ui`), `ShellData\monsterselect.bsf`, `ActionData\ConventionalGame.bsf` (`bsf-edit`) and `ShellData\attributes.xfg` (new format `text-edit`). Prepare needs `pw_bundle.py`, `bio_ui.py`, `texlib.py`, `bsf_edit.py`, `step_diff.py` next to `iso_to_mod.py` (all stdlib, no third-party tool). Takes under 2 s.
Player text: with the mod on, the select screen has a third row below the two retail rows with Biollante; she is playable as P1/P2/COM in Versus and Melee. The grid is a little smaller (scaled to 84 %) so three rows fit.
Needs the c435 runtime (front-end detection fix); with c434 the HUD is stretched and the menus lose the 4:3 pillarbox when this mod is on. Do not ship the mod enabled by default.
Mod author tool: `tools\mods\build_biollante.py <game> <out mod folder>`. Format reference: docs\formats\roster_slots.md, docs\formats\mod_build_recipe.md.
