<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Replay Revival - Godzilla: Save the Earth (Xbox) recomp - ALPHA 0.1.5a (draft)

**Early alpha. Work in progress, development continues.** Not affiliated with or endorsed by Toho, Atari or Pipeworks. You must own the game; no game data is included or distributed. Windows only for now.
Draft prepared 2026-11-10, 0.1.4 section added 2026-11-14, 0.1.5 section 2026-11-15. Nothing here is published; the owner publishes.

**Credit:** the "G:STEM-inspired options" (Monster size, Footstep camera shake, Rage meter, More traffic, No juggle limit, No knockback scaling, No minimum health, Weapon-independent vertical knockback, No results-screen dimming, Empty arena) are based on G:STEM's gameplay options: credit to Gojirex, Shame Devs, GR8GODZILLAGOD and the STEM community (docs\CREDITS_AND_SOURCES.md, docs\community\STEM_TOGGLES.md). Each is re-created from the STEM patch notes as a separate toggle, default off.

## 0.3 (draft, in progress): Solo Destruction, photo mode, Biollante assessment
| Feature | What | Status |
|---|---|---|
| Solo Destruction | a patch mod (`ste.solo-destruction`, built locally from the player's own scripts with `tools\modsuild_solo_destruction.py`): main menu row SOLO DESTRUCTION runs the retail Demolition challenge on any monster and city, own select screen (no COM step), Rules ROUND LENGTH 1-10 MIN and NO TIME LIMIT. Heading GODZILLA ULTRA FEATURES, "Inspired by ANILORAK's Godzilla Ultra research (our own code; no code from ANILORAK used). Full credits: https://github.com/replayrevival/ste-recomp/blob/main/CREDITS.md" Docs: docsormats\solo_destruction.md | built and verified in game (Tokyo, Osaka North, results screen); 16:9 and 21:9 checked |
| Photo mode | free camera with the game paused | not started |
| Biollante assessment | docs\FEATURE_BIOLLANTE.md | not started |

## 0.2.3 (draft, 2026-11-20): sound packs
| Area | Change | Status |
|---|---|---|
| Sound packs (audio mods) | a mod with `"profile": "sounds"` replaces sounds of the game: effects, announcer/voice lines, monster jingles and music tracks, from plain WAV files (PCM 16-bit, mono or stereo, any rate). Installed from the launcher's Mods tab; follows the on/off switch and order like every mod; `last_report.txt` says whether each pack is active and why a file was refused. No pack = nothing changes | in (proved: replaced announcer cues and fight music in a Tokyo fight; synthetic tones) |
| Export / build tools | `tools\mods\sound_pack.py export <game folder or .iso> <out> [--match WORDS] [--category effects|voices|music|monsters] [--list]` writes every sound as a named WAV; `import <WAV folder> <game> <pack folder>` checks the WAVs and builds the pack (matched / unmatched / refused / notes); only Python 3.9+ is needed | in |
| Loops | end a file name with `_loop` or give the WAV a `smpl` loop (Audacity does not write one); the loop flag is written, in-game looping of replaced sounds is NOT verified | partly (untested in game) |
| RAGE announcer | traced: the game plays its `RAGE` cue (voice bank slot 28) on a real rage pickup, at the level of the other announcer lines; nothing to fix found; what it sounds like is for the listening check (export the line with `--match RAGE`) | investigated, no change |
| Tokyo UFO, Unlimited pop-in, ANILORAK HD sample | not resolved / not delivered in 0.2.3 (details in docs\REPORT_0.2.md) | open |
| Version | exe resource 0.2.3 (candidate c432) | in |
Changes text for the release page: "Sound packs: replace the game's effects, announcer lines and music with your own WAV files, build the pack with one command and install it from the Mods tab."

## 0.2.2 (draft, 2026-11-19): retexture path
| Area | Change | Status |
|---|---|---|
| Export original textures | `import_texture_set.py export <game folder or .iso> <out> --monster Gigan` (also `--match`, `--list`, `--monster ?`): every texture as a named RGBA PNG plus `export_report.csv`; DXT1/3/5, A8R8G8B8, 16-bit, P8 and the linear formats are decoded and de-swizzled. Needs Python 3.9+ and Pillow | in |
| Import in one step | `import <edits> <game folder or .iso> <pack folder>` builds the texture list itself (about 10 s); prints the next step | in |
| Texture packs from the Mods tab | a texture pack installed as a mod (`"profile": "textures"`) is loaded from the mods folder and follows the Mods tab's enable switch and order; `last_report.txt` and the log say whether each pack is active. The `texturepacks` folder next to the exe keeps working | in (proved: checkerboard on Godzilla 2000 on/off in a Tokyo fight) |
| Docs | docs\formats\texturepack.md, texturepack_import.md; launcher note docs\NOTE_FOR_LAUNCHER_0.2.2_retexture.md | in |
| Version | exe resource says 0.2.2 (candidate c431) | in |
Not in 0.2.2 (moved to 0.2.3): the RAGE announcer line, the Tokyo UFO, Unlimited pop-in, ANILORAK's HD sample test, sound packs.
Changes text for the release page: "Retexture a monster: export the game's own textures as named PNGs from your game folder or disc image, edit them, build a pack in one command, install it from the launcher's Mods tab. Texture packs now follow the Mods tab's on/off switch and order."

## 0.2.1 (draft, 2026-11-19)
| Area | Change | Status |
|---|---|---|
| Keyboard name entry | the name you type on the profile name screen appears at once (about a third of a second after the first key), up to the game's own limit of 10 characters; Backspace works; the old walking method stays as `STE_TEXT_ENTRY=walk` | in |
| Rage announcer line | investigated: the announcer voice plays after a monster enters rage in our build; the reported missing "RAGE" line was not reproduced (human-player pickup not tested) | known issue |
| Tokyo UFO out of bounds, Unlimited pop-in | not resolved / not verified (see REPORT_0.2) | known issue |
| Version | exe resource and `--version` say 0.2.1 | in |

## 0.2.0 (draft, 2026-11-19)
| Area | Change | Status |
|---|---|---|
| Play from a disc image | the game mounts an Xbox disc image (.iso / .xiso, plain or redump-style with a video partition in front) read-only; the launcher passes the image path (capability `disc_image`) so no extraction step is needed. Patch mods still need a folder | in (tested with built images; a real redump is untested) |
| Keyboard name entry | about 3.5x faster than 0.1.7 (a name in about 2 s); not instant | partly |
| Low-end preset | target and settings documented (docs/LOW_END_PRESET.md); CPU side measured, weak-GPU side estimated only | partly |
| Version | exe resource and `--version` say 0.2.0 | in |

Not in this release: the stuck Tokyo UFO, the missing "RAGE" announcer line, verified pop-free Unlimited clips.

## 0.1.8 (draft, 2026-11-19)
| Area | Change | Status |
|---|---|---|
| Fullscreen | the "Fullscreen" video mode is now borderless fullscreen (flip-model swap chain on a borderless window over the monitor); true exclusive fullscreen is `mode=exclusive`, advanced and opt-in. Root cause of the menu freeze in true fullscreen found and fixed: the menu was only redrawn and read when a new game frame arrived, and the game is held while the menu is open (affects every non-flip swap chain). Tested: menu open, close, Alt+Tab, V-Sync on/off, a 21:9-sized window, exclusive | in |
| Menu with a controller | the in-game menu now follows the controller: D-pad / left stick (with hold-repeat), A confirm, B back, LB/RB tabs, Start closes. Cause: the pad state was only refreshed by the game, which is paused behind the menu. Needs the matching menu library (launcher) | in |
| Keyboard name entry | about twice as fast (6 frames per key press instead of 12); not instant: the game's own routine could not be reached without overriding game logic | partly |
| Draw distance | the showcase clips had been recorded with the query volume not widened (x1), which is the buildings popping in; the load governor no longer shrinks the skyline below x3 (`query_scale_floor`) | in |
| Out-of-bounds UFO (Tokyo), missing "RAGE" announcer | investigated, not fixed (findings in REPORT_0.1.8) | slipped |
| Version | exe resource and `--version` say 0.1.8 | in |

Known: the test PC B fullscreen freeze is unconfirmed (no evidence arrived, not reproduced); x4 Unlimited cannot be recorded at 1080p60 on the test PC (x2 clip made).

## 0.1.7 (draft, 2026-11-19)
| Area | Change | Status |
|---|---|---|
| Disc images | `--game <file.iso>` (or `STE_GAME_PATH`) mounts an Xbox disc image (XISO / redump-style) READ-ONLY: nothing is unpacked or written, the game files are read straight out of the image (intro movies, menus and a full fight tested). Folders still work | in (tested with an image built from an extracted copy; not yet with a real redump image) |
| PlayStation wording | where "PRESS OPTIONS" does not fit a string, the PlayStation style now keeps the original word START (was MENU); the Xbox style still says MENU | in |
| Cheats from the in-game menu | the five in-match cheat toggles apply immediately when switched in the menu (before: next match) | in (game side; the menu page is the launcher's) |
| Recording | `STE_RECORD` reworked: GPU ring + worker thread, one video frame per presented frame at a constant rate, x264 ultrafast by default (NVENC next to the game halved the frame rate on the test PC), dropped-frame and frame-time counters in the log; `RECOMP_AUDIO_WAV_WALL=1` keeps the WAV on the wall clock (menu pauses, loads); `STE_RAND_SEED` (harness) | in, opt-in |
| Texture inventory | `STE_TEXINV=1` writes a CSV of which textures a session samples (hashes, sizes, draw and vertex counts, no pixels): first step of the recreated-art pilot (docs\TOKYO_ART_INVENTORY.md) | in, opt-in |
| Effects sweeps | 4:3 aspect sweep of all 18 monsters and second pass of the gaps: no new defect | done |
| Version | exe resource and `--version` say 0.1.7 | in |
| Low preset | 1080p60 held on 4C/8T (p99 about 19 ms) and mostly on 2C/4T here; weak GPUs not measurable on the test PC | measured |

Known: Mechagodzilla 2 has no finger-rocket attack that could be triggered from the pad in 24 button combinations (its jet-pack, laser and sparks draw fine); two recordings of the same pad script are never identical (the fight runs on the real frame time).

## 0.1.6 (draft, 2026-11-18)
| Area | Change | Status |
|---|---|---|
| Mechagodzilla missiles | the Rocket Pack missiles of Mechagodzilla 3 now show their white heads and grey smoke trails in flight. Cause: the title's point sprites (heads, trails, many smoke puffs) sample their texture from texture stage 3, and the renderer only wrote the sprite's texture coordinate into stage 0, so every sprite pixel read the transparent corner of its texture. The coordinate is now written to all four stages (what the NV2A does). Every point-sprite effect benefits | in |
| Keyboard name entry | on the profile name screen (and any screen with the same on-screen keyboard) the PC keyboard types: letters, digits and Space are turned into the D-pad and A presses a player would make, by reading where the highlighted key is on screen; Tab = END. Backspace, Enter and the arrows keep their pad-mapping meaning. Outside that screen the keyboard behaves as before. `STE_TEXT_ENTRY=0` turns it off | in |
| Recording for trailers | opt-in `STE_RECORD=<file.mkv>` records the presented frames (no overlay) through ffmpeg at a constant frame rate without changing game timing; `RECOMP_AUDIO_WAV` records the game's own mix. Docs: RECORDING.md. Off by default | in, opt-in |
| Stall log | every frame that arrives more than 250 ms late is logged as `[STALLFRAME]` (frame, delay, game mode, output size, queue state) to help catch the rare stall seen once in a 4K water stage | in |
| Version | exe resource and `--version` say 0.1.6 | to do at release prep |

## 0.1.5a: hotfix for the in-game menu freeze (replaces 0.1.5)
**Do not publish plain 0.1.5.** On the maintainer's other PC (true fullscreen, V-Sync on) opening the in-game menu froze the game: the window greyed out, the close button and Alt+F4 did nothing, the pointer stayed invisible. Cause: the game window belongs to the render thread, which only handled window messages while it was drawing game frames. The menu pauses the game, no frames arrive, and the window stopped answering Windows for good (true fullscreen additionally makes the display driver wait for that window). Reproduced on the test PC A (window hung for the whole test, close request ignored), fixed, and checked again.
| Area | Change | Status |
|---|---|---|
| Window while the game is paused or stalled | the render thread keeps handling window messages while it waits for work, and the menu's pause wait does too: the window never greys out, the close button and Alt+F4 work with the menu open, a long load no longer makes the window "not responding" | fixed |
| Pointer | forced visible while the menu is open | in |
| Hang watchdog | no game frame for 3 s while the menu is NOT open: `[HANG]` line in the log, pointer restored (class cursor, clip rectangle, hide count), every thread's stack and a minidump in the dumps folder (max 3 per run). `STE_HANG_SECS` (default 3, 0 = off). The old 120 s watchdog stays | new |
| Guide button | no longer opens the menu by default (opt-in: `[input] overlay_guide = 1`); Back+Start (held 1 s) and F1 stay | changed |
| Checked | window mode x V-Sync x VRR at 16:9 plus 21:9 in all three window modes (15 runs): menu opens, window answers for 12 s, a close request ends the game with exit code 0; true fullscreen with the menu held open 60 s; Alt+Tab out and back with the menu open (window kept answering) | PASS on the test PC A |
| Not checked here | the owner's own setup (true fullscreen at a high refresh rate, a real pad's Guide button): the maintainer re-tests before anything is published | open |

Interim workaround for anyone on plain 0.1.5: rename `ste_overlay.dll` next to the game to turn the menu off.

## What changed since 0.1.4
| Area | Change | Status |
|---|---|---|
| In-game overlay | press **Back+Start for one second** (or F1, or the Guide button) to open the launcher's settings over the game: Video, Enhancements, Controllers (incl. remapping), Return to launcher, Quit. The game is paused while it is open and nothing reaches it; pad, keyboard and mouse work. Return to launcher exits with code 20. It needs `ste_overlay.dll` (with its `ui`, `assets` and `config` folders) next to the game, which the launcher package provides; without it the game runs as before. `master_volume` and `hide_cursor` apply at once. | in |
| Button words and PC wording | with the Xbox or PlayStation button style, "PRESS START" reads PRESS MENU / PRESS OPTIONS, "Press START to join" and friends follow, the dodge tutorial says LT/RT or L2/R2, and Xbox-only save texts say "Computer" / "this computer". The original text is untouched with `button_prompts = xbox_original`. `STE_TEXT_PATCH=0` turns it off. The text is changed in memory after the game has loaded it (about 1-2 % of one core for the scan). | in |
| Back+Start | held together they are now the overlay chord and are hidden from the game (so Start does not also open the game's pause menu) | in |
| Version | exe resource and `--version` say 0.1.5 | in |

## What changed since 0.1.3a
| Area | Change | Status |
|---|---|---|
| Start-up speed | **about 20 s faster every launch.** The game's network start-up waited ~20 s for an Ethernet link that does not exist (we reported "cable connected"). The runtime now answers "no cable" like a console without one, so the first game file is opened after 1.7 s instead of 22 s and the intro movies start 20 s sooner. `STE_ETHERNET_LINK=up` restores the old answer; a future online path (for example an Insignia-style service) would use that override. Online play never worked and still does not. | in |
| Start-up diagnostics | a `[BOOT]` timeline in the log (milliseconds since the process started, one line per step, plus the first file opens); `STE_BOOT_SAMPLE=1` adds a 2 s sampler. Symbol loading for crash reports moved to a background thread (it cost 1-3 s on a cold first launch). | in |
| Mods that are patches | the game reads an enabled patch mod's `mods\<id>\_built\<file>` (built by the launcher or by `tools\step-diff\step_diff.py`, a small GPL tool that makes and applies `step1` patches) only when its SHA-256 equals the manifest's `result_sha256`; a missing file or a wrong hash falls back to the original and says so in the log (`[MODS] ... not built` / `RESULT MISMATCH`). Mods never contain game data. A self-test mod is in `tests\mods\ste.test-patch`. | in |
| Settings | `[audio] master_volume` (0-100, default 100) and `[window] hide_cursor` (`auto` hides the pointer after 3 s without movement, `always`, `never`) in the settings ini; both are also what the in-game overlay will change live | in |
| Crash reports | the report lists the last 48 memory-protection calls (who made the faulting page read-only); `STE_PROTECT_LOG=1` prints them live; `STE_DISK_SLOW_MS` / `STE_DISK_SLOW_JITTER` simulate a slow disk for tests | in, opt-in |
| In-game overlay (moved to 0.1.5) | the launcher's overlay (Video, Enhancements, Controllers, return to launcher = exit code 20) is written but **not in this build**: inside the game process its start-up still crashes in the launcher library (the launcher session is looking at it), and linking it needs a newer Visual C++ runtime than the game needs today. It is built only with `candidate376\relink376b_overlay.bat` | not in |
| Text wording | START/BACK and "Xbox console" re-wording per button style is not in this release: the game looks its text up by the English string, so editing the strings breaks loading (docs\REPORT_0.1.4.md). The code is in, off unless `STE_TEXT_PATCH=1` | not in |
| Controller test | `STE_INPUT_SELFTEST=1` runs a virtual Xbox One pad through the real mapping (every face button, Black/White, triggers, Start/Back, D-pad) and prints PASS/FAIL; the log now shows each pad's effective bindings and warns about custom face-button bindings (docs\CONTROLLER_MAPPING_TRACE.md; an A/X swap was reported, no defect was found in the game's chain) | in |
| Version | exe resource and `--version` say 0.1.4 | in |

## What changed since 0.1.2d (carried over from 0.1.3)
| Area | Change | Status |
|---|---|---|
| Input / window | mouse pointer auto-hides while the game has focus; window client size corrected on monitors with a different DPI | in |
| Capture | opt-in frame ring (`STE_CAPTURE_RING=1`, F12 saves the last 60 frames) | in, off by default |
| Widescreen | hit/impact screen flash covers the whole widened view (it was an inset rectangle on ultrawide) | in |
| Draw distance | `draw_distance=unlimited` fog x8 and the scenery query volume `query_scale` (1..4, default 1.5): the Tokyo skyline no longer pops in at ~1360 units | in, optional |
| Performance | game code is built optimised (four hot generated chunks were not), faster vertex path, precise 60 Hz vblank; governor keeps the frame rate when the distance is wide (`query_scale_auto`) | in |
| Stability | frames are consumed from a private copy of the pushbuffer ring (the faster game could overwrite unconsumed commands), vertex reads beyond RAM are zero-filled instead of crashing; x4/x8 sessions of ~19 min ran clean | in |
| Build recipe | the `D3DDevice_Swap` frame hook is now a normal `ste_hooks.toml` hook; before this a build from the published recipe produced no picture | in (**must be in the public tree**) |
| Test tools | `STE_PERF`, `STE_PERF_SAMPLE`, `STE_TITLE_FPS` (fps in the title bar), `[PBLAP]`, `[QGUARD]` log lines | in, opt-in |
| Button prompts | `[input] button_prompts` / `STE_BUTTON_PROMPTS` = `auto` (PlayStation pad -> PlayStation symbols, any other pad -> Xbox symbols, no pad -> original art) / `xbox` / `playstation` / `xbox_original` (byte-identical to the shipped art). Swaps the A, B, X, Y face-button pictures through a texture pack (art: Kenney Input Prompts, CC0). START / BACK / Black / White are words in the font and are not changed yet (0.1.4) | in |
| No console window | the exe is a Windows-subsystem program: no black console behind the game. The log goes to `<logs>\ste_<date-time>.log` (next to the settings folder, else `data\logs`); `--console` or `STE_CONSOLE=1` opens a console that mirrors it; a launcher that redirects stdout/stderr keeps working as before. Exit codes and the `[CRASH]` dump are unchanged | in |
| Out-of-RAM guard | texture reads that would run past the 64 MB of Xbox RAM are refused (vertex reads already were) | in |
| Version | exe resource and `--version` say 0.1.3 | in |
| Point sprites | point-list draws (the title's missile bodies and smoke trails) are drawn as screen-space quads instead of one pixel (`STE_POINT_SPRITES=0` restores the old behaviour). Missiles are still not fully visible, see Known issues | in |
| Governor | the query-distance governor now moves its applied value slowly (`query_ramp_pct`, default 5 % per second) so the distance does not pump | in |
| Debug options | `STE_RING_AUTO_GAP_MS`, `STE_RING_FIGHT_FROM`, draw-log windows and the OOB report are opt-in test switches, off by default | in, opt-in |

## Known issues
- The footer word BACK next to the B picture is not re-worded (it means "go back"); only START and the Xbox-only save texts are.
- Mechagodzilla 2's finger rockets were not specifically re-checked in 0.1.6 (the point-sprite fix is generic; the sampled frames of the effects sweep showed no defect). Mechagodzilla 3's missiles are fixed in 0.1.6 (older notes in docs\ITEM_D_MISSILES.md).
- A rare freeze (no more frames, window stays open) was fixed in 0.1.3a (c370): the emulated vblank could wait for ever on an interrupt-status bit when another thread queued work at the same moment. It was seen three times in test runs, never in normal play reports.
- Possible rare stall in the sound code: in two test runs of an earlier build (c367, debug frame-sleep on) the game stopped producing frames while its main thread sat in the DirectSound buffer routine (the game's audio set-up). Not reproduced since and not fixed in c370; if the game freezes, please send the log from `<logs>`.
- test PC B (the owner's other PC) feedback is pending: two-monitor window test (c261) and the distance/performance test zip `ste-recomp-distance-perf.zip`.
- 1%-low frame rate reads ~52 fps in every mode (frame-pacing jitter, about 18-19 ms p99); average is ~59 fps.
- `query_scale` above 1 draws far objects the game never draws at that distance; some may look odd (stale vertex data is zero-filled, so they vanish rather than glitch). The governor lowers the distance in heavy fights.
- Linux/macOS are not supported yet (see `PORTABILITY_AUDIT.md`).

## Release checklist (before anyone publishes)
1. Licence audit of the merged tree file by file (GPL-2.0-or-later for our code; compiled-in third-party code GPL-compatible; Apache-2.0 compiled in => binaries ship as GPL-3.0). Reports: `internal reports (kept privately)`.
2. No game data anywhere: no default.xbe, no extracted files, no generated C in the repo (`.gitignore` covers `generated/`); screenshots with game imagery stay out. Run the repo scan.
3. GPL notices, `THIRD_PARTY_NOTICES.md`, `CREDITS.md` current; `GAME_DATA_NOTICE.md` in the package.
4. Disclaimer in README, About and the launcher: "Not affiliated with or endorsed by Toho, Atari or Pipeworks. You must own the game."
5. Hash checks: `ste/gen/xbe.sha256` verifies the user's default.xbe; `expected_generated.sha256` (includes the new `recomp_0013.c`) verifies a local regeneration; record the SHA-256 of every shipped binary.
6. Clean-directory run: unzip into an empty folder, point at a legitimate copy, play Versus Tokyo + New York for 15 minutes.
7. Version strings 0.1.3 (exe resource: done in c369; launcher, README: pending); changelog from the table above.
8. A build from the **published recipe** (not the dev tree) must show a picture and match the dev build's behaviour.

## Test plan for 2-3 PCs
| PC | What |
|---|---|
| Dev PC (RTX 2080 SUPER, Windows 11) | done: 1765x993 and 3840x2160, original / x4 / x8, Tokyo + New York, 3 x 19-min x8 + 1 NY + 1 x4 soaks |
| test PC B (owner) | `play_dist_original.bat`, `_4`, `_8` at 1080p and its native resolution; report fps in the title bar, pop-in, any stutter; send `data\logs\play_dist_*.log`; two-monitor window test when asked |
| Low-end 4-core / iGPU | `play_dist_original.bat` at 1280x720 and 1920x1080 (the 60 fps floor), then `_4`; the governor should hold the frame rate by shortening the distance |
Record for each run: exe SHA-256, resolution, mode, median fps, worst frame, `[QGUARD]` cap range, crash/replay lines (`[CRASH]`, `[PB-REPLAY]`, `[KELVIN-OOB]`).

## 0.3.1 (draft): photo mode
| Photo mode | a paused free camera in every fight: overlay entry "Photo mode" (or F8, or Back + Y held), fly with the sticks/WASD, wider/narrower FOV, roll, HUD on/off, full-resolution PNG screenshots to `<data>\screenshots`, B / Esc closes into the exact same match. `[camera]` settings in the ini. Needs the 0.3.1 generator (`return` hook mode) | built and verified in game (Tokyo, New York, 16:9 and 21:9, controller-only pass); keyboard/mouse and the overlay button not exercised in the headless harness |

## 0.4.0 (draft): Biollante and stacked recipe mods
| Biollante | recipe mod `ste.biollante` (built on your PC from your own disc, holds no game data): she becomes a normal character with a grid icon (generated from the disc's own art), unlocked, with a stock-level fight AI. Playable as P1, COM and human P2 (second pad), in Versus, Melee, Action mode (Monster Island) and Solo Destruction | verified in game (16:9, 21:9, 4:3 select screens; Tokyo matches; Action vs Anguirus; Melee with a human Biollante; sampled check of her attacks and effects) |
| Third select row | roster mods get a third row (grid scaled to 78 %), centred, up to 12 extra monsters per row, slot table `bio_ui.SLOTS`, docs\formats\roster_slots.md | verified with Biollante; a filled row only as a mock-up |
| Stacked recipes | `iso_to_mod.py prepare-stack`: several recipe mods that edit the same file are built on top of each other in load order, clashes are named; the game accepts the recorded result (c436) | synthetic tests + in game: both mods on, each alone, neither |
| Menu-state fix | the widescreen code treated any front-end state other than 0x27 as a fight: with a roster mod the menus lost their 4:3 pillarbox and the HUD stretched (c435) | verified at 16:9, 21:9, 4:3; retail unchanged |
Known issues: no preselect of a monster in the extra row when the select screen reopens; Solo Destruction title is English only; photo mode HUD clock picture keeps counting while the HUD is visible; Biollante's AI is the stock difficulty level; the attack/effects check was a scripted sample, not every move. Credits: Biollante data = retail game; third-select-page idea (PS2): ANILORAK (research only); the STEM Biollante AI function and DylanRocket's tutorial were not used.

## 0.4.1 (draft): hotfix for 0.4.0
- **Fixed: Solo Destruction no longer sticks.** After choosing SOLO DESTRUCTION (and backing out), Versus, Action and Melee could open the Solo select screen. The script flag was never cleared (`setShellCheats(x - bit)` does not clear bits; `clearShellCheats` does). It is now cleared when the main menu gets focus and after every game start. Needs the updated Solo Destruction mod (Prepare mods again).
- **Fixed: the SOLO DESTRUCTION title** is now the same font, size and colour as VERSUS (it was shrunk to fit the retail 175 px title width; the mod widens that limit).
- Runtime: a missing `#include <stdlib.h>` made `STE_ETHERNET_LINK=up` crash in the game's network start-up (a truncated pointer); fixed. The default (link down) was never affected. Developer option `STE_NIC_TRACE=1` logs the network-card register accesses (off by default).
- Not reproduced here, details wanted: the Biollante third-row tile on the Solo screen, the black attract movie and the globe on the ultrawide main menu (see docs\REPORT_0.4.1.md).
- Credits: Solo Destruction is inspired by ANILORAK's research (our own code; no code from ANILORAK used). Full credits: CREDITS.md.
