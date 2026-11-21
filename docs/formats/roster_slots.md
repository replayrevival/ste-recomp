<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Extra character-select slots (roster recipes), 0.4

Used by the Biollante recipe mod (`tools\mods\build_biollante.py`, `bio_ui.py`). Nothing here changes the retail screen: with the mod off the grid is the original 2 rows x 9.

## Layout
- The select grid is `Thumbnails` in `ShellData\MonsterSelect.ifc` (inside `ControlRoom.bdl`): objects `Row0`, `Row1` (y = 308 / 365 in a 640x480 menu space, children at x = 76 + 55 * column, 52 px tiles). The select script (`ShellData\monsterselect.bsf`) keeps the rows in `Thumbnails` and all navigation, the P1..P4 cursors and the "slot to monster" lookup work on `Thumbnails.size()` and the child names, so a monster is identified by the child's object name (the monster's script name, e.g. `biollante`).
- A third row does not fit at full size (the menu frame ends at y = 444, the Back/Select buttons sit at y = 430). The mod adds `Row2` (origin y = 422) and scales the whole grid with `<scale>0.84</scale>` and a new position (31, 21) on the `Thumbnails` object (tiles 43 px, rows end above the buttons; the cursor frames are children of the same object, so they scale with it).
- Script edits (bsf-edit, three lines): `Thumbnails.resize(3)`, `Thumbnails[2] = nails.findObject("Row2")`, plus the `biollante` entry in `lockedInMonsterPkg` (she is not behind a monster package). Navigation wraps over three rows; a row with fewer tiles snaps the column to 0 when the cursor moves into it.
- Known limit: `lookupThumbnail` (the "preselect the previous monster" search) keeps scanning rows 0..1. Editing its loop bound made the script fail to initialise in game (bisect 2026-11-21: any edit of that loop, even `row < Thumbnails.size()`, shows the default Melee layout), so a monster that lives in an extra row is not preselected when the screen reopens.

## Slot table (`bio_ui.SLOTS`)
One dict per extra monster: `name` (script name), `row` (2 = the extra row; rows 0/1 are the retail rows and are not edited), `art` (entry in `ControlRoom.bdl` holding the full-size portrait the four 52x52 tiles are generated from) and `crop` (x, y, size of the head in that art). For each slot the builder generates `Zz_dis_<Name>`, `Zz_en_<Name>`, `Zz_foc_<Name>`, `Zz_sel_<Name>` (tiles, 128-byte aligned resources, cloned from the Anguirus tiles' headers) and `Zz_Flash_<Name>.ifl`, clones the Moguera 2 child under the new name and centres the row. New names must sort after every existing name (see pipeworks_bundle.md); the `Zz_` prefix does that and the builder adds them in sorted order. Adding a monster later = one more row in the table (plus its roster, HUD and sound data).

## Front-end state word
The game-state word `0x003562B4` is `0x27` in the front-end of the retail game and `0x28` once the grid has a 19th child. The widescreen code (`d3d8_front_end_active()` and the camera hook) accepts `0x27..0x3F` since build c435; before that the roster mod lost the 4:3 menu pillarbox and stretched the fight HUD (docs\REPORT_0.4.md).
