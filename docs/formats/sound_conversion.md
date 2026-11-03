# Sound conversion feasibility

## Findings

- vgmstream documents a command-line decoder (`vgmstream-cli -o output.wav input`) and explicitly states that it decodes but does not encode: http<local-folder>/ and `doc/USAGE.md` (observed from public documentation).
- vgmstream's public format table identifies Sony MultiStream `MIC` and lists the codec as PSX ADPCM: http<local-folder>/ (from public source/docs).
- The same table recognizes RIFF/WXV-style extensions and lists `XBOX_IMA` among supported codecs, so it is a good candidate for decoding STE `.wxv` samples (from public source/docs).
- No portable `vgmstream-cli` or `ffmpeg.exe` was already available in the F: workspaces, so no `.mic` decode was attempted. No tool was installed.
- Digitzaki's GZ-Sound-Manager is for Nintendo DSP in `.samp` plus `.uber/.sdir` containers; its README says it decodes to WAV and re-encodes Nintendo DSP, not PS2 MIC or Xbox WX V: http<local-folder>/ (observed from public README).
- FFmpeg's public `adpcm.c` has an `ADPCM_IMA_XBOX` decoder: http<local-folder>/ (from public source). Its current `adpcmenc.c` encoder list does not include `ADPCM_IMA_XBOX`, so stock FFmpeg is not sufficient to encode Xbox ADPCM format `0x0069` reliably (from public source).

## Recommended pipeline

1. Use vgmstream-cli to decode STEM/PS2 `.mic` to PCM WAV, preserving channel count, 44.1-kHz rate, loop metadata, and sample boundaries.
2. For Wii `.samp/.uber`, use GZ-Sound-Manager only for Nintendo DSP extraction/rebuild; do not apply it to PS2 MIC.
3. For STE Xbox `.wxv`, inspect the RIFF `fmt` chunk and retain format tag `0x0069`, 48-kHz rate, channel count, block alignment, and Xbox IMA coefficients/initial state.
4. Encode edited WAV with a dedicated Xbox IMA ADPCM encoder or a verified custom FFmpeg patch, then rebuild the WX V container/bank and update `.samp/.uber` only when the target game uses those banks.
5. Validate by decoding the produced bank again with vgmstream/FFmpeg and comparing duration, channel count, loop points, and a known-good Xbox sample.

## Status

- MIC decode: feasible with vgmstream, but not locally executed because no portable decoder was already present.
- Xbox ADPCM WAV decode: feasible with FFmpeg/vgmstream.
- Xbox ADPCM encode: requires a dedicated encoder or custom codec work; do not assume `ffmpeg -c adpcm_ima_xbox` exists.
- GZ-Sound-Manager: useful for Wii DSP banks only, not the PS2/Xbox conversion path.

## Verified decode test

Portable vgmstream r2117 was downloaded from the official GitHub release into `<local-folder>/` without installation.

| Input | Channels | Sample rate | Duration | Decoder result |
|---|---:|---:|---:|---|
| STEM `MIC/godzilla2k.mic` | 2 | 44,100 Hz | 9.800 s | PASS; PlayStation 4-bit ADPCM, Sony MultiStream MIC header |
| Xbox `SoundData/Godzilla2K.wxv` | 2 | 48,000 Hz | 11.167 s | PASS; Xbox 4-bit IMA ADPCM, RIFF WAVE header |

Decoded WAVs are under `<local-folder>/`.
