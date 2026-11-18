# 0.1.7 promoted: c415

- Build: `mission-l6\artifacts\ste_recomp_mission_l6_candidate415.exe`, version resource 0.1.7.415 (`ste-recomp 0.1.7`). Package = 0.1.6 layout with `ste-recomp.exe` replaced.
- NEW for the launcher: `--game <file.iso>` / `STE_GAME_PATH=<file.iso>` mounts an XISO read-only (hash verification of the image stays with the launcher; the exe only logs `[CONFIG] game=disc image ... default.xbe N bytes`; exit 3 with a message for a file that is not an Xbox image or has no default.xbe).
- Cheats: setting keys `enhance.cheat_invulnerable_p1`, `_invulnerable_com`, `_super_damage_com`, `_red_p1`, `_red_com` are applied immediately by the game (STE_APPLIED_NOW); the overlay page for them is yours.
- PlayStation text fallback is now START (was MENU); Xbox unchanged.
- Recording: STE_RECORD (docs\RECORDING.md) reworked; env `RECOMP_AUDIO_WAV_WALL=1`; `STE_RAND_SEED`; `STE_TEXINV`. All opt-in.
- Media rule from Miko (1080p60, smoothness-checked) is in RELEASE_CHECKLIST item 6.
