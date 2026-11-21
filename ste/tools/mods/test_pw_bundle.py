# SPDX-License-Identifier: GPL-2.0-or-later
"""Synthetic tests for pw_bundle (no game data). Optional: PW_BUNDLE_DIR=<folder> also round-trips every .bdl in it."""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pw_bundle


def make(names, blobs):
    n = len(names)
    toc, main, res = b'', bytearray(), bytearray()
    meta = b''
    for i, (nm, (m, r)) in enumerate(zip(names, blobs)):
        mr = len(main); main += m + b'\xff' * ((-len(main) - len(m)) % 16)
        rr = len(res); res += r + b'\xff' * ((-len(res) - len(r)) % 16)
        toc += struct.pack('<HIIII', i, mr, len(m), rr, len(r))
        meta += struct.pack('<HHIII', i, 9, i, 1, 0)
    body = b''.join(s.encode() + b'\0' for s in names)
    offs, p = [], 4 + 4 * n
    for s in names:
        offs.append(p); p += len(s) + 1
    nb = struct.pack('<I', n) + struct.pack('<%dI' % n, *offs) + body
    nb += b'\xff' * (-len(nb) % 16)
    pool = b'\0' * 288
    so = 0x78 + len(toc) + len(pool); mo = so + len(nb); ma = mo + len(meta); re_ = ma + len(main)
    h = bytearray(0x78); magic = b'Pipeworks Bundle File version 1.3'; h[:len(magic)] = magic
    struct.pack_into('<I', h, 0x34, so); struct.pack_into('<H', h, 0x62, n)
    struct.pack_into('<III', h, 0x64, mo, ma, 0); struct.pack_into('<I', h, 0x70, re_)
    for k in range(9):
        struct.pack_into('<I', h, 0x3C + 4 * k, 0x78 + len(toc) + 32 * k)
    return bytes(h) + toc + pool + nb + meta + bytes(main) + bytes(res) + b'\0' * 40


def test_round_trip():
    d = make(['aa', 'bb', 'cc'], [(b'x' * 20, b'r' * 7), (b'y' * 5, b''), (b'z' * 33, b'q' * 100)])
    assert pw_bundle.Bundle.parse(d).emit() == d


def test_edit_and_add():
    d = make(['aa', 'bb', 'cc'], [(b'x' * 20, b'r' * 7), (b'y' * 5, b''), (b'z' * 33, b'q' * 100)])
    b = pw_bundle.Bundle.parse(d)
    b.set_main(b.find('BB'), b'new' * 10)                       # grows: appended
    b.set_res(b.find('aa'), b'R' * 3)                            # fits: in place
    e = b.clone_entry(b.find('cc'), 'Zz_dd')
    b.set_res(e, b'tile' * 8)
    out = pw_bundle.Bundle.parse(b.emit())
    assert out.get_main(out.find('bb')) == b'new' * 10
    assert out.get_res(out.find('aa')) == b'R' * 3
    assert out.get_main(out.find('zz_dd')) == b'z' * 33 and out.get_res(out.find('zz_dd')) == b'tile' * 8
    assert out.names == ['aa', 'bb', 'cc', 'Zz_dd'] and out.find('cc').type == 9
    assert out.get_res(out.find('cc')) == b'q' * 100


def test_name_order_enforced():
    b = pw_bundle.Bundle.parse(make(['aa', 'bb'], [(b'1', b''), (b'2', b'')]))
    try:
        b.add_entry('ab', 9)
    except ValueError:
        return
    raise AssertionError('expected ValueError')


if __name__ == '__main__':
    test_round_trip(); test_edit_and_add(); test_name_order_enforced()
    ok = True
    for root, _, fs in os.walk(os.environ.get('PW_BUNDLE_DIR', '')) if os.environ.get('PW_BUNDLE_DIR') else []:
        for f in fs:
            if f.lower().endswith('.bdl'):
                p = os.path.join(root, f); d = open(p, 'rb').read(); ok &= pw_bundle.Bundle.parse(d).emit() == d
    print('pw_bundle tests: OK' if ok else 'pw_bundle tests: round trip FAILED'); sys.exit(0 if ok else 1)
