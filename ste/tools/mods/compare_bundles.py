# SPDX-License-Identifier: GPL-2.0-or-later
"""compare_bundles.py <ps2 dir> <xbox dir> <pwbundle.py dir> [--csv out.csv]: READ-ONLY chunk-level comparison of Pipeworks bundles of the PS2 and the Xbox game (the user's own copies).
Uses the third-party reader pwbundle.py (ANILORAK's pack, used locally only, not copied into any repo) to list chunks; pairs bundles by name (.cmp/.cmf, .bdp/.bdl, .clp/.clf) and chunks by (type, name);
per chunk type counts: chunks in both, byte-identical CPU-side data (main block), identical size, only-PS2, only-Xbox."""
import hashlib, os, sys, collections

def load(pwdir):
    sys.path.insert(0, pwdir)
    import pwbundle
    return pwbundle

def main(a):
    ps2, xbox, pwdir = a[0], a[1], a[2]
    pw = load(pwdir)
    pairs = []
    for fn in sorted(os.listdir(ps2)):
        b, e = os.path.splitext(fn)
        xe = {'.cmp': '.cmf', '.bdp': '.bdl', '.clp': '.clf'}.get(e.lower())
        if not xe: continue
        for d, _, fs in os.walk(xbox):
            if b + xe in fs: pairs.append((os.path.join(ps2, fn), os.path.join(d, b + xe))); break
    st = collections.defaultdict(lambda: collections.Counter())
    for p, x in pairs:
        try: bp = pw.Bundle(p); bx = pw.Bundle(x)
        except Exception as ex: print('FAIL', os.path.basename(p), ex); continue
        mp = {(c.type, c.name): c for c in bp.chunks}; mx = {(c.type, c.name): c for c in bx.chunks}
        for k in set(mp) | set(mx):
            t = pw.TYPE_NAMES.get(k[0], hex(k[0]))
            if k in mp and k in mx:
                st[t]['both'] += 1
                a1 = mp[k].main(bp.data); a2 = mx[k].main(bx.data)
                if a1 == a2: st[t]['main identical'] += 1
                elif len(a1) == len(a2): st[t]['main same size, different'] += 1
                else: st[t]['main size differs'] += 1
                g1 = mp[k].gs(bp.data); g2 = mx[k].gs(bx.data)
                if g1 or g2: st[t]['gs/data identical' if g1 == g2 else 'gs/data differs'] += 1
            elif k in mp: st[t]['only PS2'] += 1
            else: st[t]['only Xbox'] += 1
    print(len(pairs), 'bundle pairs')
    for t in sorted(st): print('%-14s' % t, dict(st[t]))

if __name__ == '__main__':
    main(sys.argv[1:])
