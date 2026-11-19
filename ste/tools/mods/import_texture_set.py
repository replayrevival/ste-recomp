# SPDX-License-Identifier: GPL-2.0-or-later
"""import_texture_set.py: turn an outside set of replacement textures (PNGs named after the game's textures) into a STE texture-pack mod (docs\\formats\\texturepack.md, texturepack_import.md).

Own code. Reads the user's own Xbox game files READ-ONLY (bundle files .cmf/.bdl/.clf, also inside the bundle .zip files); writes only to the folders you name. Needs Python 3.9+ and Pillow.

  index  <xbox game dir> <out index.csv>
        one row per TEXTURE chunk of every bundle: bundle, texture name, D3D format word, width, height, level-0 bytes, palette name, key16 (the texture-pack key the renderer computes).
  import <set: folder of PNGs | mapping.csv(name,file)> <index.csv> <out pack dir> [--id ID --name NAME --credits TEXT --max-dim N]
        matches PNG names to texture names (case-insensitive; 'Gigan_G2_512_C.png' or 'Gigan_G2_512_C_anything.png'), writes <out>\\<key16>_<name>.png (8-bit RGBA), manifest.json and report.csv
        (status per input: matched / unmatched / size-mismatch / ambiguous). Size mismatch = a different aspect ratio or a non power of two size (still written, flagged).
  sample <index.csv> <out pack dir>
        builds a tiny pack from SYNTHETIC images (gradients) for the first 3 non-tiny textures of the index: for testing the Mods tab / the loader; contains no game imagery.
The key: key = mix(hash(level-0 texel bytes) ^ hash(palette 1024 bytes, P8 only) ^ D3D format word * 0x9E3779B97F4A7C15 ^ w<<48 ^ h<<32) as in nv2a kelvin_texpack.c texpack_key().
Level-0 byte counts are known for DXT1/DXT2-5, A8R8G8B8/X8R8G8B8, 16-bit and 8-bit formats; other formats are listed with level0=0 and no key."""
import csv, hashlib, io, json, os, struct, sys, zipfile

M64 = (1 << 64) - 1
P = 1099511628211

def hash_bytes(p):
    n = len(p); h0 = 1469598103934665603; h1 = h0 ^ 0x9E3779B97F4A7C15; h2 = h0 ^ 0xC2B2AE3D27D4EB4F; h3 = h0 ^ 0x165667B19E3779F9
    words = n // 8; q = struct.unpack('<%dQ' % words, p[:words * 8]); i = 0
    while i + 4 <= words:
        h0 = ((h0 ^ q[i]) * P) & M64; h1 = ((h1 ^ q[i + 1]) * P) & M64; h2 = ((h2 ^ q[i + 2]) * P) & M64; h3 = ((h3 ^ q[i + 3]) * P) & M64; i += 4
    while i < words:
        h0 = ((h0 ^ q[i]) * P) & M64; i += 1
    for j in range(words * 8, n): h0 = ((h0 ^ p[j]) * P) & M64
    h0 = ((h0 ^ (h1 >> 29) ^ (h1 << 35)) * P) & M64; h0 = ((h0 ^ (h2 >> 31) ^ (h2 << 33)) * P) & M64; h0 = ((h0 ^ (h3 >> 27) ^ (h3 << 37)) * P) & M64
    return (h0 ^ (h0 >> 32)) & M64

def texpack_key(h, fmt, w, hh):
    k = h ^ ((fmt * 0x9E3779B97F4A7C15) & M64) ^ ((w << 48) & M64) ^ ((hh << 32) & M64)
    k ^= k >> 29; k = (k * 0xBF58476D1CE4E5B9) & M64; k ^= k >> 32
    return k

# bytes per pixel by the D3D color code (bits 8..15 of the format word); None = not handled
BPP = {0x0C: 0.5, 0x0E: 1.0, 0x0F: 1.0, 0x06: 4.0, 0x07: 4.0, 0x3A: 4.0, 0x3F: 4.0, 0x05: 2.0, 0x04: 2.0, 0x02: 1.0, 0x01: 1.0, 0x00: 1.0, 0x0B: 1.0, 0x19: 1.0, 0x1A: 1.0, 0x1B: 2.0, 0x3C: 4.0}

LINEAR = {0x10, 0x11, 0x12, 0x1D, 0x1E, 0x1F, 0x2E, 0x2F}   # D3DFMT_LIN_* colour codes seen in the game's bundles

class Bundle:
    """Reader for 'Pipeworks Bundle File version 1.3' (layout: docs\\formats\\pipeworks_bundle.md and own observations of the STE Xbox files)."""
    def __init__(self, data):
        self.d = d = data
        if not d.startswith(b'Pipeworks Bundle File'): raise ValueError('not a Pipeworks bundle')
        st, = struct.unpack_from('<I', d, 0x34)
        n, = struct.unpack_from('<I', d, st)
        offs = struct.unpack_from('<%dI' % n, d, st + 4)
        self.strings = [d[st + o:d.index(b'\0', st + o)].decode('latin-1') for o in offs]
        self.strings = [t[4:] if t[:4] == '\x13000' else t for t in self.strings]   # monster (.cmf) bundles put the 4 bytes 13 30 30 30 in front of every string
        sec0, = struct.unpack_from('<I', d, 0x38)
        sid, cnt, info, mo, ms, go, gs = struct.unpack_from('<HHIIIII', d, sec0)
        self.chunks = []
        rec = sec0 + 24   # chunk records follow the section header: u16 index, u32 main_rel, u32 main_size, u32 gs_rel, u32 gs_size
        for i in range(cnt):
            idx, mrel, msz, grel, gsz = struct.unpack_from('<HIIII', d, rec + i * 18)
            ci, typ, name, u1, u2 = struct.unpack_from('<HHIII', d, info + i * 16)
            self.chunks.append((typ, self.strings[name] if name < len(self.strings) else '<%d>' % name, mo + mrel, msz, go + grel, gsz))

def bundles(game_dir):
    for dp, _, fs in os.walk(game_dir):
        for f in sorted(fs):
            p = os.path.join(dp, f); e = f.lower().rsplit('.', 1)[-1]
            if e in ('cmf', 'bdl', 'clf'):
                try: yield f, Bundle(open(p, 'rb').read())
                except Exception: pass
            elif e == 'zip':
                try:
                    z = zipfile.ZipFile(p)
                    for m in z.namelist():
                        if m.lower().rsplit('.', 1)[-1] in ('cmf', 'bdl', 'clf'):
                            try: yield f + '!' + m, Bundle(z.read(m))
                            except Exception: pass
                except Exception: pass

def cmd_index(a):
    rows = []
    for bname, b in bundles(a[0]):
        pal = {c[1].lower(): c for c in b.chunks if c[0] == 0x0D}
        for typ, name, mo, ms, go, gs in b.chunks:
            if typ != 9 or ms < 20: continue
            com, data, lock, fmt, size = struct.unpack_from('<5I', b.d, mo)
            if fmt in (0, 0xFFFFFFFF) or not gs: continue
            col = (fmt >> 8) & 0xFF; w = 1 << ((fmt >> 20) & 0xF); h = 1 << ((fmt >> 24) & 0xF)
            bpp = BPP.get(col); lvl0 = int(w * h * bpp) if bpp else 0
            if col in LINEAR:   # linear (unswizzled) textures: the size comes from the Size word: width-1 (bits 0-11), height-1 (12-23), pitch/64-1 (24-31)
                w = (size & 0xFFF) + 1; h = ((size >> 12) & 0xFFF) + 1; lvl0 = ((size >> 24) + 1) * 64 * h
            palname = ''; key = ''
            if lvl0 and lvl0 <= gs:
                hs = hash_bytes(b.d[go:go + lvl0])
                pc = pal.get((name + '.pal').lower())
                if col == 0x0B and pc:
                    hs ^= hash_bytes(b.d[pc[4]:pc[4] + 1024]); palname = pc[1]
                key = '%016x' % texpack_key(hs, fmt, w, h)
            rows.append((bname, name, '%08X' % fmt, w, h, lvl0, palname, key))
    with open(a[1], 'w', newline='', encoding='utf-8') as o:
        wr = csv.writer(o); wr.writerow(['bundle', 'name', 'd3d_format', 'width', 'height', 'level0_bytes', 'palette', 'key16']); wr.writerows(rows)
    print(len(rows), 'textures,', sum(1 for r in rows if r[7]), 'with a key ->', a[1])

def load_index(path):
    by = {}
    for r in csv.DictReader(open(path, encoding='utf-8')):
        by.setdefault(r['name'].lower(), []).append(r)
    return by

def cmd_import(a):
    from PIL import Image
    src, idxp, out = a[0], a[1], a[2]
    opt = {a[i]: a[i + 1] for i in range(3, len(a) - 1, 2) if a[i].startswith('--')}
    maxdim = int(opt.get('--max-dim', 8192)); idx = load_index(idxp)
    os.makedirs(out, exist_ok=True)
    files = []
    if os.path.isdir(src):
        for dp, _, fs in os.walk(src):
            for f in sorted(fs):
                if f.lower().endswith('.png'): files.append((os.path.splitext(f)[0], os.path.join(dp, f)))
    else:
        for r in csv.reader(open(src, encoding='utf-8')):
            if r and not r[0].startswith('#') and r[0].lower() != 'name': files.append((r[0], r[1]))
    report = []; done = set()
    for stem, path in files:
        cand = idx.get(stem.lower())
        if not cand:
            base = stem.lower()
            while not cand and '_' in base:
                base = base.rsplit('_', 1)[0]; cand = idx.get(base)
        if not cand: report.append((stem, '', '', 'unmatched', '')); continue
        keys = sorted(set(r['key16'] for r in cand if r['key16']))
        if not keys: report.append((stem, '', '', 'no-key (format not handled)', '')); continue
        im = Image.open(path).convert('RGBA'); gw, gh = int(cand[0]['width']), int(cand[0]['height'])
        notes = []
        if im.width > maxdim or im.height > maxdim: im.thumbnail((maxdim, maxdim)); notes.append('shrunk to max-dim')
        if abs(im.width * gh - im.height * gw) > 1: notes.append('aspect differs from %dx%d' % (gw, gh))
        if im.width & (im.width - 1) or im.height & (im.height - 1): notes.append('not a power of two')
        status = 'size-mismatch' if notes else 'matched'
        if len(keys) > 1: status = 'ambiguous (%d keys, all written)' % len(keys)
        for k in keys:
            if k in done: continue
            done.add(k); im.save(os.path.join(out, '%s_%s.png' % (k, stem)))
        report.append((stem, ';'.join(keys), '%dx%d -> %dx%d' % (im.width, im.height, gw, gh), status, '; '.join(notes)))
    man = {'id': opt.get('--id', 'texture-set'), 'name': opt.get('--name', 'Texture set'), 'version': '1.0', 'profile': 'textures', 'enabled': True, 'priority': 100,
           'credits': opt.get('--credits', 'SET YOUR SOURCE AND LICENCE HERE')}
    json.dump(man, open(os.path.join(out, 'manifest.json'), 'w'), indent=2)
    with open(os.path.join(out, 'report.csv'), 'w', newline='', encoding='utf-8') as o:
        wr = csv.writer(o); wr.writerow(['input', 'keys', 'size', 'status', 'notes']); wr.writerows(report)
    c = {}
    for r in report: c[r[3].split(' ')[0]] = c.get(r[3].split(' ')[0], 0) + 1
    print('inputs', len(files), c, '->', out)

def cmd_sample(a):
    from PIL import Image
    idx = [r for rows in load_index(a[0]).values() for r in rows if r['key16'] and int(r['width']) >= 64 and int(r['height']) >= 64][:3]
    tmp = a[1].rstrip('\\/') + '_src'; os.makedirs(tmp, exist_ok=True)   # the source images go next to the pack, not into it
    for n, r in enumerate(idx):
        w, h = int(r['width']), int(r['height'])
        im = Image.new('RGBA', (w, h))
        px = im.load()
        for y in range(h):
            for x in range(w): px[x, y] = ((x * 255) // w, (y * 255) // h, 64 + 60 * n, 255)
        im.save(os.path.join(tmp, r['name'] + '.png'))
    cmd_import([tmp, a[0], a[1], '--id', 'sample-synthetic', '--name', 'Sample texture set (synthetic gradients)', '--credits', 'Synthetic test images generated by import_texture_set.py (GPL-2.0-or-later); no game imagery.'])

if __name__ == '__main__':
    c = sys.argv[1] if len(sys.argv) > 1 else ''
    {'index': cmd_index, 'import': cmd_import, 'sample': cmd_sample}.get(c, lambda a: print(__doc__))(sys.argv[2:])
