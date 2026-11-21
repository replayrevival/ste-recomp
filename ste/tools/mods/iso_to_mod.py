# SPDX-License-Identifier: GPL-2.0-or-later
"""iso_to_mod.py - turn a modified STE disc image or folder into a patch mod (manifest api 2), without shipping game data.

  python -I iso_to_mod.py diff <modded .iso or folder> <retail folder or .iso>
  python -I iso_to_mod.py make <modded> <retail> <out mod folder> --id my.mod --name "My mod" [--license CC-BY-4.0]
  python -I iso_to_mod.py prepare <mod folder> <retail folder or .iso>      (builds <mod>\\_built from YOUR original files)

make writes ONLY differences (step1 patches, docs\\formats\\mod_patch.md) for files that exist in both. Files that are new in the modded
image are reported, never copied: a new file needs a close retail file as its base (use --new-base NEW=RETAIL, repeatable) and then
becomes "retail file + patch". A .zip that holds exactly one member whose content is the (patched) sibling .cmf is not diffed (compressed
bytes do not diff): it is recorded as "zip-of" and rebuilt deterministically (zlib level 6, fixed date) from the patched member.
Needs Python 3.9+, standard library only. Private input data stays where it is; nothing is written next to it."""
import sys, os, io, json, hashlib, struct, zipfile, zlib
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'step-diff'))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import step_diff

class Src:
    """read-only file source: a folder or an Xbox disc image (XDVDFS, plain or with a video partition in front)"""
    OFFS = (0, 0x18300000, 0x0FD90000, 0x02090000)
    def __init__(self, p):
        self.p = p; self.files = {}
        if os.path.isdir(p):
            self.f = None
            for d, _, fs in os.walk(p):
                for n in fs:
                    full = os.path.join(d, n); rel = os.path.relpath(full, p).replace('\\', '/')
                    self.files[rel.lower()] = (rel, full, os.path.getsize(full))
        else:
            self.f = open(p, 'rb'); self.base = None
            for off in self.OFFS:
                self.f.seek(off + 32 * 2048); b = self.f.read(2048)
                if b[:20] == b'MICROSOFT*XBOX*MEDIA': self.base = off; root, rsize = struct.unpack_from('<II', b, 20); break
            if self.base is None: raise SystemExit('%s: not a folder or an Xbox disc image' % p)
            for rel, sec, sz in self._walk(root, rsize): self.files[rel.lower()] = (rel, sec, sz)
    def _walk(self, sec, size, prefix=''):
        self.f.seek(self.base + sec * 2048); data = self.f.read(size); seen = set(); st = [0]
        while st:
            o = st.pop()
            if o in seen or o + 14 > len(data): continue
            seen.add(o); l, r, s, sz, at, nl = struct.unpack_from('<HHIIBB', data, o)
            if l == 0xFFFF and r == 0xFFFF and s == 0xFFFFFFFF: continue
            name = data[o + 14:o + 14 + nl].decode('latin-1')
            if at & 0x10: yield from self._walk(s, sz, prefix + name + '/')
            else: yield prefix + name, s, sz
            for t in (l, r):
                if t and t != 0xFFFF: st.append(t * 4)
    def read(self, rel):
        r, loc, sz = self.files[rel.lower()]
        if self.f is None: return open(loc, 'rb').read()
        self.f.seek(self.base + loc * 2048); return self.f.read(sz)
    def has(self, rel): return rel.lower() in self.files
    def name(self, rel): return self.files[rel.lower()][0]

SKIP = ('udata/', 'tdata/', 'cache/')   # save-game and cache files of an extracted copy are not disc content
def disc_files(s): return sorted(k for k in s.files if not k.startswith(SKIP))
def _jload(path):
    """JSON file reader that tolerates a UTF-8 BOM (PowerShell's Set-Content/Out-File -Encoding utf8 writes one)."""
    with open(path, encoding='utf-8-sig') as f: return json.load(f)


def sha(b): return hashlib.sha256(b).hexdigest()

def zip_single(blob):
    """(member name, member bytes) when blob is a zip with exactly one member, else None"""
    try:
        z = zipfile.ZipFile(io.BytesIO(blob)); il = z.infolist()
        if len(il) != 1: return None
        return il[0].filename, z.read(il[0].filename)
    except Exception: return None

def build_zip(member, data):
    bio = io.BytesIO()
    with zipfile.ZipFile(bio, 'w') as z:
        zi = zipfile.ZipInfo(member, date_time=(1980, 1, 1, 0, 0, 0)); zi.compress_type = zipfile.ZIP_DEFLATED; zi.external_attr = 0
        z.writestr(zi, data, compress_type=zipfile.ZIP_DEFLATED, compresslevel=6)
    return bio.getvalue()

def classify(mod, ret):
    new, changed, same, removed = [], [], 0, []
    mk = disc_files(mod); rk = set(disc_files(ret))
    for k in mk:
        if k not in rk: new.append(k)
        elif sha(mod.read(k)) != sha(ret.read(k)): changed.append(k)
        else: same += 1
    removed = [k for k in sorted(rk) if not mod.has(k)]
    return new, changed, same, removed

def cmd_diff(a):
    mod, ret = Src(a[0]), Src(a[1]); new, changed, same, removed = classify(mod, ret)
    print('identical: %d files' % same)
    print('new in modded image (%d):' % len(new)); [print('  %s  %d bytes' % (mod.name(k), mod.files[k][2])) for k in new]
    print('changed (%d):' % len(changed)); [print('  %s  %d -> %d bytes' % (mod.name(k), ret.files[k][2], mod.files[k][2])) for k in changed]
    print('missing from modded image (%d):' % len(removed)); [print('  %s' % ret.name(k)) for k in removed]

def opt(a, key, default=None):
    return a[a.index(key) + 1] if key in a else default

def cmd_make(a):
    mod, ret, out = Src(a[0]), Src(a[1]), a[2]
    mid = opt(a, '--id', 'unnamed.mod'); nm = opt(a, '--name', mid); lic = opt(a, '--license', 'CC-BY-4.0')
    bases = {}
    for i, x in enumerate(a):
        if x == '--new-base': n, r = a[i + 1].split('=', 1); bases[n.lower().replace('\\', '/')] = r.replace('\\', '/')
    if os.path.exists(out) and os.listdir(out): raise SystemExit('%s exists and is not empty' % out)
    os.makedirs(os.path.join(out, 'patches'), exist_ok=True)
    new, changed, same, removed = classify(mod, ret)
    entries, report, todo = [], [], []
    mem_patched = {}   # target (lower) -> result bytes, for zip-of detection
    for k in changed + [n for n in new if n in bases]:
        base_k = k if k in ret.files else bases[k].lower()
        a_b, b_b = ret.read(base_k), mod.read(k)
        zs_a, zs_b = zip_single(a_b), zip_single(b_b)
        sib = k[:-4] + '.cmf' if k.endswith('.zip') else None
        if zs_b and zs_a and sib and sib in changed and mod.read(sib) == zs_b[1]:
            todo.append((k, zs_b[0], sib)); continue
        pf = 'patches/' + mod.name(k).replace('/', '__') + '.step'
        pth = os.path.join(out, pf)
        ops = step_diff.make_ops(a_b, b_b)
        body = zlib.compress(b''.join(bytes([kd]) + p for kd, p in ops), 9)
        head = step_diff.MAGIC + struct.pack('<QQ', len(a_b), len(b_b)) + step_diff.sha(a_b) + step_diff.sha(b_b) + struct.pack('<II', len(ops), 1)
        blob = head + body; blob += struct.pack('<I', zlib.crc32(blob) & 0xFFFFFFFF); open(pth, 'wb').write(blob)
        tgt = mod.name(k).replace('/', '\\')
        entries.append({'target': tgt, 'base_sha256': 'sha256:' + sha(a_b), 'result_sha256': 'sha256:' + sha(b_b), 'patch': pf.replace('/', '\\'), 'size': len(b_b), 'base_from': ret.name(base_k)})
        report.append('%s: step1 patch %d bytes (result %d)' % (tgt, len(blob), len(b_b)))
    for k, member, sib in todo:   # zip-of entries: result = deterministic rebuild from the PATCHED sibling
        res = build_zip(member, mod.read(sib)); tgt = mod.name(k).replace('/', '\\')
        entries.append({'target': tgt, 'base_sha256': 'sha256:' + sha(ret.read(k)), 'result_sha256': 'sha256:' + sha(res), 'patch': 'zip-of:' + mod.name(sib).replace('/', '\\'),
                        'format': 'zip-of', 'member': member, 'size': len(res)})
        report.append('%s: zip-of %s (rebuilt, %d bytes, no patch data)' % (tgt, mod.name(sib), len(res)))
    unhandled_new = [n for n in new if n not in bases]
    man = {'api': 2, 'id': mid, 'name': nm, 'version': '1.0.0', 'game': 'STE-Xbox', 'profile': 'content', 'license': lic,
           'description': 'Patch mod generated by iso_to_mod.py. Contains only differences against the original files, no game data.',
           'patch_tool': 'iso_to_mod 1.0 (step-diff 1.0)', 'patches': entries}
    json.dump(man, open(os.path.join(out, 'manifest.json'), 'w'), indent=2)
    print('\n'.join(report)); print('mod folder %s: %d entries (%d patch files, %d zip-of), patch data %d bytes' % (
        out, len(entries), len(entries) - len(todo), len(todo), sum(os.path.getsize(os.path.join(out, 'patches', f)) for f in os.listdir(os.path.join(out, 'patches')))))
    if unhandled_new:
        print('NOT EXPRESSIBLE (new files without a --new-base): %s' % ', '.join(mod.name(n) for n in unhandled_new))
    if removed and '--partial' not in a: print('note: %d retail files are missing from the modded image (a mod cannot remove files): %s' % (len(removed), ', '.join(ret.name(n) for n in removed[:10])))

def write_recipe_mod(out, mid, name, lic, desc, recipes):
    """recipes: [(target, base bytes, result bytes, [edit dicts])] -> manifest api 2 with format 'bsf-edit' entries.
    The mod holds only the edit lists and the hashes (no game bytes), so it can be shared; prepare builds the files from the player's own."""
    if os.path.exists(out) and os.listdir(out): raise SystemExit('%s exists and is not empty' % out)
    os.makedirs(os.path.join(out, 'edits'), exist_ok=True); entries = []
    for rec in recipes:
        tgt, base, res, edits = rec[:4]; fmt = rec[4] if len(rec) > 4 else 'bsf-edit'
        if fmt == 'builtin':   # the build is a function shipped with the tools (tools/mods/<module>.py: build(bytes) -> bytes); edits = module name
            pf = 'builtin:' + edits
        else:
            pf = 'edits/' + tgt.replace('/', '__') + '.json'
            json.dump({'format': fmt, 'edits': edits}, open(os.path.join(out, pf), 'w'), indent=1)
        entries.append({'target': tgt.replace('/', '\\'), 'format': fmt, 'patch': pf.replace('/', '\\'), 'base_sha256': 'sha256:' + sha(base),
                        'result_sha256': 'sha256:' + sha(res), 'size': len(res)})
    json.dump({'api': 2, 'id': mid, 'name': name, 'version': '1.0.0', 'game': 'STE-Xbox', 'profile': 'content', 'license': lic, 'description': desc,
               'patch_tool': 'bsf_edit 1.0', 'patches': entries}, open(os.path.join(out, 'manifest.json'), 'w'), indent=2)


BUILTINS = ('bio_ui',)   # builders shipped with these tools; a manifest can only name one of these (never arbitrary modules)

def cmd_prepare(a):
    mdir, ret = a[0], Src(a[1]); man = _jload(os.path.join(mdir, 'manifest.json')); ok = True; patched = {}
    for e in man['patches']:
        tgt = e['target'].replace('\\', '/'); dst = os.path.join(mdir, '_built', e['target'].replace('/', '\\'))
        if e.get('format') == 'zip-of': continue
        if not ret.has(tgt): print('%s: not in your game, skipped' % tgt); ok = False; continue
        orig = ret.read(tgt)
        if 'sha256:' + sha(orig) != e['base_sha256']: print('%s: BASE MISMATCH (your file differs from the one the mod was made for)' % tgt); ok = False; continue
        if e.get('format') == 'bsf-edit':
            import bsf_edit
            try: res = bsf_edit.apply_edits(orig, _jload(os.path.join(mdir, e['patch'].replace('\\', '/')))['edits'])
            except bsf_edit.BsfError as x: print('%s: %s' % (tgt, x)); ok = False; continue
        elif e.get('format') == 'text-edit':
            res = orig
            for ed in _jload(os.path.join(mdir, e['patch'].replace('\\', '/')))['edits']:
                o, n = ed['old'].encode('latin-1'), ed['new'].encode('latin-1')
                if res.count(o) != 1: print('%s: text edit does not match exactly once' % tgt); ok = False; res = None; break
                res = res.replace(o, n)
            if res is None: continue
        elif e.get('format') == 'builtin':
            import importlib
            nm = e['patch'][len('builtin:'):]
            if nm not in BUILTINS: print('%s: unknown builtin %r (this tools version knows %s)' % (tgt, nm, sorted(BUILTINS))); ok = False; continue
            mod = importlib.import_module(nm)
            try: res = mod.build(orig)
            except SystemExit as x: print('%s: %s' % (tgt, x)); ok = False; continue
        else: res = step_diff.apply(orig, open(os.path.join(mdir, e['patch'].replace('\\', '/')), 'rb').read())
        if 'sha256:' + sha(res) != e['result_sha256']: print('%s: RESULT MISMATCH' % tgt); ok = False; continue
        os.makedirs(os.path.dirname(dst), exist_ok=True); open(dst, 'wb').write(res); patched[tgt.lower()] = res; print('%s: built' % tgt)
    for e in man['patches']:
        if e.get('format') != 'zip-of': continue
        tgt = e['target'].replace('\\', '/'); sib = e['patch'][len('zip-of:'):].replace('\\', '/')
        if 'sha256:' + sha(ret.read(tgt)) != e['base_sha256']: print('%s: BASE MISMATCH' % tgt); ok = False; continue
        res = build_zip(e['member'], patched[sib.lower()]) if sib.lower() in patched else None
        if res is None or 'sha256:' + sha(res) != e['result_sha256']: print('%s: RESULT MISMATCH (zlib differs on this machine?)' % tgt); ok = False; continue
        dst = os.path.join(mdir, '_built', e['target'].replace('/', '\\')); os.makedirs(os.path.dirname(dst), exist_ok=True); open(dst, 'wb').write(res); print('%s: built (zip-of)' % tgt)
    return 0 if ok else 2

def _mod_order(root, only=None):
    """enabled mods of a mods root in the game's load order (kernel_mods.c): order.json 'order' first, then manifest priority (default 100), then id."""
    mods = {}
    for d in sorted(os.listdir(root)):
        mp = os.path.join(root, d, 'manifest.json')
        if not os.path.isfile(mp): continue
        try: m = _jload(mp)
        except ValueError: continue
        if not m.get('patches') or m.get('api', 1) < 2 or not m.get('id'): continue
        mods[m['id']] = {'dir': os.path.join(root, d), 'man': m, 'enabled': bool(m.get('enabled', True)), 'pos': 1 << 20, 'prio': m.get('priority', 100)}
    oj = os.path.join(root, 'order.json')
    if os.path.isfile(oj):
        o = _jload(oj)
        for i, mid in enumerate(o.get('order', [])):
            if mid in mods: mods[mid]['pos'] = i
        for mid, en in (o.get('enabled') or {}).items():
            if mid in mods: mods[mid]['enabled'] = bool(en)
    if only is not None:
        for mid in mods: mods[mid]['enabled'] = mid in only
    return [mods[k] for k in sorted((k for k in mods if mods[k]['enabled']), key=lambda k: (mods[k]['pos'], mods[k]['prio'], k))]


class StackConflict(Exception):
    pass


def _apply_layer(e, mdir, base, mid, touched):
    """apply one recipe entry on top of `base` (the retail file or the result of earlier mods). Returns the new bytes or raises StackConflict."""
    fmt = e.get('format')
    prior = touched.get(e['target'].replace(chr(92), '/').lower(), [])
    on_top = ' (already changed by %s)' % ', '.join(prior) if prior else ''
    if fmt in ('bsf-edit', 'text-edit'):
        edits = _jload(os.path.join(mdir, e['patch'].replace('\\', '/')))['edits']
        if fmt == 'bsf-edit':
            import bsf_edit
            toks = bsf_edit.parse(base)
            for i, ed in enumerate(edits):
                try: toks = bsf_edit.replace(toks, ed['old'], ed['new'])
                except bsf_edit.BsfError as x:
                    raise StackConflict('%s: %s: edit #%d does not match exactly once%s: %s | starts with: %s' % (mid, e['target'], i + 1, on_top, x, ed['old'].strip()[:70].replace('\n', ' ')))
            return bsf_edit.emit(toks)
        res = base
        for i, ed in enumerate(edits):
            o, n = ed['old'].encode('latin-1'), ed['new'].encode('latin-1')
            if res.count(o) != 1: raise StackConflict('%s: %s: text edit #%d does not match exactly once%s: %s' % (mid, e['target'], i + 1, on_top, ed['old'][:70]))
            res = res.replace(o, n)
        return res
    if fmt == 'builtin':
        import importlib
        nm = e['patch'][len('builtin:'):]
        if nm not in BUILTINS: raise StackConflict('%s: %s: unknown builtin %r' % (mid, e['target'], nm))
        try: return importlib.import_module(nm).build(base)
        except SystemExit as x: raise StackConflict('%s: %s: builtin %s refused the file%s: %s' % (mid, e['target'], nm, on_top, x))
    if prior: raise StackConflict('%s: %s: a binary patch cannot be stacked on top of the changes of %s' % (mid, e['target'], ', '.join(prior)))
    if 'sha256:' + sha(base) != e['base_sha256']: raise StackConflict('%s: %s: BASE MISMATCH (your file differs from the one the mod was made for)' % (mid, e['target']))
    return step_diff.apply(base, open(os.path.join(mdir, e['patch'].replace('\\', '/')), 'rb').read())


def cmd_prepare_stack(a):
    """prepare-stack <mods root> <retail folder or .iso> [--enable id,id]: build every enabled recipe mod ON TOP OF the earlier ones (load order).
    A mod whose edits no longer match the already edited file is reported as a conflict naming it and the mods it clashes with, and is skipped (its
    stale _built file for that target is renamed to .conflict so it cannot override the others). A layer on top of another mod's result gets a hash
    the manifest cannot know: it is recorded in <mod>\\_built\\_stack.json, which the game accepts (kernel_mods.c) in place of result_sha256."""
    root, ret = a[0], Src(a[1])
    only = set(a[a.index('--enable') + 1].split(',')) if '--enable' in a else None
    cur, touched, ok, sidecar = {}, {}, True, {}
    for M in _mod_order(root, only):
        mid, mdir = M['man']['id'], M['dir']; sidecar[mid] = {}
        for e in M['man']['patches']:
            if e.get('format') == 'zip-of': print('%s: %s: zip-of entries are not stacked here, use prepare for this mod alone' % (mid, e['target'])); ok = False; continue
            tgt = e['target'].replace('\\', '/'); key = tgt.lower(); dst = os.path.join(mdir, '_built', e['target'].replace('/', '\\'))
            if not ret.has(tgt): print('%s: %s: not in your game, skipped' % (mid, tgt)); ok = False; continue
            base = cur.get(key)
            if base is None: base = ret.read(tgt)
            try: res = _apply_layer(e, mdir, base, mid, touched)
            except StackConflict as x:
                print('CONFLICT %s' % x); ok = False
                if os.path.exists(dst): os.replace(dst, dst + '.conflict')
                continue
            alone = 'sha256:' + sha(res) == e['result_sha256']
            if not alone and not touched.get(key): print('%s: %s: RESULT MISMATCH' % (mid, tgt)); ok = False; continue
            os.makedirs(os.path.dirname(dst), exist_ok=True); open(dst, 'wb').write(res)
            if not alone: sidecar[mid][e['target']] = {'sha256': sha(res), 'stack': touched.get(key, []) + [mid]}
            cur[key] = res; touched.setdefault(key, []).append(mid)
            print('%s: %s: built%s' % (mid, tgt, (' on top of ' + ', '.join(touched[key][:-1])) if len(touched[key]) > 1 else ''))
        sc = os.path.join(mdir, '_built', '_stack.json')
        if sidecar[mid]:
            os.makedirs(os.path.dirname(sc), exist_ok=True); json.dump({'api': 1, 'targets': sidecar[mid]}, open(sc, 'w'), indent=1)
        elif os.path.exists(sc): os.replace(sc, sc + '.old')
    return 0 if ok else 2


if __name__ == '__main__':
    c = sys.argv[1] if len(sys.argv) > 1 else ''
    if c == 'prepare-stack' and len(sys.argv) >= 4: sys.exit(cmd_prepare_stack(sys.argv[2:]))
    if c == 'diff' and len(sys.argv) >= 4: cmd_diff(sys.argv[2:])
    elif c == 'make' and len(sys.argv) >= 5: cmd_make(sys.argv[2:])
    elif c == 'prepare' and len(sys.argv) >= 4: sys.exit(cmd_prepare(sys.argv[2:]))
    else: print(__doc__)
