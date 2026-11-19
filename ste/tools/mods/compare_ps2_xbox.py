# SPDX-License-Identifier: GPL-2.0-or-later
"""compare_ps2_xbox.py <GODZILLA.VOL> <xbox extracted dir> [--csv out.csv]: READ-ONLY comparison of the PS2 PVOL archive with the Xbox file tree (both are the user's own copies).
Own code (PVOL layout: 'PVOL', u32 version, u32 count, u32 name-block size, count x {offset, size, index}, then u32 count + name offsets + NUL-terminated names).
Per file: name (extension mapped PS2 -> Xbox: .cmp->.cmf, .bdp->.bdl, .clp->.clf, .mic->.wxv), PS2 size, Xbox size, byte-identical yes/no. Nothing is written except the optional CSV."""
import hashlib, os, struct, sys

EXTMAP = {'.cmp': '.cmf', '.bdp': '.bdl', '.clp': '.clf', '.mic': '.wxv'}

def pvol_toc(path):
    with open(path, 'rb') as f:
        h = f.read(16)
        if h[:4] != b'PVOL': raise ValueError('not a PVOL')
        _, count, nsz = struct.unpack_from('<III', h, 4)
        recs = f.read(count * 12); block = f.read(nsz + 4 + count * 4)
    out = []
    for i in range(count):
        off, size, idx = struct.unpack_from('<III', recs, i * 12)
        noff = struct.unpack_from('<I', block, 4 + idx * 4)[0]
        out.append((block[noff:block.index(b'\0', noff)].decode('latin-1'), off, size))
    return out

def sha_slice(f, off, size):
    f.seek(off); h = hashlib.sha256(); left = size
    while left > 0:
        c = f.read(min(left, 1 << 22))
        if not c: break
        h.update(c); left -= len(c)
    return h.hexdigest()

def xbox_index(root):
    idx = {}
    for d, _, fs in os.walk(root):
        for n in fs:
            idx.setdefault(n.lower(), []).append(os.path.join(d, n))
    return idx

def main(a):
    vol, xroot = a[0], a[1]
    toc = pvol_toc(vol); xi = xbox_index(xroot)
    rows = []; stats = {}
    with open(vol, 'rb') as f:
        for name, off, size in toc:
            base, ext = os.path.splitext(name); e = ext.lower()
            if e in ('.pss',): continue   # movies: not compared
            xn = (base + EXTMAP.get(e, ext)).lower()
            cand = xi.get(xn)
            if not cand: rows.append((name, size, '', '', 'no-xbox-file')); stats[('missing', e)] = stats.get(('missing', e), 0) + 1; continue
            xp = cand[0]; xs = os.path.getsize(xp)
            if xs == size:
                with open(xp, 'rb') as xf: same = hashlib.sha256(xf.read()).hexdigest() == sha_slice(f, off, size)
                st = 'identical' if same else 'same-size-different'
            else: st = 'different-size'
            rows.append((name, size, xs, os.path.relpath(xp, xroot), st)); stats[(st, e)] = stats.get((st, e), 0) + 1
    for k in sorted(stats): print(k, stats[k])
    if '--csv' in a:
        with open(a[a.index('--csv') + 1], 'w', encoding='utf-8') as o:
            o.write('ps2_name,ps2_size,xbox_size,xbox_path,status\n')
            for r in rows: o.write(','.join('"%s"' % str(x).replace('"', "'") for x in r) + '\n')

if __name__ == '__main__':
    main(sys.argv[1:])
