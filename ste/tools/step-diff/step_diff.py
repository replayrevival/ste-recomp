#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""step-diff: make, apply and inspect `step1` binary patches (docs/formats/mod_patch.md section 3).

A patch turns the user's ORIGINAL file into the modded file. It stores the SHA-256 of both and only the bytes the modder changed
(INSERT / ADD ops) plus references into the original (COPY ops), so it never contains the original's data.

  python step_diff.py make  <original> <modded> <out.step> [--no-zlib]
  python step_diff.py apply <original> <patch.step> <out>      (verifies base and result hashes and the crc)
  python step_diff.py info  <patch.step>
  python step_diff.py manifest-entry <original> <modded> <patch.step> <target-rel-path>   (prints the patches[] JSON entry)

The differ is simple (difflib on the byte strings, fine for files up to a few MB, linear for same-size files); it is meant for
modders' small edits and for tests. Larger rebuilds may use any other tool that writes the same container.
Only the Python standard library is used."""
import difflib, hashlib, json, struct, sys, zlib

MAGIC = b'STEPATC1'

def varint(n):
    out = bytearray()
    while True:
        b = n & 0x7F; n >>= 7
        out.append(b | (0x80 if n else 0))
        if not n: return bytes(out)

def read_varint(buf, pos):
    shift = val = 0
    while True:
        b = buf[pos]; pos += 1
        val |= (b & 0x7F) << shift; shift += 7
        if not b & 0x80: return val, pos

def sha(b): return hashlib.sha256(b).digest()

def make_ops(a, b):
    ops = []
    def copy(o, n):
        if n: ops.append((0, varint(o) + varint(n)))
    def ins(lit):
        if lit: ops.append((2, varint(len(lit)) + bytes(lit)))
    def add(o, new):
        d = bytes((new[i] - a[o + i]) & 0xFF for i in range(len(new)))
        ops.append((1, varint(o) + varint(len(new)) + d))
    if len(a) == len(b):   # same size: runs of equal bytes are COPY, differing runs are ADD deltas (small edits stay tiny)
        i, n = 0, len(a)
        while i < n:
            j = i
            if a[i] == b[i]:
                while j < n and a[j] == b[j]: j += 1
                copy(i, j - i)
            else:
                gap = 0
                while j < n and (a[j] != b[j] or (j + 1 < n and a[j + 1] != b[j + 1])): j += 1
                add(i, b[i:j])
            i = j
        return ops
    sm = difflib.SequenceMatcher(None, a, b, autojunk=False)
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == 'equal': copy(i1, i2 - i1)
        elif tag in ('replace', 'insert'): ins(b[j1:j2])
        # 'delete': nothing is emitted, the original bytes are simply not copied
    return ops

def make(orig, mod, out, use_zlib=True):
    a, b = open(orig, 'rb').read(), open(mod, 'rb').read()
    ops = make_ops(a, b)
    body = b''.join(bytes([k]) + p for k, p in ops)
    flags = 1 if use_zlib else 0
    if use_zlib: body = zlib.compress(body, 9)
    head = MAGIC + struct.pack('<QQ', len(a), len(b)) + sha(a) + sha(b) + struct.pack('<II', len(ops), flags)
    blob = head + body
    blob += struct.pack('<I', zlib.crc32(blob) & 0xFFFFFFFF)
    open(out, 'wb').write(blob)
    return a, b, len(ops), len(blob)

def parse(blob):
    if len(blob) < 100 or blob[:8] != MAGIC: raise ValueError('BAD PATCH: magic')
    if zlib.crc32(blob[:-4]) & 0xFFFFFFFF != struct.unpack('<I', blob[-4:])[0]: raise ValueError('BAD PATCH: crc')
    bs, rs = struct.unpack_from('<QQ', blob, 8); bh, rh = blob[24:56], blob[56:88]
    n, flags = struct.unpack_from('<II', blob, 88)
    body = blob[96:-4]
    if flags & 1: body = zlib.decompress(body)
    return bs, rs, bh, rh, n, body

def apply(orig_bytes, blob):
    bs, rs, bh, rh, n, body = parse(blob)
    if len(orig_bytes) != bs or sha(orig_bytes) != bh: raise ValueError('BASE MISMATCH: the original file is not the one the patch was made for')
    out = bytearray(); pos = 0
    for _ in range(n):
        k = body[pos]; pos += 1
        if k == 0:
            o, pos = read_varint(body, pos); l, pos = read_varint(body, pos)
            if o + l > len(orig_bytes): raise ValueError('BAD PATCH: copy outside the original')
            out += orig_bytes[o:o + l]
        elif k == 1:
            o, pos = read_varint(body, pos); l, pos = read_varint(body, pos)
            if o + l > len(orig_bytes) or pos + l > len(body): raise ValueError('BAD PATCH: add outside the data')
            out += bytes((orig_bytes[o + i] + body[pos + i]) & 0xFF for i in range(l)); pos += l
        elif k == 2:
            l, pos = read_varint(body, pos)
            if pos + l > len(body): raise ValueError('BAD PATCH: insert outside the data')
            out += body[pos:pos + l]; pos += l
        else: raise ValueError('BAD PATCH: op %d' % k)
        if len(out) > rs: raise ValueError('BAD PATCH: result too long')
    if len(out) != rs or sha(bytes(out)) != rh: raise ValueError('RESULT MISMATCH')
    return bytes(out)

def main(argv):
    if len(argv) >= 5 and argv[1] == 'make':
        a, b, nops, size = make(argv[2], argv[3], argv[4], '--no-zlib' not in argv)
        print('patch %s: %d ops, %d bytes (original %d, result %d)' % (argv[4], nops, size, len(a), len(b))); return 0
    if len(argv) == 5 and argv[1] == 'apply':
        try: out = apply(open(argv[2], 'rb').read(), open(argv[3], 'rb').read())
        except ValueError as e: print(e); return 2
        open(argv[4], 'wb').write(out); print('built %s: %d bytes sha256:%s' % (argv[4], len(out), hashlib.sha256(out).hexdigest())); return 0
    if len(argv) == 3 and argv[1] == 'info':
        bs, rs, bh, rh, n, body = parse(open(argv[2], 'rb').read())
        print('base %d bytes sha256:%s\nresult %d bytes sha256:%s\nops %d' % (bs, bh.hex(), rs, rh.hex(), n)); return 0
    if len(argv) == 6 and argv[1] == 'manifest-entry':
        a, b = open(argv[2], 'rb').read(), open(argv[3], 'rb').read()
        print(json.dumps({'target': argv[5], 'base_sha256': 'sha256:' + hashlib.sha256(a).hexdigest(), 'result_sha256': 'sha256:' + hashlib.sha256(b).hexdigest(),
                          'patch': 'patches\\' + argv[4].replace('/', '\\').split('\\')[-1], 'size': len(b)}, indent=2)); return 0
    print(__doc__); return 1

if __name__ == '__main__':
    sys.exit(main(sys.argv))
