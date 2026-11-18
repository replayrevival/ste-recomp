<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Release checklist

Never publish or push: Miko publishes.

1. Candidate promoted (PROMOTED note for the launcher session), validator 0 FAIL, SHA-256 recorded.
2. Soak clean (mixed run, dumps, [DISPATCH] stalls, fps against the previous release).
3. **Showcase screenshots refreshed (standing rule, Miko 2026-11-13).**
   - Capture with the game's own capture path (backbuffer, not the desktop). Clean frames: no debug overlays or probes; HUD on unless the shot is a comparison.
   - Set: at least 1080p 16:9 and 21:9 ultrawide; Unlimited draw distance (wide city skyline); a big fight with effects (beams, explosions, hit flash); each feature new in this release; button prompts in Xbox and PlayStation styles; from beta on, HD art vs original side by side.
   - Files: `docs/screenshots/<version>_<what>.png` (e.g. `0.1.4_ultrawide_tokyo.png`), PNG, at most ~2560 px wide, compressed.
   - Move the previous release's set to `docs/screenshots/archive/<version>/` (do not delete).
   - Update the README "Screenshots" section: one short caption per image; keep Miko's own shots if she provides them.
   - Hand the set to the launcher session.
4. Docs: RELEASE_ALPHA.md known issues, LAUNCHER_INTERFACE.md, credits.
5. Publish repo synced locally (author "Replay Revival", no push).
6. **Trailer / YouTube Short footage (Miko's order, 2026-11-18; launcher session cuts the video with ffmpeg).**
   - Request and formats: `E:\GodzillaSTE\launcher\docs\MEDIA_CLIPS_REQUEST.md`. Raw clips go to `E:\GodzillaSTE\release\media\<version>\clips\` with a `CLIPS.txt` (one row per clip: file, what it shows, resolution, fps, `audio_offset_s`). Clips are NEVER committed to any repo (game imagery in videos is for YouTube, not for git).
   - Audio: `RECOMP_AUDIO_WAV=<file.wav>` (48 kHz 16-bit stereo, the APU mix handed to the host device; header valid every second). Put `<clip>.wav` next to `<clip>.mkv`; `audio_offset_s` = WAV time of the clip's first video frame (wav_time = video_time + audio_offset_s).
   - Video: opt-in recorder `STE_RECORD=<path>` (see docs\RECORDING.md): the presented frames (same capture path as the screenshots, no overlay) are buffered and written by a worker thread so the game timing does not change; off by default and free when off.
   - Scripted showcase runs (pad scripts in `tests\pads\showcase_*.pad.txt`): ultrawide Tokyo flyover at Unlimited; big fight with beams/explosions/hit flashes; Mechagodzilla missiles (a salvo from a few angles, 8-12 s); in-game menu opening/closing over a fight; Xbox vs PlayStation button prompts; 4:3 original vs 21:9 of the SAME scripted moment (same start frame); draw distance original vs unlimited; each new feature of the release (0.1.6: missile trails, keyboard name entry typing a name).
   - Resolutions: 3440x1440 or 2560x1080 (21:9), 1920x1080 and 3840x2160 where possible.
   - Send the launcher session a message when the clips are in place.
   - **Standing rule (Miko, 2026-11-18): showcase footage is recorded at 1920x1080, 60 fps** (ultrawide takes at 21:9 only where the clip is about ultrawide) and checked for smoothness before it is handed over: `check_clip.py` of the launcher (pts gaps + repeated frames during motion; static pauses may repeat) and `ste/tests/verify_clip.py` with the run log (recorder: dropped = 0). Re-record on REJECT. No "4K" claims unless the footage is verified smooth. Shorts: pulled-back camera (STE_VIDEO_FOV_SCALE about 1.25 with fov_min_zoom 0.7854), both monsters in frame, action not buildings.
