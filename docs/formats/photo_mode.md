<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Photo mode (0.3.1): a paused free camera

Opens during a fight (Versus, Melee, Survival, Action, Solo Destruction ...), freezes the simulation, lets you fly a free camera,
hide the HUD, zoom, roll and save full-resolution PNG screenshots, and closes back into the exact same match.

## For players
| | Controller | Keyboard / mouse |
|---|---|---|
| Open | overlay menu "Photo mode" (launcher overlay); optional chord **Back + Y** held 0.6 s (`photo_pad_chord = on`) | **F8** (`photo_key`) |
| Move | left stick (forward/back, strafe) | W A S D |
| Look | right stick | arrow keys, or mouse with a button held |
| Down / up | LT / RT | Q / E |
| Slow / fast | LB (x0.25) / RB (x4) | Ctrl (x0.25) / Shift (x4) |
| FOV wider / narrower | D-pad down / up | `-` / `+` (also `[` `]`) |
| Roll | D-pad left / right (both = level) | Z / X |
| HUD on/off | Y | H (`photo_hud_key`) |
| Screenshot | A | F12 (`photo_screenshot_key`) |
| Close | B | Esc or F8 |

Screenshots are full-resolution PNGs (the size of the game's swap chain, no overlay) in `<data>\screenshots\photo_<date>_<time>_<n>.png`.
The camera cannot leave a sphere of `photo_range` units (default 8000) around the place where you opened it.
While photo mode is open the game gets a released pad (like the overlay), the simulation (movement, AI, physics, timers, the match
clock, power-ups) is frozen, and the match continues from the same state when you close. The HUD clock picture keeps counting while the
HUD is visible and snaps back to the true time when you close (the match clock itself does not move).

## Settings (`ste_enhance.ini`, section `[camera]`)
| key | default | meaning |
|---|---|---|
| `photo_mode` | `on` | `off` removes photo mode (menu entry refused, hotkey ignored) |
| `photo_key` | `F8` | open/close key (`none` = no hotkey) |
| `photo_screenshot_key` | `F12` | screenshot key |
| `photo_hud_key` | `H` | HUD toggle key |
| `photo_speed` | `400` | flying speed, units per second |
| `photo_range` | `8000` | flying radius around the opening position |
| `photo_pad_chord` | `off` | `on` = Back + Y held 0.6 s opens photo mode |
The launcher's overlay opens it with `apply_setting("camera.open_photo_mode", "1")`.

## How it works (for modders and porters)
* **Freeze, exactly**: the generator hook mode `return` (docs\formats\RECOMP_HOOKS.md) makes `World_Update(dt)` (guest 0x00084800, `ret 4`)
  and the game-time accumulator `sub_000861C0(dt)` (`ret 4`) return at once while photo mode is open, and a `before` hook on
  `Game::Update(dt)` (0x00086F70) sets its dt argument to 0 for everything else it calls. The two `return` hooks are the only places where
  game code is skipped; they exist only while photo mode is open. All other generated functions are byte-identical with and without the
  hooks (checked by regenerating from `default.xbe`: 19 of 21 generated files hash-identical, the other two differ by exactly one inserted
  hook line each: see docs\REPORT_0.3.md / the 0.3.1 report).
* **Camera**: the fight camera manager `sub_001E9E60` passes its view matrix (12 floats, D3D layout: `p[i*3+j]` = world axis i, camera axis
  j right/up/forward; translation `p[9..11]`) to `sub_0001FA30(camera, p)` at 0x001EA1D8. A `before` hook replaces `p` with the free camera, so the
  camera object, its frustum and every camera-relative query (draw distance, query volume) follow it. The shared HUD/room camera
  0x00425EB0 is not touched. World up is detected from the camera's up vector (Tokyo: +z).
* **FOV** is the camera object's projection matrix (`camera+0x60`, built by `sub_001846B0`: `m[0]`, `m[5]` = focal terms); photo mode scales
  them and writes the originals back on exit.
* **HUD hide**: the Kelvin layer skips draws whose vertex-program constants map pixels to clip space with the 1/320 .. 1/160 scale in a 640x480
  viewport (`kelvin_is_hud`: c58 = (320,-240), |c96.x| and |c97.y| < 0.01). World draws have a real projection; particles and billboards have large
  terms.
* **Screenshot**: `d3d8_capture_backbuffer_bmp_public(path)` writes PNG when the path ends in `.png` (stored-deflate RGB PNG).
* Test hooks (not for players): `STE_PHOTO_SCRIPT=<file>` with `open n`, `close n`, `shot n`, `hud n 0|1`, `pose n dx dy dz yaw pitch`, `zoom n k`,
  `roll n deg`, `scan n`, counted in world-camera updates; `STE_PHOTO_PAD_CHORD=1`; `STE_PHOTO_SCAN=<s>` (which guest words keep changing).

## Not in 0.3.1
Live free cam while the match runs (v2, risks in docs\FEATURE_PHOTO_MODE.md section 4), freezing the HUD clock picture, saving the pose.
