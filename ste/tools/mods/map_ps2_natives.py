# SPDX-License-Identifier: GPL-2.0-or-later
"""map_ps2_natives.py <default.xbe> <script-natives.tsv> <out.tsv>: join the script-native table of the PS2 game (from ANILORAK's published program map, used locally only: rows are read, nothing is copied into a repo)
with the native descriptors found in the Xbox default.xbe. An Xbox descriptor is {u32 native id, u32 0, u32 type-info 0x356630, u32 parameter-type list, NUL-terminated name}; the PS2 table says class#id and
class.method plus the PS2 address of the body. Rows are matched on (method name, id): the ids are the same numbers on both platforms (e.g. startRage is native 115 = 0x73 in both).
Output columns: xbox_descriptor_va, xbox_id, name, ps2_class_method, ps2_body_address, confidence, method. The Xbox HANDLER address is not in this table (not located yet)."""
import csv, struct, sys

def main(a):
    d = open(a[0], 'rb').read()
    hdr, = struct.unpack_from('<I', d, 0x104); nsec, = struct.unpack_from('<I', d, 0x11C); sh, = struct.unpack_from('<I', d, 0x120); sh -= hdr
    secs = []
    for i in range(nsec):
        flags, va, vs, ra, rs = struct.unpack_from('<IIIII', d, sh + i * 56); secs.append((va, vs, ra, rs))
    desc = []
    for va, vs, ra, rs in secs:
        for o in range(ra, ra + rs - 20, 4):
            if struct.unpack_from('<I', d, o + 8)[0] != 0x356630 or struct.unpack_from('<I', d, o + 4)[0] != 0: continue
            nid, = struct.unpack_from('<I', d, o)
            if nid > 1000: continue
            e = d.find(b'\0', o + 16, o + 80)
            if e < 0: continue
            nm = d[o + 16:e]
            if not nm or not all(32 <= c < 127 for c in nm): continue
            desc.append((va + o - ra, nid, nm.decode()))
    ps2 = {}
    for r in csv.DictReader(open(a[1], encoding='utf-8'), delimiter='\t'):
        cls, nid = r['slot'].split('#') if '#' in r['slot'] else (r['class'], '-1')
        method = r['name'].split('.', 1)[-1]
        ps2.setdefault(method, []).append((r['name'], r['body'], int(nid), r['tag']))
    rows = []; matched = 0
    for va, nid, nm in desc:
        c = ps2.get(nm, [])
        same = [x for x in c if x[2] == nid]
        if same: rows.append(('%08X' % va, nid, nm, same[0][0], same[0][1], 'high (name and id equal)' if len(same) == 1 else 'medium (name and id equal, %d PS2 candidates)' % len(same), 'script-native name+id')); matched += 1
        elif len(c) == 1: rows.append(('%08X' % va, nid, nm, c[0][0], c[0][1], 'medium (name equal, id differs: PS2 %d)' % c[0][2], 'script-native name')); matched += 1
        else: rows.append(('%08X' % va, nid, nm, '', '', 'none', 'no PS2 row with this name' if not c else 'ambiguous name'))
    with open(a[2], 'w', newline='', encoding='utf-8') as o:
        w = csv.writer(o, delimiter='\t'); w.writerow(['xbox_descriptor_va', 'xbox_native_id', 'name', 'ps2_class_method', 'ps2_body_address', 'confidence', 'method']); w.writerows(rows)
    print(len(desc), 'Xbox descriptors,', matched, 'matched to PS2 rows ->', a[2])

if __name__ == '__main__':
    main(sys.argv[1:])
