# SPDX-License-Identifier: GPL-2.0-or-later
"""sound_pack.py: export the original sounds of Godzilla: Save the Earth (Xbox) as WAV files, and build a sound-pack mod from edited WAVs.

Own code. Reads the user's own game folder or Xbox disc image (.iso) READ-ONLY; writes only to the folders you name.
Requirements: Python 3.9+ (no other package needed). Windows paths work as written.

  export <game folder | .iso> <out folder> [--match WORDS] [--category effects|voices|music|monsters] [--list]
        every sound as <bank>_<slot>_<cue>.wav (banks) or <track>.wav (music), 16-bit PCM, mono (banks) / stereo 48 kHz (music), plus export_report.csv.
        --match: comma-separated words, case-insensitive substring of the file name. --list prints the names only.
  import <folder of WAVs> <game folder | .iso> <pack folder> [--id ID --name NAME --credits TEXT]
        checks every WAV against the game's sounds (matched / unmatched / format-refused / notes), copies the usable ones into <pack folder>
        with a manifest.json ("profile": "sounds") and writes report.csv. Install the pack folder with the launcher's Mods tab.
Naming: keep the exported file names (the part before the cue name is the id: <bank>_<3-digit slot> or the music track name).
Replacement WAVs: PCM 16-bit, mono or stereo, any sample rate. Loops: end the file name with _loop (my_sound_loop.wav) or give the WAV a 'smpl' loop (editors such as Wavosaur write one; Audacity does not) to make a bank sound loop.
Share only audio you made or have the rights to; packs with copyrighted music or game voice clips are for personal use only."""
import csv, io, json, os, re, struct, sys

BANKS = {'effects': ['godzilla_sound', 'shell_sound', 'rail_fpm'], 'voices': ['godzilla_voice_e', 'godzilla_voice_j']}
MAX_SECONDS = 60

# ---------------------------------------------------------------- Xbox IMA ADPCM
STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
        724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
        18500, 20350, 22385, 24623, 27086, 29794, 32767]
IDX = [-1, -1, -1, -1, 2, 4, 6, 8]

def adpcm_decode(data, channels):
    """Xbox ADPCM bytes -> list of per-channel int16 lists (64 samples per block per channel; the 65th decoded value belongs to the next block)."""
    bs = 36 * channels; out = [[] for _ in range(channels)]
    for b0 in range(0, len(data) - bs + 1, bs):
        blk = data[b0:b0 + bs]; pred = []; idx = []
        for c in range(channels):
            p, i = struct.unpack_from('<hB', blk, c * 4); pred.append(p); idx.append(min(i, 88))
        dec = [[p] for p in pred]; pos = 4 * channels
        while pos + 4 * channels <= bs:
            for c in range(channels):
                for k in range(4):
                    byte = blk[pos]; pos += 1
                    for nib in (byte & 15, byte >> 4):
                        step = STEP[idx[c]]; d = step >> 3
                        if nib & 1: d += step >> 2
                        if nib & 2: d += step >> 1
                        if nib & 4: d += step
                        if nib & 8: d = -d
                        pred[c] = max(-32768, min(32767, pred[c] + d)); idx[c] = max(0, min(88, idx[c] + IDX[nib & 7])); dec[c].append(pred[c])
        for c in range(channels): out[c].extend(dec[c][:64])
    return out

def adpcm_encode(chans):
    """per-channel int16 lists (same length) -> Xbox ADPCM bytes (blocks of 64 samples per channel; the tail is padded with the last sample)."""
    n = len(chans[0]); nch = len(chans); blocks = (n + 63) // 64; out = bytearray()
    idx = [0] * nch
    for b in range(blocks):
        seg = []
        for c in range(nch):
            s = chans[c][b * 64:b * 64 + 65]
            s = s + [chans[c][-1] if chans[c] else 0] * (65 - len(s)); seg.append(s)
        hdr = bytearray(); body = [bytearray() for _ in range(nch)]; state = []
        for c in range(nch):
            hdr += struct.pack('<hBB', seg[c][0], idx[c], 0); state.append([seg[c][0], idx[c]])
        nibs = [[] for _ in range(nch)]
        for c in range(nch):
            pred, ix = state[c]
            for k in range(1, 65):
                diff = seg[c][k] - pred; nib = 0
                if diff < 0: nib = 8; diff = -diff
                step = STEP[ix]; d = step >> 3
                if diff >= step: nib |= 4; diff -= step; d += step
                step >>= 1
                if diff >= step: nib |= 2; diff -= step; d += step
                step >>= 1
                if diff >= step: nib |= 1; d += step
                pred = max(-32768, min(32767, pred + (-d if nib & 8 else d))); ix = max(0, min(88, ix + IDX[nib & 7])); nibs[c].append(nib)
            idx[c] = ix   # the next block starts from the encoder's index (its header carries the true first sample)
        out += hdr
        for g in range(8):
            for c in range(nch):
                for k in range(4):
                    out.append(nibs[c][g * 8 + k * 2] | (nibs[c][g * 8 + k * 2 + 1] << 4))
    return bytes(out)

# ---------------------------------------------------------------- WAV
def wav_write(path, chans, rate, loop=None):
    n = len(chans[0]); nch = len(chans); pcm = bytearray()
    for i in range(n):
        for c in range(nch): pcm += struct.pack('<h', chans[c][i])
    fmt = struct.pack('<HHIIHH', 1, nch, rate, rate * nch * 2, nch * 2, 16)
    extra = b''
    if loop: extra = b'smpl' + struct.pack('<I', 60) + struct.pack('<9I', 0, 0, 0, 60, 0, 0, 0, 1, 0) + struct.pack('<6I', 0, 0, loop[0], loop[1], 0, 0)
    body = b'WAVE' + b'fmt ' + struct.pack('<I', 16) + fmt + b'data' + struct.pack('<I', len(pcm)) + bytes(pcm) + (b'\0' if len(pcm) & 1 else b'') + extra
    open(path, 'wb').write(b'RIFF' + struct.pack('<I', len(body)) + body)

class WavError(Exception): pass

def wav_read(data):
    """-> (channels list of int16 lists, rate, loop (start, end) or None). Raises WavError with a plain message."""
    if data[:4] != b'RIFF' or data[8:12] != b'WAVE': raise WavError('not a WAV file')
    pos = 12; fmt = None; pcm = None; loop = None
    while pos + 8 <= len(data):
        tag = data[pos:pos + 4]; size, = struct.unpack_from('<I', data, pos + 4); body = data[pos + 8:pos + 8 + size]
        if tag == b'fmt ': fmt = struct.unpack_from('<HHIIHH', body, 0)
        elif tag == b'data': pcm = body
        elif tag == b'smpl' and len(body) >= 60 and struct.unpack_from('<I', body, 28)[0] >= 1: loop = struct.unpack_from('<II', body, 44)
        pos += 8 + size + (size & 1)
    if not fmt or pcm is None: raise WavError('no fmt/data chunk')
    tag, nch, rate, _, _, bits = fmt
    if tag != 1: raise WavError('format %s is not supported: use PCM 16-bit (not float, not compressed)' % ('0x%04X' % tag))
    if bits != 16: raise WavError('%d-bit WAV is not supported: use 16-bit PCM' % bits)
    if nch not in (1, 2): raise WavError('%d channels: use mono or stereo' % nch)
    n = len(pcm) // (2 * nch); v = struct.unpack_from('<%dh' % (n * nch), pcm, 0)
    return [list(v[c::nch]) for c in range(nch)], rate, loop

# ---------------------------------------------------------------- game files
class _Folder:
    def __init__(self, p): self.p = p
    def get(self, rel):
        for cand in (rel, rel.lower()):
            f = os.path.join(self.p, *cand.split('/'))
            if os.path.isfile(f): return open(f, 'rb').read()
        d = os.path.join(self.p, rel.split('/')[0])
        if os.path.isdir(d):
            for n in os.listdir(d):
                if n.lower() == rel.split('/')[-1].lower(): return open(os.path.join(d, n), 'rb').read()
        return None
    def list(self, sub, exts):
        d = os.path.join(self.p, sub)
        return sorted(f for f in (os.listdir(d) if os.path.isdir(d) else []) if f.lower().rsplit('.', 1)[-1] in exts)

class _Xiso:
    OFFS = (0, 0x18300000, 0x0FD90000, 0x02090000)
    def __init__(self, p):
        self.f = open(p, 'rb'); self.base = None
        for off in self.OFFS:
            self.f.seek(off + 32 * 2048); b = self.f.read(2048)
            if b[:20] == b'MICROSOFT*XBOX*MEDIA': self.base = off; self.root, self.rsize = struct.unpack_from('<II', b, 20); break
        if self.base is None: raise ValueError('not an Xbox disc image (no XDVDFS volume descriptor)')
    def _dir(self, sector, size):
        self.f.seek(self.base + sector * 2048); data = self.f.read(size); seen = set(); stack = [0]
        while stack:
            o = stack.pop()
            if o in seen or o + 14 > len(data): continue
            seen.add(o); l, r, sec, sz, attr, nl = struct.unpack_from('<HHIIBB', data, o)
            if l == 0xFFFF and r == 0xFFFF and sec == 0xFFFFFFFF: continue
            yield data[o + 14:o + 14 + nl].decode('latin-1'), sec, sz, attr
            for t in (l, r):
                if t and t != 0xFFFF: stack.append(t * 4)
    def _find(self, parts):
        sec, size = self.root, self.rsize
        for i, p in enumerate(parts):
            hit = None
            for name, s, z, a in self._dir(sec, size):
                if name.lower() == p.lower(): hit = (s, z, a); break
            if not hit: return None
            if i < len(parts) - 1:
                if not hit[2] & 0x10: return None
                sec, size = hit[0], hit[1]
            else: return hit
    def get(self, rel):
        h = self._find(rel.split('/'))
        if not h or h[2] & 0x10: return None
        self.f.seek(self.base + h[0] * 2048); return self.f.read(h[1])
    def list(self, sub, exts):
        h = self._find([sub]); out = []
        if not h: return out
        for name, s, z, a in self._dir(h[0], h[1]):
            if not a & 0x10 and name.lower().rsplit('.', 1)[-1] in exts: out.append(name)
        return sorted(out)

def open_game(path):
    if os.path.isdir(path): return _Folder(path)
    if os.path.isfile(path): return _Xiso(path)
    raise ValueError('not found: %s' % path)

def parse_bank(uber, samp):
    """-> list of dicts: slot, name (cue name or ''), rate, data (ADPCM bytes), loop flag, header"""
    offs = struct.unpack_from('<5I', uber, 12); pool_o, sdir_o = offs[0], offs[1]
    cnt = struct.unpack_from('<I', uber, sdir_o + 12)[0]
    slots = [struct.unpack_from('<4I', uber, sdir_o + 16 + i * 16) for i in range(cnt)]
    ncue = struct.unpack_from('<I', uber, pool_o + 8)[0]; poffs = struct.unpack_from('<%dI' % ncue, uber, pool_o + 16)
    nn = struct.unpack_from('<I', uber, offs[4])[0]; no = struct.unpack_from('<%dI' % nn, uber, offs[4] + 4)
    names = [uber[x:uber.index(b'\0', x)].decode('latin-1') for x in no]
    cue_of = {}
    for i in range(min(ncue, len(names))):
        rec = uber[pool_o + poffs[i]:pool_o + (poffs[i + 1] if i + 1 < ncue else sdir_o - pool_o)]
        for j in range(0, len(rec) - 7):
            if rec[j:j + 2] == b'\x10\x00' and rec[j + 4:j + 8] == b'\0\0\0\0': cue_of.setdefault(struct.unpack_from('<H', rec, j + 2)[0], names[i])
    out = []
    for k, (do, dl, fl, ho) in enumerate(slots):
        if do == 0xFFFFFFFF or dl == 0: continue
        tag, ch, rate = struct.unpack_from('<HHI', samp, ho)
        out.append({'slot': k, 'name': cue_of.get(k, ''), 'rate': rate, 'ch': ch, 'tag': tag, 'data': samp[do:do + dl], 'loop': fl, 'hdr_off': ho, 'data_off': do})
    return out

def safe(n): return re.sub(r'[^A-Za-z0-9_.+()-]+', '_', n).strip('_')

def category_of(f):
    return 'effects' if f in BANKS['effects'] else 'voices' if f in BANKS['voices'] else None

def music_files(g):
    return [f for f in g.list('SoundData', ('wxv',))]

def wxv_decode(data):
    tag, nch, rate = struct.unpack_from('<HHI', data, 0x14)
    if data[:4] != b'RIFF' or tag != 0x69: raise WavError('not an Xbox ADPCM WAV')
    pos = 12
    while pos + 8 <= len(data):
        t = data[pos:pos + 4]; size, = struct.unpack_from('<I', data, pos + 4)
        if t == b'data': return adpcm_decode(data[pos + 8:pos + 8 + size], nch), rate
        pos += 8 + size + (size & 1)
    raise WavError('no data chunk')

def music_category(name):
    n = name.lower().rsplit('.', 1)[0]
    return 'music' if re.match(r'(fight\d+|menu|results|victory\d|defeat\d)$', n) else 'monsters'

# ---------------------------------------------------------------- export
def cmd_export(a):
    import argparse
    ap = argparse.ArgumentParser(prog='sound_pack.py export', add_help=False)
    ap.add_argument('game'); ap.add_argument('out', nargs='?'); ap.add_argument('--match', default=''); ap.add_argument('--category', default=''); ap.add_argument('--list', action='store_true')
    try: o = ap.parse_args(a)
    except SystemExit: print('Could not read the arguments.\n\n' + __doc__); return 2
    words = [w.strip().lower() for w in o.match.split(',') if w.strip()]
    if o.category and o.category not in ('effects', 'voices', 'music', 'monsters'): print('--category must be effects, voices, music or monsters'); return 2
    if not o.list and not o.out: print('Give an output folder as the second argument (or use --list).'); return 2
    try: g = open_game(o.game)
    except Exception as e: print('Cannot read the game: %s' % e); return 2
    jobs = []   # (category, file name, kind, payload)
    for bank in BANKS['effects'] + BANKS['voices']:
        cat = category_of(bank)
        if o.category and o.category != cat: continue
        u = g.get('SoundData/%s.uber' % bank); s = g.get('SoundData/%s.samp' % bank)
        if not u or not s: continue
        for sm in parse_bank(u, s):
            fn = '%s_%03d_%s' % (bank, sm['slot'], safe(sm['name']) if sm['name'] else 'unnamed')
            if words and not any(w in fn.lower() for w in words): continue
            jobs.append((cat, fn, 'bank', sm))
    for f in music_files(g):
        cat = music_category(f)
        if o.category and o.category != cat: continue
        fn = f.rsplit('.', 1)[0]
        if words and not any(w in fn.lower() for w in words): continue
        jobs.append((cat, fn, 'music', f))
    if not jobs:
        print('Nothing matches. Known banks: %s; music/monster tracks are the .wxv files of SoundData.' % ', '.join(BANKS['effects'] + BANKS['voices'])); return 1
    if o.list:
        for cat, fn, kind, p in jobs: print('%-9s %s' % (cat, fn))
        print('%d sounds.' % len(jobs)); return 0
    os.makedirs(o.out, exist_ok=True); rep = []; nw = 0
    for cat, fn, kind, p in jobs:
        try:
            if kind == 'bank':
                ch = adpcm_decode(p['data'], 1); loop = (0, len(ch[0]) - 1) if p['loop'] else None
                wav_write(os.path.join(o.out, fn + '.wav'), ch, p['rate'], loop); rep.append((fn, cat, '%d Hz mono' % p['rate'], '%.2f s' % (len(ch[0]) / p['rate']), fn + '.wav', 'written', 'loops' if loop else ''))
            else:
                ch, rate = wxv_decode(g.get('SoundData/' + p)); wav_write(os.path.join(o.out, fn + '.wav'), ch, rate)
                rep.append((fn, cat, '%d Hz stereo' % rate, '%.1f s' % (len(ch[0]) / rate), fn + '.wav', 'written', ''))
            nw += 1
        except Exception as e: rep.append((fn, cat, '', '', '', 'skipped', str(e)))
    with open(os.path.join(o.out, 'export_report.csv'), 'w', newline='', encoding='utf-8') as f:
        w = csv.writer(f); w.writerow(['name', 'category', 'format', 'length', 'file', 'status', 'note']); w.writerows(rep)
    print('Exported %d of %d sound(s) to %s' % (nw, len(jobs), o.out))
    print('Report: %s' % os.path.join(o.out, 'export_report.csv'))
    print('Next: edit the WAVs (keep the file names), then run:')
    print('  python -I sound_pack.py import %s <game folder or .iso> <pack folder> --id my-sounds --name "My sounds"' % o.out)
    return 0

# ---------------------------------------------------------------- import (build the pack)
ID_BANK = re.compile(r'^(godzilla_sound|shell_sound|rail_fpm|godzilla_voice_e|godzilla_voice_j)_(\d{3})(?:_.*)?$', re.I)

def cmd_import(a):
    if len(a) < 3: print(__doc__); return 2
    src, game, out = a[0], a[1], a[2]
    opt = {a[i]: a[i + 1] for i in range(3, len(a) - 1, 2) if a[i].startswith('--')}
    if not os.path.isdir(src): print('Not a folder: %s' % src); return 2
    try: g = open_game(game)
    except Exception as e: print('Cannot read the game: %s' % e); return 2
    slots = {}
    for bank in BANKS['effects'] + BANKS['voices']:
        u = g.get('SoundData/%s.uber' % bank); s = g.get('SoundData/%s.samp' % bank)
        if u and s: slots[bank] = {sm['slot']: sm for sm in parse_bank(u, s)}
    music = {f.rsplit('.', 1)[0].lower(): f for f in music_files(g)}
    if not slots and not music: print('No game sound files found in %s (SoundData). Is this the Xbox Godzilla: Save the Earth game?' % game); return 2
    if os.path.exists(os.path.join(out, 'manifest.json')): print('%s already holds a pack; choose a new folder.' % out); return 2
    os.makedirs(out, exist_ok=True); rep = []; used = set()
    for f in sorted(os.listdir(src)):
        if not f.lower().endswith('.wav'): continue
        stem = f[:-4]; data = open(os.path.join(src, f), 'rb').read(); note = []
        m = ID_BANK.match(stem); ident = None
        if m and int(m.group(2)) in slots.get(m.group(1).lower(), {}): ident = '%s_%s' % (m.group(1).lower(), m.group(2))
        elif stem.lower() in music: ident = 'music:' + stem.lower()
        else:
            rep.append((f, '', 'unmatched', 'no sound of the game has this id (keep the exported file name)')); continue
        try: ch, rate, loop = wav_read(data)
        except WavError as e: rep.append((f, ident, 'format-refused', str(e))); continue
        secs = len(ch[0]) / float(rate)
        if secs > MAX_SECONDS: rep.append((f, ident, 'refused', 'longer than %d s (%.1f s)' % (MAX_SECONDS, secs))); continue
        if ident in used: rep.append((f, ident, 'duplicate', 'another file already replaces this sound')); continue
        if ident.startswith('music:'):
            if len(ch) == 1: note.append('mono: will play on both sides')
        else:
            if len(ch) == 2: note.append('stereo: mixed down to mono (game sounds are mono)')
            if loop and (loop[0] > 0 or loop[1] < len(ch[0]) - 2): note.append('partial loop ignored: the whole sound loops')
            if stem.lower().endswith('_loop'): note.append('loops (file name ends with _loop)')
            orig = slots[m.group(1).lower()][int(m.group(2))]
            if abs(secs - len(adpcm_decode(orig['data'], 1)[0]) / float(orig['rate'])) > 0.25: note.append('length differs from the original (%.2f s vs %.2f s)' % (secs, len(adpcm_decode(orig['data'], 1)[0]) / float(orig['rate'])))
        used.add(ident); os.makedirs(os.path.join(out, 'sounds'), exist_ok=True)
        open(os.path.join(out, 'sounds', f), 'wb').write(data); rep.append((f, ident, 'matched', '; '.join(note)))
    man = {'id': opt.get('--id', 'sound-set'), 'name': opt.get('--name', 'Sound set'), 'version': '1.0', 'profile': 'sounds', 'enabled': True, 'priority': 100,
           'credits': opt.get('--credits', 'SET YOUR SOURCE AND LICENCE HERE')}
    json.dump(man, open(os.path.join(out, 'manifest.json'), 'w'), indent=2)
    with open(os.path.join(out, 'report.csv'), 'w', newline='', encoding='utf-8') as fh:
        w = csv.writer(fh); w.writerow(['file', 'sound', 'status', 'notes']); w.writerows(rep)
    c = {}
    for r in rep: c[r[2]] = c.get(r[2], 0) + 1
    print('WAV files: %d %s -> %s' % (len(rep), c, out))
    for r in rep:
        if r[2] != 'matched': print('  %s: %s (%s)' % (r[0], r[2], r[3]))
    if c.get('matched'): print('Next: install the folder %s with the launcher Mods tab and switch it on. Share only audio you made or have the rights to.' % out)
    return 0

def main():
    c = sys.argv[1] if len(sys.argv) > 1 else ''
    try:
        if c == 'export': sys.exit(cmd_export(sys.argv[2:]))
        if c == 'import': sys.exit(cmd_import(sys.argv[2:]))
    except KeyboardInterrupt: sys.exit(130)
    except Exception as e: print('%s failed: %s: %s' % (c, type(e).__name__, e)); sys.exit(2)
    print(__doc__)

if __name__ == '__main__':
    main()
