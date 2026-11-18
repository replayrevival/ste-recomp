# Gameplay recording (STE_RECORD) — opt-in, for trailers and release media

Added in 0.1.6 (c404). Off by default; when off it costs one integer test per presented frame.

| Env | Meaning |
|---|---|
| `STE_RECORD=<file.mkv>` | record the presented frames (what the screenshots show: the swap-chain back buffer after upscaling/blit, bars included, **no overlay**) |
| `STE_RECORD_OVERLAY=1` | include the in-game overlay |
| `STE_FFMPEG=<ffmpeg.exe>` | ffmpeg to pipe into (default `ffmpeg` on PATH). Example: the ffmpeg.exe of a winget "Gyan.FFmpeg" install |
| `STE_RECORD_CODEC=x264\|nvenc\|raw` | x264 (default, `-preset veryfast -crf 14`), `h264_nvenc -cq 16`, or raw frames (`<file>.rgba` + `<file>.txt`, no ffmpeg needed) |
| `STE_RECORD_ARGS="<ffmpeg encoder args>"` | replace the encoder arguments |
| `STE_RECORD_FPS=<n>` | constant output frame rate (default 60) |
| `RECOMP_AUDIO_WAV=<file.wav>` | the game's own final mix, 48 kHz 16-bit stereo (existing APU tap) |

How it works: the render thread copies the frame into a 3-deep ring of staging textures (never waits; a copy that is not finished yet is skipped), memcpy's it into a pooled buffer and a worker thread writes it to ffmpeg's stdin. ffmpeg timestamps frames by the wall clock and writes a constant-frame-rate file, so a dropped frame never changes the speed. The game's timing is not touched. At exit `[RECORD] stopped: N written, N dropped, N skipped` is logged; if the process is killed ffmpeg still finalises the file (the pipe closes).

Sizes: the file has the size of the swap chain (the window's client area). Use `window_mode = borderless` (or fullscreen) for an exact 1920x1080 / 2560x1080 / 3440x1440 / 3840x2160 frame; odd sizes are padded to even.

Audio sync: the WAV starts at its first samples, the video at its first frame. Both log a `GetTickCount64()` value (`[AUDIO-WAV] first samples tick=…`, `[RECORD] first frame tick=…`); `audio_offset_s = (video first frame tick − wav first samples tick) / 1000` is the WAV time of the clip's first video frame (wav_time = video_time + audio_offset_s). Example: first-frame tick minus first-samples tick = 0.469 s in a test run.

Clips are never committed to a repo (see RELEASE_CHECKLIST.md item 6).
