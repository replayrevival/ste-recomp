<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Plan 0.1.5 (approved 2026-11-14)
Moved from 0.1.4: overlay, button-word/PC-wording text re-wording, keyboard name entry, effects sweep. Missiles (item D) stay a known issue until solved.
1. **Text lookup hook** (button words per style, PC wording): the game finds UI text by the English string in parallel per-language tables, so the files must not be edited (see REPORT_0.1.4.md). Find the lookup/draw function (generate-and-splice probe like c372), hook it and substitute at use time; original text when `button_prompts = xbox_original`.
2. **Keyboard name entry** on the profile name screen and other on-screen keyboards (SDL text input, Backspace/Enter/Esc, max length and charset).
3. **Overlay**: the launcher fixed its crash (stale ste_overlay.lib) and now ships `ste_overlay.dll` with a plain C interface (launcher\include\ste_overlay_c.h, header-only loader). The glue becomes a small C file that loads the DLL (the game exe keeps needing only VCRUNTIME140.dll); confirm it loads in the game process, then wire show/hide, input, pause, exit code 20, apply levels, draw before Present.
4. **Effects sweep** of every monster's attacks (table in the report; fix what is quick).
5. **Item D** (Mechagodzilla 3 missiles): rocket and trail instances are alive after the spawn; find why the head sprite never reaches a draw (c372 dumps, F:\GodzillaSTE_work\d372_ram3).
