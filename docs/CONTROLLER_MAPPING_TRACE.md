<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Controller mapping: from the physical button to the game's action (trace and regression test, c377)

Report that started this (0.1.3, an Xbox One pad): "pressing X does A's action and vice versa". This page is the full path of a button press and the test that guards it. Result of the trace: **no defect was found in the game's own mapping chain**; the chain is covered by a test, and the log now says which binding is in effect so a report can be answered from one line.

## The chain
1. **SDL3 (positional).** `SDL_GAMEPAD_BUTTON_SOUTH` = A (bottom), `EAST` = B, `WEST` = X, `NORTH` = Y, `LEFT_SHOULDER` = LB, `RIGHT_SHOULDER` = RB, triggers are axes. SDL names them "a", "b", "x", "y", "leftshoulder", "rightshoulder", "lefttrigger", "righttrigger", "start", "back", "leftstick", "rightstick", "dpup" ... (`SDL_GetGamepadStringForButton`). SDL's own mapping for the Xbox One pad GUID in question (`0300ba665e040000ff02000000007801`) is `a:b0,b:b1,x:b2,y:b3,leftshoulder:b4,rightshoulder:b5,back:b6,start:b7,...` (printed by the self-test with `STE_INPUT_SELFTEST_GUID`): standard.
2. **Binding table** (`ste_sdlinput.c`, `k_def` / `src_parse`): action `a` <- SDL "a" (SOUTH), `b` <- "b" (EAST), `x` <- "x" (WEST), `y` <- "y" (NORTH), Black <- leftshoulder, White <- rightshoulder, left/right trigger <- the trigger axes, start/back/stick clicks/D-pad as named. The same names are what the launcher writes (`config\controls.json` defaults, `input_mapping.json` `bindings`): checked, identity (`"a":"a"`, `"x":"x"` ...) in the launcher's default file.
3. **Xbox pad state** (`ste_sdl_get_state`): analog slots `bAnalogButtons[0..7]` = A, B, X, Y, Black, White, LeftTrigger, RightTrigger; digital bits in `wButtons` (D-pad 0x01..0x08, Start 0x10, Back 0x20, left/right stick click 0x40/0x80). These are the XDK's own indices (`XINPUT_GAMEPAD_A` = 0 ... `XINPUT_GAMEPAD_RIGHT_TRIGGER` = 7). The XInput fallback (`xinput_device.c`) uses the same constants.
4. **Guest**: `XInputGetState` copies the 22-byte state into the title's `XINPUT_STATE` (`recomp_manual.c`, `memcpy(GPTR(ps), &st, 22)`). The game then reads slot 0 as A, slot 2 as X.

## The regression test (`tests\input_selftest.ps1`, exe flag `STE_INPUT_SELFTEST=1`)
Runs the packaged exe, attaches a **virtual SDL Xbox One pad** (standard layout), presses every control one at a time and checks that exactly the expected game button/bit is set and nothing else: A, B, X, Y, Black (left shoulder), White (right shoulder), Start, Back, both stick clicks, the four D-pad directions, both triggers. It prints one `[INPUTTEST]` line per control and `RESULT PASS|FAIL`; no game files are read. Three cases:
1. built-in defaults must PASS;
2. the launcher's default `input_mapping.json` must PASS (skipped when the launcher is not on the PC);
3. a mapping with **A and X swapped** (`tests\data\input_mapping_swapped_ax.json`) must FAIL and must log `CUSTOM face-button binding`: proof that the test would catch the reported bug.
Result on c377: 1 PASS, 2 PASS, 3 FAIL as intended (2 failures: A and X).

## What the log says now
At the first poll of a pad: `[INPUT] port 1 pad slot 0 bindings: A<-SDL 'a' B<-SDL 'b' X<-SDL 'x' Y<-SDL 'y' Black<-SDL 'leftshoulder' White<-SDL 'rightshoulder'`. If A/B/X/Y are not on their own positions: `[INPUT] NOTE: port 1 has a CUSTOM face-button binding ... it comes from the mapping file (remapped in the launcher or overlay), not from the game`. The first press of each game button logs `[INPUT] port 1: game button X first pressed (bound to SDL 'x')`.

## What this means for the report
Everything the game controls is positional and was verified. A swap like the reported one can only come from outside the game's chain: (a) a custom binding in `input_mapping.json` (the launcher's remap swaps two actions when a source that another action uses is bound, `input_map.h`: "the two swap"), which the new `NOTE` line makes visible; (b) a remap below the game: the Xbox Accessories app, Steam Input, or another tool that remaps the pad before SDL sees it; (c) a pad whose driver reports its buttons in a non-standard order (SDL's mapping string for the model, printed by the self-test, shows it). A log of the session (the `bindings:` line and the `first pressed` lines) tells which in one look.
