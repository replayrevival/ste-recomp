# SPDX-License-Identifier: GPL-2.0-or-later
"""make_sample_soundpack.py <out folder of WAVs>: generate SYNTHETIC test sounds (pure tones, no game audio, no copyrighted material) named after three game sounds:
   shell_sound_001_ROLLOVER.wav (menu rollover beep), godzilla_voice_e_028_RAGE.wav (announcer RAGE cue: a rising three-note chime),
   fight1.wav (music track of the first fight: a 14 s arpeggio, stereo 48 kHz).
Then build the pack: python -I sound_pack.py import <out folder> <game folder|.iso> <pack folder> --id sample-sounds --name "Sample sounds (synthetic tones)" --credits "generated tones (GPL-2.0-or-later)"."""
import importlib.util, math, os, sys
here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('sound_pack', os.path.join(here, 'sound_pack.py')); sp = importlib.util.module_from_spec(spec); spec.loader.exec_module(sp)

def tone(freq, secs, rate, amp=0.5, fade=0.01):
    n = int(secs * rate); out = []
    for i in range(n):
        e = min(1.0, i / (fade * rate), (n - i) / (fade * rate))
        out.append(int(32767 * amp * e * math.sin(2 * math.pi * freq * i / rate)))
    return out

def main():
    out = sys.argv[1] if len(sys.argv) > 1 else '.'
    os.makedirs(out, exist_ok=True)
    sp.wav_write(os.path.join(out, 'shell_sound_001_ROLLOVER.wav'), [tone(880, 0.35, 22050)], 22050)
    r = 11025; v = tone(440, 0.5, r) + tone(554, 0.5, r) + tone(659, 0.7, r)
    sp.wav_write(os.path.join(out, 'godzilla_voice_e_028_RAGE.wav'), [v], r)
    r = 48000; notes = [262, 330, 392, 523, 392, 330]; L = []; R = []
    for k in range(14 * 2):
        t = tone(notes[k % 6], 0.5, r, 0.4); L += t; R += tone(notes[(k + 2) % 6] / 2.0, 0.5, r, 0.3)
    sp.wav_write(os.path.join(out, 'fight1.wav'), [L, R], r)
    print('wrote 3 synthetic WAVs to', out)

if __name__ == '__main__':
    main()
