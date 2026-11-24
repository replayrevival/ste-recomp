<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Note for the launcher session: 0.4.3 work, Biollante grid layout fix (not released)
`ste.biollante` is now **1.0.2**: the third-row grid is smaller (scale 0.70) and sits under the portrait cards / bios panel, so it no longer overlaps them (reported on the Solo Destruction screen). Manifest hash and size of `ShellData\ControlRoom.bdl` changed.
- Ship the updated `ste\mods\ste.biollante\` (manifest 1.0.2 + `ste\tools\mods\bio_ui.py` with the new constants); the version refresh should offer it as an update (as for 1.0.1).
- Users must **Prepare mods again** after the update. The exe is unchanged (c460 works with it).
- Smoke: both mods on (prepare-stack): Action, Versus, Melee, Survival, Solo Destruction screens: the tile grid must not touch the portrait card, the bios panel or the Back/Select hints, at 16:9 and 21:9.
