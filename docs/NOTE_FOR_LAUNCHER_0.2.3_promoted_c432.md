# NOTE FOR THE LAUNCHER SESSION: 0.2.3 promoted = c432
Exe: mission-l6\artifacts\ste_recomp_mission_l6_candidate432.exe, SHA-256 0243A6D854DDB1094F75E035286F896DECD33B43ADEFFB71FDD3927301672481, version resource 0.2.3 / 0.2.3+c432, capabilities.json adds "sound_mod_packs". Validator PASS. Not published. Base c431 (0.2.2). Code differences: kernel_soundpack.c (new) + the kernel_mods.c hook (patches\c432_sound_packs.patch, patches\c432_kernel_soundpack.c) and env-gated debug trace filters in recomp_manual.c (STE_DD_TRACE_MOVED / STE_DD_TRACE_ZMIN, logging only).
## Lines you can quote, and what is verified
1. "Sound packs: replace the game's effects, announcer lines and music with your own WAV files." Verified in game: announcer cues VERSUS and START FIGHT replaced by tone chimes, all fight music tracks replaced by a tone arpeggio (log: `[MODS] sound pack proof-sounds: ACTIVE, 16 file(s)`, banks rebuilt, `open sounddata\fight1.wxv -> ...\_soundcache\...`; recorded audio analysed). Offline: rebuilt banks decode to the replacement tones, every other sound byte-identical.
2. "Export and build tools (Python only)": verified from a clean folder by you; docs\formats\soundpack.md, docs\NOTE_FOR_LAUNCHER_0.2.3_sound_packs.md.
3. Loops: "end the file name with _loop" is implemented (flag written to the bank directory and the cue record) but looping of replaced sounds is NOT verified in game: say "untested" or leave it out of the release text.
## Not in 0.2.3 (do not claim)
- RAGE announcer: investigated, no defect found (the game plays its RAGE cue on a real rage pickup at normal level; docs\REPORT_0.2.md 0.2.3 log). The wording of the line is for Miko's listening check.
- Tokyo UFO: not resolved (the UFO is not culled by the distance tests we override; no change). Unlimited pop-in: unchanged. ANILORAK HD sample: not delivered.
Docs: docs\formats\soundpack.md, FEATURE_SOUND_PACKS.md, RELEASE_ALPHA.md section 0.2.3 (with a changes text). Proof clip (not for the showcase, launcher check_clip says REJECT): E:\GodzillaSTE\release\media\0.2.3\clips\sound_proof.mkv with CLIPS.txt.
Library source for this build is in publish\ste-recomp xboxrecomp\src\kernel (kernel_soundpack.c, kernel_mods.c, CMakeLists.txt) once the commit below is pushed by Miko.
For the machine: my game is not running; say "launcher busy"/"launcher clear".
