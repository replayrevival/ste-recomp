# SPDX-License-Identifier: GPL-2.0-or-later
"""Synthetic tests for iso_to_mod.py prepare-stack (no game data): text-edit recipes on a made-up game folder.
Cases: each mod alone, both stacked in load order, a conflict (both change the same text), and order.json reordering."""
import os, sys, json, hashlib, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import iso_to_mod


def sha(b): return 'sha256:' + hashlib.sha256(b).hexdigest()


def make_mod(root, mid, target, base, edits, prio=100):
    d = os.path.join(root, mid); os.makedirs(os.path.join(d, 'edits'))
    res = base
    for o, n in edits: res = res.replace(o.encode(), n.encode())
    json.dump({'format': 'text-edit', 'edits': [{'old': o, 'new': n} for o, n in edits]}, open(os.path.join(d, 'edits', 'e.json'), 'w'))
    json.dump({'api': 2, 'id': mid, 'priority': prio, 'patches': [{'target': target, 'format': 'text-edit', 'patch': 'edits\\e.json',
               'base_sha256': sha(base), 'result_sha256': sha(res), 'size': len(res)}]}, open(os.path.join(d, 'manifest.json'), 'w'))
    return d


def run(root, game, enable=None):
    import io, contextlib
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf): rc = iso_to_mod.cmd_prepare_stack([root, game] + (['--enable', enable] if enable else []))
    return rc, buf.getvalue()


def main():
    base = b'alpha=1\nbeta=2\ngamma=3\n'
    work = tempfile.mkdtemp(prefix='stacktest_'); game = os.path.join(work, 'game'); os.makedirs(os.path.join(game, 'Data'))
    open(os.path.join(game, 'Data', 'x.txt'), 'wb').write(base)
    root = os.path.join(work, 'mods'); os.makedirs(root)
    a = make_mod(root, 'a.mod', 'Data\\x.txt', base, [('alpha=1', 'alpha=10')])
    b = make_mod(root, 'b.mod', 'Data\\x.txt', base, [('gamma=3', 'gamma=30')])
    built = lambda d: open(os.path.join(d, '_built', 'Data', 'x.txt'), 'rb').read()
    rc, out = run(root, game, 'a.mod'); assert rc == 0 and built(a) == b'alpha=10\nbeta=2\ngamma=3\n', out
    rc, out = run(root, game, 'b.mod'); assert rc == 0 and built(b) == b'alpha=1\nbeta=2\ngamma=30\n', out
    rc, out = run(root, game); assert rc == 0 and 'on top of a.mod' in out, out
    assert built(b) == b'alpha=10\nbeta=2\ngamma=30\n'
    sc = json.load(open(os.path.join(b, '_built', '_stack.json')))
    assert sc['targets']['Data\\x.txt']['sha256'] == hashlib.sha256(built(b)).hexdigest() and sc['targets']['Data\\x.txt']['stack'] == ['a.mod', 'b.mod']
    assert not os.path.exists(os.path.join(a, '_built', '_stack.json'))
    # conflict: c changes the same text as a
    c = make_mod(root, 'c.mod', 'Data\\x.txt', base, [('alpha=1\n', 'alpha=99\n')], prio=200)
    rc, out = run(root, game, 'a.mod,c.mod'); assert rc == 2 and 'CONFLICT c.mod: Data' in out and 'a.mod' in out, out
    assert not os.path.exists(os.path.join(c, '_built', 'Data', 'x.txt')) and built(a) == b'alpha=10\nbeta=2\ngamma=3\n'
    # order.json: c before a -> a is the one in conflict
    open(os.path.join(root, 'order.json'), 'w', encoding='utf-8-sig').write(json.dumps({'api': 1, 'order': ['c.mod', 'a.mod']}))   # with a UTF-8 BOM (PowerShell)
    rc, out = run(root, game, 'a.mod,c.mod'); assert rc == 2 and 'CONFLICT a.mod' in out, out
    print('stack tests: OK')


if __name__ == '__main__':
    main()
