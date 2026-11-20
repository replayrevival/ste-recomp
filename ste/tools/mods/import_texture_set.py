# SPDX-License-Identifier: GPL-2.0-or-later
"""import_texture_set.py: turn an outside set of replacement textures (PNGs named after the game's textures) into a STE texture-pack mod (docs\\formats\\texturepack.md, texturepack_import.md).

Own code. Reads the user's own Xbox game files READ-ONLY (bundle files .cmf/.bdl/.clf, also inside the bundle .zip files); writes only to the folders you name. Needs Python 3.9+ and Pillow.

  index  <xbox game dir> <out index.csv>
        one row per TEXTURE chunk of every bundle: bundle, texture name, D3D format word, width, height, level-0 bytes, palette name, key16 (the texture-pack key the renderer computes).
  import <set: folder of PNGs | mapping.csv(name,file)> <index.csv | game folder | game .iso> <out pack dir> [--id ID --name NAME --credits TEXT --max-dim N]
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

def open_source(path):
    if os.path.isdir(path): return _Folder(path)
    if os.path.isfile(path): return _Xiso(path)
    raise ValueError('not found: %s' % path)

def index_rows(path):
    rows = []
    for bname, b in _bundle_iter(open_source(path)):
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
    return rows

def cmd_index(a):
    rows = index_rows(a[0])
    with open(a[1], 'w', newline='', encoding='utf-8') as o:
        wr = csv.writer(o); wr.writerow(['bundle', 'name', 'd3d_format', 'width', 'height', 'level0_bytes', 'palette', 'key16']); wr.writerows(rows)
    print(len(rows), 'textures,', sum(1 for r in rows if r[7]), 'with a key ->', a[1])

def load_index(path):
    by = {}
    if path.lower().endswith('.csv'): rows = list(csv.DictReader(open(path, encoding='utf-8')))
    else:   # a game folder or an Xbox disc image: build the index on the fly (about 7 s)
        print('Reading the game textures from %s (about 10 seconds)...' % path)
        rows = [dict(zip(['bundle', 'name', 'd3d_format', 'width', 'height', 'level0_bytes', 'palette', 'key16'], r)) for r in index_rows(path)]
    for r in rows:
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
    if done: print("Next: put the folder %s into the launcher Mods tab (Install from folder) and switch it on, or copy it into the game's texturepacks folder." % out)

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

# ---------------------------------------------------------------- export (original textures -> named PNGs)
import array, difflib, re

EXPORT_HELP = """export: write the game's ORIGINAL textures as named PNGs (level 0, RGBA) so you can retexture them, then use 'import'.

  python -I import_texture_set.py export <game folder | game .iso> <out folder> --match WORDS [--list] [--monster NAME]

  --match WORDS   comma-separated words; a texture is exported when its name contains any word (case-insensitive). Example: --match Gigan
  --monster NAME  EVERY texture of one monster: all textures in its own bundle (skin colour + bump map, weapons, effects, face) and its HUD bundle;
                  --monster ? lists the monster names found in the game (for example Gigan, Godzilla2k, MechaGodzilla3)
  --list          only print the matching texture names (nothing is written)
Writes <out>\\<Name>.png for every texture and <out>\\export_report.csv (name, size, format, file, written/skipped, reason).
When one name exists with different pixels in several bundles (day/night, per-level copies) each is written as <Name>__<bundle>.png;
editing ANY of them and importing it under the base name changes all variants (import writes every key of the name).
Requires: Python 3.9+ and Pillow (pip install Pillow). Never writes into the game folder or image."""


def _swz_masks(w, h):
    x = y = 0; bit = 1; mb = 1; more = True
    while more:
        more = False
        if bit < w: x |= mb; mb <<= 1; more = True
        if bit < h: y |= mb; mb <<= 1; more = True
        bit <<= 1
    return x, y

def _spread(n, mask):
    t = []
    for v in range(n):
        o = 0; b = 0; m = mask; i = 0
        while m:
            if m & 1:
                if v >> b & 1: o |= 1 << i
                b += 1
            m >>= 1; i += 1
        t.append(o)
    return t

def _unswizzle(raw, w, h, bpp):
    mx, my = _swz_masks(w, h); tx = _spread(w, mx); ty = _spread(h, my); out = bytearray(w * h * bpp)
    for y in range(h):
        ry = ty[y]; base = y * w * bpp
        for x in range(w):
            s = (tx[x] | ry) * bpp; d = base + x * bpp
            out[d:d + bpp] = raw[s:s + bpp]
    return bytes(out)

def _rgb565(v): return ((v >> 11 & 31) * 255 // 31, (v >> 5 & 63) * 255 // 63, (v & 31) * 255 // 31)

def _dxt_colors(c0, c1, one):
    r0, g0, b0 = _rgb565(c0); r1, g1, b1 = _rgb565(c1)
    if c0 > c1 or not one:
        return [(r0, g0, b0, 255), (r1, g1, b1, 255), ((2 * r0 + r1) // 3, (2 * g0 + g1) // 3, (2 * b0 + b1) // 3, 255), ((r0 + 2 * r1) // 3, (g0 + 2 * g1) // 3, (b0 + 2 * b1) // 3, 255)]
    return [(r0, g0, b0, 255), (r1, g1, b1, 255), ((r0 + r1) // 2, (g0 + g1) // 2, (b0 + b1) // 2, 255), (0, 0, 0, 0)]

def _decode_dxt(raw, w, h, kind):
    out = bytearray(w * h * 4); bs = 8 if kind == 1 else 16; bw = max(1, w // 4); bh = max(1, h // 4); o = 0
    for by in range(bh):
        for bx in range(bw):
            blk = raw[o:o + bs]; o += bs
            if len(blk) < bs: continue
            cb = blk[bs - 8:]; c0, c1, bits = struct.unpack('<HHI', cb); col = _dxt_colors(c0, c1, kind == 1)
            if kind == 3: al = [((int.from_bytes(blk[:8], 'little') >> (4 * i)) & 15) * 17 for i in range(16)]
            elif kind == 5:
                a0, a1 = blk[0], blk[1]; ab = int.from_bytes(blk[2:8], 'little')
                tab = [a0, a1] + ([(( 6 - i) * a0 + (1 + i) * a1) // 7 for i in range(6)] if a0 > a1 else [((4 - i) * a0 + (1 + i) * a1) // 5 for i in range(4)] + [0, 255])
                al = [tab[(ab >> (3 * i)) & 7] for i in range(16)]
            else: al = None
            for i in range(16):
                px, py = bx * 4 + (i & 3), by * 4 + (i >> 2)
                if px >= w or py >= h: continue
                r, g, b, a = col[(bits >> (2 * i)) & 3]
                if al: a = al[i]
                d = (py * w + px) * 4; out[d] = r; out[d + 1] = g; out[d + 2] = b; out[d + 3] = a
    return bytes(out)

# colour code -> (name, bytes per pixel, swizzled)
FMT = {0x00: ('L8', 1, 1), 0x01: ('AL8', 1, 1), 0x02: ('A1R5G5B5', 2, 1), 0x03: ('X1R5G5B5', 2, 1), 0x04: ('A4R4G4B4', 2, 1), 0x05: ('R5G6B5', 2, 1), 0x06: ('A8R8G8B8', 4, 1), 0x07: ('X8R8G8B8', 4, 1),
       0x0B: ('P8', 1, 1), 0x0C: ('DXT1', 0, 0), 0x0E: ('DXT3', 0, 0), 0x0F: ('DXT5', 0, 0), 0x10: ('LIN_A1R5G5B5', 2, 0), 0x11: ('LIN_R5G6B5', 2, 0), 0x12: ('LIN_A8R8G8B8', 4, 0),
       0x19: ('A8', 1, 1), 0x1A: ('A8L8', 2, 1), 0x1C: ('LIN_X1R5G5B5', 2, 0), 0x1D: ('LIN_A4R4G4B4', 2, 0), 0x1E: ('LIN_X8R8G8B8', 4, 0), 0x1F: ('LIN_A8', 1, 0)}

def _rgba_from(col, px, w, h, pal):
    """px: unswizzled/linear tightly packed level-0 bytes -> RGBA bytes"""
    n = w * h; out = bytearray(n * 4)
    if col in (0x06, 0x12):
        out[0::4] = px[2::4]; out[1::4] = px[1::4]; out[2::4] = px[0::4]; out[3::4] = px[3::4]
    elif col in (0x07, 0x1E):
        out[0::4] = px[2::4]; out[1::4] = px[1::4]; out[2::4] = px[0::4]; out[3::4] = b'\xff' * n
    elif col in (0x02, 0x03, 0x04, 0x05, 0x10, 0x11, 0x1C, 0x1D, 0x1A):
        v = array.array('H'); v.frombytes(px[:n * 2])
        if sys.byteorder == 'big': v.byteswap()
        for i, s in enumerate(v):
            if col in (0x02, 0x10): c = ((s >> 10 & 31) * 255 // 31, (s >> 5 & 31) * 255 // 31, (s & 31) * 255 // 31, 255 if s >> 15 else 0)
            elif col in (0x03, 0x1C): c = ((s >> 10 & 31) * 255 // 31, (s >> 5 & 31) * 255 // 31, (s & 31) * 255 // 31, 255)
            elif col in (0x04, 0x1D): c = ((s >> 8 & 15) * 17, (s >> 4 & 15) * 17, (s & 15) * 17, (s >> 12) * 17)
            elif col == 0x1A: c = (s & 255, s & 255, s & 255, s >> 8)
            else: c = _rgb565(s) + (255,)
            out[i * 4:i * 4 + 4] = bytes(c)
    elif col == 0x00: out[0::4] = px[:n]; out[1::4] = px[:n]; out[2::4] = px[:n]; out[3::4] = b'\xff' * n
    elif col == 0x01: out[0::4] = px[:n]; out[1::4] = px[:n]; out[2::4] = px[:n]; out[3::4] = px[:n]
    elif col in (0x19, 0x1F): out[0::4] = b'\xff' * n; out[1::4] = b'\xff' * n; out[2::4] = b'\xff' * n; out[3::4] = px[:n]
    elif col == 0x0B:
        for i in range(n):
            o = px[i] * 4; out[i * 4] = pal[o + 2]; out[i * 4 + 1] = pal[o + 1]; out[i * 4 + 2] = pal[o]; out[i * 4 + 3] = pal[o + 3]
    return bytes(out)

def decode_texture(fmt, size, raw, pal):
    """-> (w, h, RGBA bytes) or raises ValueError(reason)"""
    col = (fmt >> 8) & 0xFF
    if col not in FMT: raise ValueError('format 0x%02X not supported' % col)
    name, bpp, swz = FMT[col]
    if col in LINEAR:
        w = (size & 0xFFF) + 1; h = ((size >> 12) & 0xFFF) + 1; pitch = ((size >> 24) + 1) * 64
    else:
        w = 1 << ((fmt >> 20) & 0xF); h = 1 << ((fmt >> 24) & 0xF); pitch = w * bpp
    if col in (0x0C, 0x0E, 0x0F):
        need = max(1, w // 4) * max(1, h // 4) * (8 if col == 0x0C else 16)
        if len(raw) < need: raise ValueError('data too short')
        return w, h, _decode_dxt(raw, w, h, {0x0C: 1, 0x0E: 3, 0x0F: 5}[col])
    if w * h > 4096 * 4096: raise ValueError('size not plausible')
    if col in LINEAR:
        if len(raw) < pitch * h: raise ValueError('data too short')
        px = b''.join(raw[y * pitch:y * pitch + w * bpp] for y in range(h))
    else:
        if len(raw) < w * h * bpp: raise ValueError('data too short')
        px = _unswizzle(raw, w, h, bpp)
    if col == 0x0B and (not pal or len(pal) < 1024): raise ValueError('palette missing')
    return w, h, _rgba_from(col, px, w, h, pal)

# ---- sources: a folder or an Xbox disc image (XDVDFS), read-only
class _Folder:
    def __init__(self, p): self.p = p
    def files(self):
        for dp, _, fs in os.walk(self.p):
            for f in sorted(fs):
                if f.lower().rsplit('.', 1)[-1] in ('cmf', 'bdl', 'clf', 'zip'):
                    full = os.path.join(dp, f); yield f, (lambda full=full: open(full, 'rb').read())

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
    def files(self, sector=None, size=None, prefix=''):
        if sector is None: sector, size = self.root, self.rsize
        for name, sec, sz, attr in self._dir(sector, size):
            if attr & 0x10: yield from self.files(sec, sz, prefix + name + '/')
            elif name.lower().rsplit('.', 1)[-1] in ('cmf', 'bdl', 'clf', 'zip'):
                def rd(sec=sec, sz=sz): self.f.seek(self.base + sec * 2048); return self.f.read(sz)
                yield name, rd

def _bundle_iter(src):
    for fname, rd in src.files():
        e = fname.lower().rsplit('.', 1)[-1]
        try:
            if e == 'zip':
                z = zipfile.ZipFile(io.BytesIO(rd()))
                for m in z.namelist():
                    if m.lower().rsplit('.', 1)[-1] in ('cmf', 'bdl', 'clf'):
                        try: yield os.path.splitext(fname)[0] + '_' + os.path.splitext(os.path.basename(m))[0], Bundle(z.read(m))
                        except Exception: pass
            else: yield os.path.splitext(fname)[0], Bundle(rd())
        except Exception: pass

def _safe(n): return re.sub(r'[^A-Za-z0-9_.\-+ ()]', '_', n)

def cmd_export(a):
    import argparse
    ap = argparse.ArgumentParser(prog='import_texture_set.py export', add_help=False)
    ap.add_argument('game'); ap.add_argument('out', nargs='?'); ap.add_argument('--match', default=''); ap.add_argument('--monster', default=''); ap.add_argument('--list', action='store_true'); ap.add_argument('-h', '--help', action='store_true')
    if not a or a[0] in ('-h', '--help'): print(EXPORT_HELP); return 0
    try: o = ap.parse_args(a)
    except SystemExit: print('Could not read the arguments.\n\n' + EXPORT_HELP); return 2
    words = [w.strip().lower() for w in o.match.split(',') if w.strip()]
    mon = o.monster.lower()
    if not words and not mon: print('Tell me what to export: --match WORDS (for example --match Gigan) or --monster NAME.\n\n' + EXPORT_HELP); return 2
    if not o.list and not o.out: print('Give an output folder as the second argument (or use --list).'); return 2
    try:
        from PIL import Image
    except ImportError:
        if not o.list: print('Pillow is missing. Install it with:  pip install Pillow  (Python 3.9 or newer)'); return 2
        Image = None
    if os.path.isdir(o.game): src = _Folder(o.game)
    elif os.path.isfile(o.game):
        try: src = _Xiso(o.game)
        except Exception as e: print('Cannot read %s as an Xbox disc image: %s' % (o.game, e)); return 2
    else: print('Not found: %s (give the game folder or the .iso)' % o.game); return 2
    allnames = {}; hits = []; nb = 0; monsters = set()
    try:
        for bn, b in _bundle_iter(src):
            nb += 1
            if '_' not in bn and any(c[0] == 9 and '_g2_' in c[1].lower() for c in b.chunks): monsters.add(bn)
            inmon = bool(mon) and bn.lower() in (mon, mon + '_' + mon, 'hud' + mon)
            pal = {c[1].lower(): c for c in b.chunks if c[0] == 0x0D}
            for typ, name, mo, ms, go, gs in b.chunks:
                if typ != 9 or ms < 20: continue
                allnames[name] = allnames.get(name, 0) + 1
                if not (inmon or any(w in name.lower() for w in words)): continue
                if o.list: hits.append((bn, name)); continue
                com, data, lock, fmt, size = struct.unpack_from('<5I', b.d, mo)
                pc = pal.get((name + '.pal').lower())
                hits.append((bn, name, fmt, size, b.d[go:go + gs], b.d[pc[4]:pc[4] + 1024] if pc else b''))
    except OSError as e: print('Read error: %s' % e); return 2
    if o.monster == '?' or (mon and mon not in {m.lower() for m in monsters}):
        print(('Unknown monster %r. ' % o.monster if o.monster != '?' else '') + 'Monsters found in the game: ' + ', '.join(sorted(monsters, key=str.lower))); return 0 if o.monster == '?' else 2
    if nb == 0: print('No game bundles (.cmf/.bdl/.clf/.zip) found in %s. Is this the Xbox Godzilla: Save the Earth game folder or disc image?' % o.game); return 2
    if not hits:
        print('No texture name contains %s.' % ', '.join(words))
        near = difflib.get_close_matches(words[0], list(allnames), n=8, cutoff=0.4)
        if near: print('Closest names: ' + ', '.join(near))
        return 1
    if o.list:
        cnt = {}
        for bn, n in hits: cnt.setdefault(n, []).append(bn)
        for n in sorted(cnt, key=str.lower): print('%s  (%d bundle%s)' % (n, len(cnt[n]), '' if len(cnt[n]) == 1 else 's'))
        print('%d texture names.' % len(cnt)); return 0
    os.makedirs(o.out, exist_ok=True)
    by = {}
    for h in hits: by.setdefault(h[1].lower(), []).append(h)
    rep = []; nw = 0
    for lname in sorted(by):
        group = by[lname]; items = []
        for bn, name, fmt, size, raw, pal in group:
            fmtname = FMT.get((fmt >> 8) & 0xFF, ('0x%02X' % ((fmt >> 8) & 0xFF),))[0]
            try: w, h, rgba = decode_texture(fmt, size, raw, pal)
            except Exception as e: rep.append((name, '', fmtname, '', 'skipped', '%s (%s)' % (e, bn))); continue
            items.append((bn, name, fmtname, w, h, rgba))
        distinct = {}
        for it in items: distinct.setdefault(hashlib.sha1(it[5]).hexdigest() + '%dx%d' % (it[3], it[4]), []).append(it)
        for k, (digest, lst) in enumerate(sorted(distinct.items())):
            bn, name, fmtname, w, h, rgba = lst[0]
            fn = _safe(name) + ('.png' if len(distinct) == 1 else '__%s.png' % _safe(bn))
            if os.path.exists(os.path.join(o.out, fn)) and len(distinct) > 1: fn = fn[:-4] + '_%d.png' % k
            Image.frombytes('RGBA', (w, h), rgba).save(os.path.join(o.out, fn)); nw += 1
            rep.append((name, '%dx%d' % (w, h), fmtname, fn, 'written', ('same pixels also in: ' + ', '.join(i[0] for i in lst[1:])) if len(lst) > 1 else ''))
    with open(os.path.join(o.out, 'export_report.csv'), 'w', newline='', encoding='utf-8') as f:
        wr = csv.writer(f); wr.writerow(['name', 'size', 'format', 'file', 'status', 'reason']); wr.writerows(rep)
    sk = [r for r in rep if r[4] == 'skipped']
    print('Exported %d PNG file(s) for %d texture name(s) to %s' % (nw, len(by), o.out))
    if sk: print('%d skipped (see export_report.csv), e.g. %s: %s' % (len(sk), sk[0][0], sk[0][5]))
    print('Report: %s' % os.path.join(o.out, 'export_report.csv'))
    print('Next: edit the PNGs (keep each file name; keep the size or use the same proportions), then run:')
    print('  python -I import_texture_set.py import %s <game folder or .iso> <pack folder> --id my-retexture --name "My retexture"' % o.out)
    return 0

def main():
    c = sys.argv[1] if len(sys.argv) > 1 else ''
    if c == 'export':
        try: sys.exit(cmd_export(sys.argv[2:]))
        except KeyboardInterrupt: sys.exit(130)
        except Exception as e: print('Export failed: %s: %s' % (type(e).__name__, e)); sys.exit(2)
    {'index': cmd_index, 'import': cmd_import, 'sample': cmd_sample}.get(c, lambda a: print(__doc__ + '\n' + EXPORT_HELP))(sys.argv[2:])

if __name__ == '__main__':
    main()
