<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Low-end preset (play_low.bat / artifacts\low) -- target, settings, evidence (0.2, c425)

**Target machine (the claim):** 4 cores / 8 threads (or 2C/4T with some dips), 8 GB RAM, a GPU of about 3 TFLOPS fp32 (GTX 1650 / RX 570 / Intel Iris Xe 96 EU class), Windows 10/11, 1080p60 display.
**Settings (artifacts\low\ste_video.ini):** 1920x1080 window, render_scale 0.667 (720p internal) + AMD FSR 1.0 (sharpness 0.25), draw_distance 2, render_threads 2, vsync off + vrr auto, fps_cap 60, anisotropy 0, mip_bias 0, original fog/culling, no extras, no texture packs.

## Evidence on the dev PC (Ryzen 7 3700X, RTX 3080 Ti; the only PC available)
- CPU side (REPORT_0.1.7 5d, c415): with the process restricted to 8 logical CPUs the Low preset holds 58.8-59.6 fps per 10 s window, p99 18.2-19.2 ms, 0 frames > 33 ms; with 4 logical CPUs 55.5-59.8 fps (two windows dip, p99 up to 35 ms).
- GPU side (new, c425, 2026-11-19; F:\GodzillaSTE_work\low_gpu + lowgpu_s.out): Low preset, 1080p window, 8 logical CPUs, Tokyo Godzilla 2000 vs Gigan fight, Windows per-process counter `GPU Engine(pid_*)\Utilization Percentage`, 3D engine, 17 samples of one second each (5 s apart) over the fight: mean 12.6 %, min 5.0 %, max 24.3 % of the RTX 3080 Ti (copy / video engines 0 %). At 60 fps that is a mean of about 2.1 ms and a peak of about 4.1 ms of 3D-engine busy time per frame.
- Scaling to the target GPU (an ESTIMATE, not a measurement): RTX 3080 Ti is about 34 TFLOPS; a 3 TFLOPS GPU is about 11x slower: mean about 23 ms, peak about 45 ms if busy time scaled linearly with raw throughput, which would NOT hold 60 fps. The utilisation figure overstates the cost because the 3080 Ti runs at reduced clocks at 12 % load (a rough factor 1.5-2), so the realistic range for a 3 TFLOPS GPU is about 11-16 ms mean frame GPU time (inside 16.7 ms, with the heavy moments dropping to 45-60 fps). A GTX 750 / UHD 630 class GPU (0.4-1.3 TFLOPS) is 3-10x slower still and will not hold 60 fps with this preset.

## Verdict
Proven here: the CPU budget (4C/8T, mostly 2C/4T) and the Low preset's settings. NOT proven: 1080p60 on the target GPU; the measurement says it is plausible but marginal at ~3 TFLOPS and not reachable below ~2 TFLOPS. A real run on a modest PC (e.g. a GTX 1650 or Iris Xe laptop) is still needed; until then the preset should be described as "aims at 1080p60 on a 3 TFLOPS-class GPU". Knobs to go lower: render_scale 0.5 (540p + FSR), draw_distance original (1), render_threads 2, fps_cap 30 (the game logic is delta-time driven).
