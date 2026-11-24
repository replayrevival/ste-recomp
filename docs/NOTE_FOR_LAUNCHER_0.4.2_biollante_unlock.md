<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Note for the launcher session: Biollante recipe changed (0.4.2 work, not released)

`ste.biollante` now also makes her always selectable (one more edit in `edits\ShellData__monsterselect.bsf.json`; the manifest `result_sha256` and `size` of that target changed). Consequences:
- Anyone with prepared mods from 0.4.0/0.4.1 must run **Prepare** again (the built file's hash differs; the game then reports a hash mismatch and ignores the mod file otherwise).
- The bundled mod copy in the launcher package must be refreshed from `ste\mods\ste.biollante`.
- Nothing else for the user: no new setting. Mod off = base game unchanged.
