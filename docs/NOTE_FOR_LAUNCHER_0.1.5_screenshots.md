<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Note for the launcher session: 0.1.5 showcase screenshots (c379)

Committed locally in publish\ste-recomp, docs/screenshots/ (not pushed). The 0.1.4 game shots moved to docs/screenshots/archive/0.1.4/ (the launcher shots were already archived there). All are clean backbuffer captures from the c379 build (packaged layout with texture packs, HUD on, no probes, no profile names), PNG, max 2560 wide. Rendered captures only, no game files. The overlay shots come from the game's own presented frame (STE_OVERLAY_SHOT), so they include the overlay.

| File | Size | Caption |
|---|---|---|
| **0.1.5_hero_ultrawide_fight_tokyo.png** (HERO) | 2560x1072 (from 3440x1440, 21:9) | Godzilla 2000's atomic breath crosses a Tokyo canal toward Gigan at 21:9 ultrawide, Unlimited draw distance. |
| 0.1.5_ultrawide_beam_fight_tokyo.png | 2560x1072 | A long beam crosses the wide view between the two fighters. |
| 0.1.5_ultrawide_building_collapse.png | 2560x1072 | A building explodes next to Godzilla 2000 at 21:9. |
| 0.1.5_ultrawide_tokyo_skyline_unlimited.png | 2560x1072 | Unlimited draw distance: the skyline runs to the hills and the water. |
| 0.1.5_16x9_fight_tokyo.png | 1920x1080 | A fight at plain 16:9 with the original HUD. |
| 0.1.5_button_prompts_xbox.png | 1920x1080 | Versus screen with Xbox button pictures (test switches unlocked all fighters). |
| 0.1.5_button_prompts_playstation.png | 1920x1080 | The same screen with PlayStation button pictures (square, circle, cross). |
| 0.1.5_press_menu_wording_playstation.png | 1920x1080 | New in 0.1.5: the on-screen text follows the button style ("Press MENU to add COM" instead of "Press START"). |
| 0.1.5_title_press_menu.png | 1280x720 | New in 0.1.5: the title screen says PRESS MENU (original text with `button_prompts = xbox_original`: PRESS START). |
| 0.1.5_overlay_paused.png | 1920x1061 | New in 0.1.5: the in-game overlay (Back+Start, F1 or Guide): the game is paused behind it. |
| 0.1.5_overlay_video.png | 1920x1061 | The overlay's Video page: display, resolution, draw distance and more. |
| 0.1.5_overlay_controllers.png | 1920x1061 | The overlay's Controllers page: button prompts, port, menu confirm/back swap. |

Notes: the launcher's own shots (0.1.5_launcher_*) are yours. The overlay shots sit on the title screen (a fresh scripted run), not on a fight, and are 1920x1061 as captured (the presented frame of that run). Release-set gaps against docs\RELEASE_CHECKLIST.md: no 32:9 or 4K set this time (the older ste_*_fight_hud.jpg compare shots are unchanged), no beta HD vs original side-by-side (no HD art yet).
