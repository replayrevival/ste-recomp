# Sound packs (audio mods) -- STE recomp, 0.2.3 (c432)
Own analysis of the Xbox game files; no third-party code. Tools: `tools\mods\sound_pack.py` (export / import). Overview and design: docs\FEATURE_SOUND_PACKS.md.

## Pack layout
A sound pack is a mod folder (installed in the launcher's Mods tab, `<STE_MODS_DIR>\<id>\`):
```
manifest.json        {"id": "...", "name": "...", "version": "1.0", "profile": "sounds", "enabled": true, "priority": 100, "credits": "..."}
sounds\<file>.wav    PCM 16-bit WAV files named after the game's sounds
report.csv           written by the import tool (matched / unmatched / format-refused / notes)
```
Loading follows the mods system: `order.json` `enabled` flags override the manifest, `STE_MODS_ENABLE` overrides both, a pack that fails validation is skipped, later packs win on the same sound. `last_report.txt` gets `sound pack <id>: ACTIVE, K file(s)` and one line per refused file and per rebuilt bank/stream. Capability: `sound_mod_packs`. No active sound pack = nothing is built and the game reads its original files (byte-identical behaviour).

## Sound names (stable ids)
- Bank sample: `<bank>_<3-digit slot>[_<cue name>].wav`, bank = `godzilla_sound` (effects, monster sounds, ambience: 788 slots), `shell_sound` (menu/shell effects), `rail_fpm` (railer vehicles), `godzilla_voice_e` (English announcer/voices, 65 slots), `godzilla_voice_j` (Japanese). Only bank + slot count; the cue name after them is for readability (e.g. `godzilla_voice_e_028_RAGE.wav`).
- Music / jingle stream: `<track>.wav`, track = the name of a `SoundData\*.wxv` file without extension: `fight1`..`fight11`, `menu`, `Results`, `Victory1/2`, `Defeat1/2` (music) and one per monster (`Godzilla2K`, `Gigan`, `Rodan`, ... jingles).
`sound_pack.py export <game folder|.iso> <out> [--match WORDS] [--category effects|voices|music|monsters] [--list]` writes every sound under exactly these names (16-bit WAV: bank sounds mono at their own rate, streams stereo 48 kHz) plus `export_report.csv`; looped bank sounds get a WAV `smpl` loop chunk.

## Replacement rules
- WAV PCM **16-bit**, mono or stereo, any sample rate (4 kHz..192 kHz). 8/24-bit, float, ADPCM/other codecs, more than 2 channels, more than 60 s: refused with a plain message (report status `format-refused` / `refused`; the game keeps the original sound).
- Bank sounds are mono: a stereo WAV is mixed down (note in the report). The WAV is resampled to the rate of the original slot (linear, with a box pre-filter when downsampling) and encoded to Xbox IMA ADPCM. Length may differ (note "length differs"): the bank is rebuilt with the new length; very long sounds make the bank bigger (limit: 32 MB growth per bank).
- Streams are stereo 48 kHz: a mono WAV plays on both sides; the file is resampled and encoded. The whole stream is replaced; the game restarts tracks itself, so a stream has no loop marker.
- Loop (bank sounds only): a WAV `smpl` chunk with at least one loop (Audacity: Export > WAV with a loop region / Tracks > add loop) sets the slot's loop flag: the WHOLE sound loops (a loop that does not cover the whole sound is noted and ignored). Without `smpl` the sound plays once (the flag is taken from the WAV: no loop chunk = one-shot, even if the original looped).
- Safety: unknown names are ignored ("unmatched"); duplicates: the later pack/file wins; a WAV that cannot be decoded never stops the game (the original stays).

## How it reaches the game (implementation)
At start the mods system hands the active sound packs to `xbox_soundpack_build` (`kernel_soundpack.c`), which reads the ORIGINAL bank/stream from the game folder or the read-only XISO, replaces the slots/streams, writes the result to `<STE_MODS_DIR>\_soundcache\<key>\` and registers it as a file override of `SoundData\<name>.samp/.uber/.wxv`. The key is a hash of the pack file names, sizes and times, so an unchanged pack is not rebuilt. Packs hold only the user's own audio; nothing from the game is stored in a pack, the originals are never written.

## File formats (decoded for this feature)
- `.samp`: samples back to back; each = 0x14-byte WAVEFORMATEX (tag 0x69, 1 channel, rate, rate*36/64 bytes/s, block align 36, 4 bits, cbSize 2, samples/block 64) + Xbox IMA ADPCM (36-byte blocks, 64 samples). Verified for all 5 banks: rebuilding from the slot table reproduces the file byte for byte.
- `.uber` (UBER 1.1): header (magic, version, header size 0x20, 5 section offsets) then `PROJ` (cue table), `POOL` (per-cue records of 0x28 bytes; u16 at +10 = slot), `SDIR` (16-byte header: tag, version, .samp size, slot count; then 16 bytes per slot: data offset, data length, loop flag, header offset = data offset - 0x14; slot 0 is a null entry) and the name table (project name, then one NUL-terminated name per cue). A rebuild rewrites only the SDIR table and its size field.
- `.wxv`: RIFF WAVE, `fmt ` 0x14 bytes (tag 0x69, 2 channels, 48000 Hz, 54000 B/s, block 72, 4 bits, cbSize 2, 64 samples/block), `data`. Block = 4-byte header per channel (int16 predictor, step index, reserved) then 8 groups of per-channel 4-byte nibble words (low nibble first); a block holds 64 samples per channel. Identical to the decoder in `src\apu\apu_state.h`.

## Proof (2026-11-19, c432)
Synthetic pack (tones only) in a Tokyo Godzilla 2000 vs Gigan run: log `[MODS] sound pack proof-sounds: ACTIVE, 16 file(s)`, `bank shell_sound rebuilt`, `bank godzilla_voice_e rebuilt`, `stream fight1..11 built`, and the game opening `sounddata\fight1.wxv` from `_soundcache`. The recorded audio shows the replaced announcer cues (VERSUS 660 Hz, START FIGHT 330/440/550 Hz chime) and the replaced fight music (arpeggio notes 392/330 Hz). Offline: the rebuilt banks decode (independent Python decoder) to the replacement tones with every other slot byte-identical (`mission-l6\tests\sp_harness`).
