# SPDX-License-Identifier: GPL-2.0-or-later
"""bio_ui: build ShellData\\ControlRoom.bdl with a Biollante character-select slot from the player's own ControlRoom.bdl.

build(orig_bytes) -> new bundle bytes. Own code (pw_bundle + texlib), stdlib only, no third-party tool.
Adds, to the bundle: four 52x52 grid tiles generated from the disc's own biollante_en art (styled after the Anguirus tiles),
the select-grid flash IFL, and a MonsterSelect.ifc with a new "biollante" child after the last one of the top row.
The new names are Zz_*_Biollante so they sort after every existing name (inserting names in the middle of the table crashes
the title, docs/formats/pipeworks_bundle.md). No game bytes are stored in this file.
"""
import os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pw_bundle, texlib as t

KINDS = ('dis', 'en', 'foc', 'sel')          # sorted order of the Zz_ names, flash goes between en and foc
X0, Y0, S = 28, 34, 92                       # head crop of biollante_en (source pixels)


def _tiles(bio_raw, ref_raw, crop=(X0, Y0, S)):
    X0_, Y0_, S_ = crop
    bio = t.dec('a4r4g4b4', bio_raw, 128, 192, 256)
    ref = {k: t.dec('a4r4g4b4' if k in ('en', 'dis') else 'a1r5g5b5', ref_raw[k], 52, 52, 128) for k in KINDS}

    def samp(x, y):
        fx, fy = X0_ + x * S_ / 52.0, Y0_ + y * S_ / 52.0
        x0, y0 = int(fx), int(fy); ax, ay = fx - x0, fy - y0
        acc = [0, 0, 0]
        for dx, dy, w in ((0, 0, (1 - ax) * (1 - ay)), (1, 0, ax * (1 - ay)), (0, 1, (1 - ax) * ay), (1, 1, ax * ay)):
            p = bio[min(191, y0 + dy)][min(127, x0 + dx)]
            for i in range(3): acc[i] += p[i] * w
        return tuple(int(v) for v in acc)
    face = [[samp(x, y) for x in range(52)] for y in range(52)]
    lum = lambda p: (p[0] * 30 + p[1] * 59 + p[2] * 11) // 100
    ring = lambda x, y: min(x, y, 51 - x, 51 - y) < 5
    out = {}
    for kind in KINDS:
        px = []
        for y in range(52):
            row = []
            for x in range(52):
                f = face[y][x]; r = ref[kind][y][x]; l = lum(f)
                if kind in ('en', 'dis'):
                    if ring(x, y): row.append(r)
                    elif kind == 'en': row.append((f[0] * 9 // 10, f[1] * 9 // 10, f[2] * 9 // 10, 255))
                    else: row.append((l // 8, l // 2, min(255, l * 2 + 20), r[3] if r[3] else 200))
                elif kind == 'foc':
                    row.append((min(255, f[0] * 12 // 10), min(255, f[1] * 12 // 10), min(255, f[2] * 12 // 10), 255))
                else:
                    row.append((255 - (255 - l) * 3 // 10 - 20 * (255 - l) // 255, 255 - (255 - l) * 3 // 10, 255 - (255 - l) * 2 // 10, 255))
            px.append(row)
        out[kind] = t.enc('a4r4g4b4' if kind in ('en', 'dis') else 'a1r5g5b5', px, 128)
    return out


def _end_of_tag(s, start):
    n = 0
    for m in re.finditer(r'<(/?)o\b[^>]*>', s[start:]):
        n += -1 if m.group(1) else 1
        if n == 0: return start + m.end()
    raise ValueError('unbalanced ifc')


# Slot table: one row per extra select-grid slot. name = the monster's script name, row = grid row (0..2; Row2 is created by this mod),
# art = the bundle's own full-size portrait entry the four 52x52 tiles are generated from, crop = (x, y, size) of the head in that art.
# Adding a monster later = one more row here (plus its roster/HUD data); the select script finds slots by name.
SLOTS = [
    {'name': 'biollante', 'row': 2, 'art': 'biollante_en', 'crop': (28, 34, 92)},
]
# 3-row layout: the retail grid (2 rows, 52 px tiles, 57 px pitch) is scaled so a third row fits above the Back/Select buttons inside the 4:3 menu area
GRID_SCALE = 0.84
GRID_POS = (31, 21)
ROW_Y = {0: 308, 1: 365, 2: 422}     # row origins in the grid's own (unscaled) space


def build(orig):
    b = pw_bundle.Bundle.parse(orig)
    need = lambda n: b.find(n) or (_ for _ in ()).throw(SystemExit('ControlRoom.bdl lacks %s: not the retail STE file?' % n))
    ref_e = {k: need('MonsterSelect_%s_Anguirus' % k) for k in KINDS}
    ref_raw = {k: b.get_res(ref_e[k]) for k in KINDS}
    flash_src = b.get_main(need('MonsterSelect_Anguirus_Flash.ifl'))
    ifc_e = need('MonsterSelect.ifc')
    ifc = b.get_main(ifc_e).decode('latin-1')
    a = ifc.index('<o n="moguera2">'); e = _end_of_tag(ifc, a); tmpl = ifc[a:e]
    new_entries = []        # (name, kind, main, res, res_align, clone_src)
    rows = {}
    for sl in SLOTS:
        nm = sl['name']; cap = nm.capitalize(); up = nm.upper()
        tiles = _tiles(b.get_res(need(sl['art'])), ref_raw, sl['crop'])
        tn = lambda k, cap=cap: 'Zz_%s_%s' % (k, cap)
        fl_name = 'Zz_Flash_%s.ifl' % cap
        fl = flash_src.replace(b'Anguirus', cap.encode()).replace(b'MonsterSelect_foc_', b'Zz_foc_').replace(b'MonsterSelect_sel_', b'Zz_sel_')
        for k in KINDS: new_entries.append((tn(k), 'tile', ref_e[k], tiles[k]))
        new_entries.append((fl_name, 'text', None, fl))
        c = tmpl.replace('<o n="moguera2">', '<o n="%s">' % nm, 1)
        c = (c.replace('MONSTERSELECT_DIS_MOGUERA', tn('dis').upper()).replace('MONSTERSELECT_EN_MOGUERA', tn('en').upper())
              .replace('MONSTERSELECT_MOGUERA_FLASH.IFL', fl_name.upper()).replace('MONSTERSELECT_MOGUERA_FLASH', fl_name.upper()))
        if up not in c or 'MOGUERA' in c.replace('QUESTION', ''): raise SystemExit('MonsterSelect.ifc layout differs from the retail file')
        rows.setdefault(sl['row'], []).append(c)
    # build the extra rows (centred on the retail 9-column grid; x of a column = 76 + 55 * i)
    xml = ''
    for r in sorted(rows):
        kids = rows[r]
        if r < 2 or len(kids) > 10: raise SystemExit('slot table: row %d is not an extra row or has too many slots' % r)
        x0 = 76 + (9 - len(kids)) * 55 // 2
        body = ''.join(k.replace('<p> 516 0 0</p>', '<p> %d 0 0</p>' % (x0 + 55 * i), 1) for i, k in enumerate(kids))
        xml += '<o n="Row%d"><p> 0 %d 0</p>%s</o>' % (r, ROW_Y[r], body)
    r1 = ifc.index('<o n="Row1">'); r1e = _end_of_tag(ifc, r1)
    ifc2 = ifc[:r1e] + xml + ifc[r1e:]
    old = '<o n="Thumbnails"><p> 3 -7 0</p>'
    if ifc2.count(old) != 1: raise SystemExit('MonsterSelect.ifc layout differs from the retail file')
    ifc2 = ifc2.replace(old, '<o n="Thumbnails"><p> %d %d 0</p><scale>%s</scale>' % (GRID_POS[0], GRID_POS[1], GRID_SCALE))
    b.set_main(ifc_e, ifc2.encode('latin-1'))
    # new names must sort after every existing name and be added in sorted order
    for nm, kind, src, data in sorted(new_entries, key=lambda t: t[0].lower()):
        if kind == 'tile': b.clone_entry(src, nm, res=data, res_align=128)
        else: b.add_entry(nm, 2, main=data)
    return b.emit()


if __name__ == '__main__':
    if len(sys.argv) != 3:
        print('usage: bio_ui.py <retail ControlRoom.bdl> <out ControlRoom.bdl>'); sys.exit(2)
    open(sys.argv[2], 'wb').write(build(open(sys.argv[1], 'rb').read()))
    print('wrote', sys.argv[2])
