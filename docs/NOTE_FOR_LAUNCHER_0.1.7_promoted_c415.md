# 0.1.7 promoted: c415

- Build: `mission-l6\artifacts\ste_recomp_mission_l6_candidate415.exe`, version resource 0.1.7.415 (`ste-recomp 0.1.7`). Package = 0.1.6 layout with `ste-recomp.exe` replaced.
- NEW for the launcher: `--game <file.iso>` / `STE_GAME_PATH=<file.iso>` mounts an XISO read-only (hash verification of the image stays with the launcher; the exe only logs `[CONFIG] game=disc image ... default.xbe N bytes`; exit 3 with a message for a file that is not an Xbox image or has no default.xbe).
- Cheats: setting keys `enhance.cheat_invulnerable_p1`, `_invulnerable_com`, `_super_damage_com`, `_red_p1`, `_red_com` are applied immediately by the game (STE_APPLIED_NOW); the overlay page for them is yours.
- PlayStation text fallback is now START (was MENU); Xbox unchanged.
- Recording: STE_RECORD (docs\RECORDING.md) reworked; env `RECOMP_AUDIO_WAV_WALL=1`; `STE_RAND_SEED`; `STE_TEXINV`. All opt-in.
- Media rule from Miko (1080p60, smoothness-checked) is in RELEASE_CHECKLIST item 6.

## Correction (2026-11-19): promote c416, not c415
c415 carried a stale version resource (0.1.7.412 / 0.1.7+c412; the candidate-copy script did not update ste_version.rc). **c416 = c415 + only the version resource**: `mission-l6\artifacts\ste_recomp_mission_l6_candidate416.exe`, SHA-256 BD423369D814BBDA69295A47B32A2E01F24DC0635670CCBB18A38DB7E976BC3B, FileVersion 0.1.7.416, ProductVersion 0.1.7+c416. The script (tests\newcand.ps1) now rewrites the resource.
Live cheat toggle verified on the c416 code path: Enhancements page > Cheats > "Player 1 invulnerable" ON shows "applies now"; log `[OVERLAY] setting enhance.cheat_invulnerable_p1 = '1'`; the fight continues with P1's bar intact. Screenshots: docs\screenshots\0.1.7_*.jpg.
