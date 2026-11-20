<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Sound packs (audio mods): inventory and design (2026-11-19, 0.2.3)

Own analysis of the Xbox game files (read-only) and of our runtime. ANILORAK's pack was NOT used for this document. Technical reference for the finished format: docs\formats\soundpack.md (written with the implementation).

## 1. How the Xbox game stores audio
| What | Files | Format | Evidence |
|---|---|---|---|
| Effects, monster sounds, ambience | `SoundData\godzilla_sound.samp` + `.uber` (973 cues, 788 sample slots, 15.5 MB), `shell_sound.*` (menu/shell effects, 14 cues), `rail_fpm.*` (railer/vehicle effects, 59 cues) | `.samp` = concatenated samples; each sample = 0x14-byte WAVEFORMATEX + data. All samples are **Xbox IMA ADPCM, mono, 36-byte blocks (65 samples header+64)**; rates 11025/22050 (almost all 22050), 16000, 22000, 44100 | `.samp` headers parsed for all 5 banks |
| Announcer and voices | `SoundData\godzilla_voice_e.*` (English, 66 cues, 998 KB) and `godzilla_voice_j.*` (Japanese, same cue list) | same sample format, **mono 11025 Hz** | same |
| Bank directory | `.uber` (UBER 1.1): header (5 section offsets), `PROJ` (cue table: kind, cue index, volume), `POOL` (per-cue records of 0x28 bytes; the u16 at +10 of each block = sample slot), `SDIR` (sample directory: per slot 16 bytes = data offset, data length, **loop flag** (0/1), header offset = data offset - 0x14; slot 0 is a null entry) and the **cue-name table** (project name, then one NUL-terminated name per cue) | decoded for all 5 banks (e.g. shell bank: 14 cues -> slots 1..13, `Mainmenu Ambient loop` is the looped one) |
| Music and per-monster jingles | `SoundData\fight1..11.wxv`, `menu.wxv`, `Results.wxv`, `Victory1/2.wxv`, `Defeat1/2.wxv` and one `<Monster>.wxv` per monster (Godzilla2K, Gigan, Rodan, ... 19 files) | `.wxv` = a plain RIFF/WAVE file, format tag **0x69 Xbox IMA ADPCM, stereo, 48000 Hz, block 72 bytes (2 x 4-byte headers + 64 bytes = 64 samples/channel)**; 39 files, 113 MB total with the banks. No loop markers in the file (the game restarts a track itself) | RIFF headers of all 39 |
| Not game audio | `media\Wave.xwb` / `Sound.xsb` (XACT bank, dashboard sound?), `dsstdfx.bin` (DSP effects image) | n/a | not referenced by name in default.xbe; ignored |

**Decoder facts** (identical to our APU's own decoder `adpcm_decode_block`): per channel a 4-byte header (int16 predictor, byte step index, reserved), then 4-byte groups per channel (8 nibbles, low nibble first); sample 0 of a block = the predictor, the VP uses 64 samples per block.

## 2. How the audio reaches our audio layer
The game loads a bank `.samp` into guest RAM (the English voice bank sits at 0x1716B40.., see REPORT_0.2) and plays a cue by programming an APU voice (DirectSound buffer) that reads the ADPCM blocks from that RAM (our `apu_vp.c`, the xemu-derived MCPX VP). Music `.wxv` files are streamed from the file through DirectSound streaming buffers (file reads through `NtReadFile` -> our file layer). With `RECOMP_APU_TRACE=1` each active voice prints its guest address; mapping the address into a bank gives bank + sample + cue: **a voice at 0x173xxxx was `GZ2K INTRO` (voice bank sample 14)**.

## 3. Cue names
Every cue of the five banks has a name in the bank's own name table (973 + 14 + 59 + 66 + 66 names, e.g. `RAGE`, `KO`, `START FIGHT`, `POWERUP1 PICKUP`, `GIGAN ROAR RAGE`); `docs\formats\soundpack.md` will list the bank/cue ids that `export` writes. Music tracks are named by their file (`fight1` ... `menu`).

## 4. RAGE announcer: first result of the mapping (for the 0.2.3 RAGE item)
Mapping the voice-bank addresses of the earlier traces: the voice heard at rage start (t=348 s in `F:\GodzillaSTE_work\rage_b`) is **voice-bank sample 28 = cue `RAGE`** (2.79 s, voice 69, same level as the other announcer lines). So the game does play its `RAGE` cue; our audio layer plays it (rms > 0). Whether it sounds right is for the listening check (exported WAV of that cue: `python -I tools\mods\sound_pack.py export <game> <out> --match RAGE`).

## 5. Design (pack format, loader)
**Replacement granularity**: one WAV per bank sample (`<bank>_<slot>_<cue>.wav`, e.g. `godzilla_voice_e_028_RAGE.wav`) or per music file (`fight1.wav`). Pack = mod folder with `manifest.json` (`"profile": "sounds"`) + the WAVs. Nothing from the game is inside a pack.
**Loader (runtime, offline cache)**: at start the mods system collects the active sound packs (same `order.json`/`STE_MODS_DIR` rules as textures), and for each affected game file builds a replacement in `<mods>\_soundcache\<pack-hash>\`: for a bank it reads the ORIGINAL `.samp`/`.uber` (from the game folder or the XISO), swaps the replaced samples (WAV -> 16-bit mono -> resample to the slot's rate -> Xbox ADPCM encode), rewrites `.samp` and the SDIR table, and the mods file override then serves that file; for music it encodes the WAV (-> stereo 48 kHz Xbox ADPCM RIFF) as the whole `.wxv`. No active sound pack = nothing is built and no override exists = byte-identical behaviour to 0.2.2. This keeps the APU untouched (no new code in the audio hot path) and handles any length.
**Why not hook the APU voice fetch**: replacements of a different length would need voice end/loop registers rewritten per voice; a rebuilt bank is just a new file in the format the game already reads.
**Rules** (to be fixed in soundpack.md): WAV PCM 16-bit mono or stereo, any rate; 8/24-bit, float and compressed WAVs are refused with a plain message (report line `format-refused`); bank samples are mono (stereo is mixed down, reported), music is stereo (mono is duplicated); loop: a WAV `smpl` loop (Audacity writes it) turns the slot's loop flag on (the whole sample loops; a loop that does not span the whole sample is reported and the whole sample loops); music has no loop flag of its own (the game restarts tracks); safety limits: samples longer than 60 s or banks that would grow beyond 32 MB are refused.

## 6. Effort
Python tools (export, import/build): 2 days; C loader (WAV, resample, ADPCM encode, bank rebuild, override registration, cache): 3-4 days; proof (one effect, one announcer line, one music track, recorded) 1 day; docs 0.5 day.
