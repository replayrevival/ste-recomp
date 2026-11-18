# Gameplay recording (STE_RECORD) - opt-in, for trailers and release media

Added in 0.1.6, reworked in 0.1.7. Off by default; when off it costs one integer test per presented frame.

| Env | Meaning |
|---|---|
| `STE_RECORD=<file.mkv>` | record the presented frames (what the screenshots show: the swap-chain back buffer after upscaling/blit, bars included, **no overlay**) |
| `STE_RECORD_OVERLAY=1` | include the in-game overlay |
| `STE_RECORD_FROM=<s>` / `STE_RECORD_SECS=<n>` | start at process age `<s>` seconds, stop after `<n>` seconds |
| `STE_FFMPEG=<ffmpeg.exe>` | ffmpeg to pipe into (default `ffmpeg` on PATH) |
| `STE_RECORD_CODEC=x264\|x264fast\|nvenc\|raw` | default `x264` ultrafast crf 17 (light on the CPU, ffmpeg runs at below-normal priority); `x264fast` = veryfast crf 14; `nvenc` = h264_nvenc (on the 0.1.7 test PC the GPU encoder next to the game halved the frame rate, so it is opt-in); `raw` = `<file>.rgba` + `<file>.txt`, no ffmpeg |
| `STE_RECORD_ARGS="<ffmpeg encoder args>"` | replace the encoder arguments |
| `STE_RECORD_FPS=<n>` | constant output frame rate (default 60) |
| `STE_RECORD_TS=frames\|wall` | `frames` (default): one video frame per presented frame at the constant rate, so ffmpeg adds no repeated or dropped frames; `wall`: wall-clock timestamps (needed when presents are sparse, e.g. an open in-game menu) |
| `RECOMP_AUDIO_WAV=<file.wav>` | the game's own final mix, 48 kHz 16-bit stereo (APU tap); `RECOMP_AUDIO_WAV_WALL=1` pads the file with silence whenever more than 200 ms of wall time has no samples (pause, load), so wav time = time since the first sample |

How it works: the render thread issues a GPU copy into a ring of 12 staging textures and, a few frames later, a non-blocking Map; a worker thread repacks the rows and writes them to ffmpeg's stdin, so the render thread never copies a frame or waits for the encoder. If the encoder cannot keep up the ring fills and frames are DROPPED and counted: the game is never slowed. At stop `[RECORD] stopped: N frames written, D dropped (ring full), R map retries; presented frames P, intervals > 20 ms: A, > 34 ms: B, longest L ms` is logged: a clip is smooth when D = 0; A/B/L are the game's own frame-time hitches during the take (shader compiles, effects), not recorder cost.

Sizes: the file has the size of the swap chain (the window's client area); use `window_mode = borderless` for an exact frame size; odd sizes are padded to even. Showcase footage is 1920x1080 at 60 fps (project rule, RELEASE_CHECKLIST item 6).

Audio sync: both files log a tick (`[AUDIO-WAV] first samples tick=`, `[RECORD] first frame tick=`); with `RECOMP_AUDIO_WAV_WALL=1`, `audio_offset_s = (video first frame tick - wav first samples tick) / 1000` is the WAV time of the first video frame.

Checking a clip: `ste/tests/verify_clip.py <clip.mkv> --log <run stderr>` (timestamps uniform, recorder counters) and the launcher's `check_clip.py` (repeated frames during motion).
Clips are never committed to a repo.
