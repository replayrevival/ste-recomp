<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# 0.4.2 work log

## Biollante always unlocked (done, local)
Cause of the "dim blue tile" seen on a profile without "unlock all monsters": the select script draws a roster slot as disabled when the profile has no unlocked flag for that monster (see `docs\formats\roster_slots.md`, "Always available"). Biollante has no flag on any profile. Fix: one token edit in the Biollante recipe (`ShellData\monsterselect.bsf`), manifest hash and size updated (`result_sha256` `4f270ad6...`, 80576 bytes alone; the Solo stack on top records its own hash in `_stack.json`).
Test (fresh profile, no unlock-all, both mods stacked, 1920x1080): the third-row tile shows her colour icon on the Solo and Action screens; Up from the grid reaches her, Select makes P1 = BIOLLANTE, then City Select. Mod off: no change (the edit lives in the mod).

## Waiting for the reporter
Black attract movie (Fullscreen only, or Borderless too) and the menu-side globe (stays, or only during the Back transition): no changes until the answers arrive.
