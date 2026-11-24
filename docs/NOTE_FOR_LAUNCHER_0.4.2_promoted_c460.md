<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# NOTE FOR THE LAUNCHER SESSION: 0.4.2 = c460 (Biollante always available) - NOT published, waiting for the maintainer
Exe: the c460 build artifact, SHA-256 `AC4EDB6F85EE7A16BFCB1163469CF9503AA6E85B3B7FE9B27306E7FAAD60986B`, version resource 0.4.2 / 0.4.2+c460. Validator: no FAIL. It is c441 plus the opt-in online research code (inactive unless an environment variable is set; default behaviour unchanged) and no gameplay change; the player-visible change is in the mod.

## What changed for players
1. With the Biollante mod on, her tile is drawn with her icon and selectable on every profile (it was a dark silhouette on profiles that had not unlocked her). Base game unchanged with the mod off.
2. Mod `ste.biollante` is now **1.0.1** (manifest hash and size of `monsterselect.bsf` changed). Existing users must **Prepare mods again** ("re-run Prepare mods after updating"); until they do, the game reports a hash mismatch for that file and ignores it.
Release text: `docs\RELEASE_ALPHA.md` "0.4.2"; README "What's new in v0.4.2".

## What the launcher must do
1. Ship the updated `ste\mods\ste.biollante\` (manifest 1.0.1 + `edits\ShellData__monsterselect.bsf.json` with one more edit). The launcher's version-based mod refresh should pick up 1.0.1 (as for Solo Destruction in 0.4.1); keep the user's on/off choices.
2. Smoke test on a FRESH profile **without** "unlock all monsters": both mods on (prepare-stack), Solo screen and Versus screen show Biollante's coloured icon in the third row; Up to her + Select makes P1 = BIOLLANTE; then each mod alone; then none (base game unchanged).
3. Host/Join screens (online, not released yet): add a line "Other players will see your profile name" (the System Link advertisement the game broadcasts carries the profile name in plain text).
Media: reuse the 0.4.0 media. Machine: my game is not running.
