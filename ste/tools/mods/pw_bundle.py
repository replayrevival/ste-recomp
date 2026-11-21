# SPDX-License-Identifier: GPL-2.0-or-later
"""pw_bundle: read, edit and write Pipeworks bundles (.bdl, "Pipeworks Bundle File version 1.3", little endian).

Own implementation from the observed layout (docs/formats/pipeworks_bundle.md). Stdlib only.
No game data is stored here; the tool only works on files the user supplies.

Layout (offsets are absolute in the file):
  0x00 magic text, 0x34 u32 name-table offset, 0x62 u16 entry count, 0x64 u32 metadata offset,
  0x68 u32 main-data offset, 0x70 u32 resource-data offset, 0x3C..0x5C nine u32 pool pointers (just after the TOC),
  0x78 TOC, 18 bytes per entry: u16 file number, u32 main rel offset, u32 main size, u32 res rel offset, u32 res size.
  name table: u32 count, count x u32 offsets (relative to the table), NUL terminated names (sorted, case-insensitive).
  metadata: 16 bytes per entry: u16 file number, u16 type, u32 name id (index in the name table), u32 1, u32 0 (or text size).

Round trip: Bundle.parse(data).emit() == data for an unmodified bundle (the sections are kept as raw bytes).
Edits: set_main/set_res replace an entry in place (when it fits) or append it at the section end (16 byte aligned);
add_entry appends a new entry; the name must sort after every existing name (see the format note: inserting names in the
middle of the table crashes the title), add_entry checks that.
"""
import struct, sys

HDR = 0x78
TOC = 18
ALIGN = 16


def _align(n, a=ALIGN):
    return (n + a - 1) // a * a


class Entry(object):
    __slots__ = ('num', 'main_rel', 'main_size', 'res_rel', 'res_size', 'meta')

    def __init__(self, num, main_rel, main_size, res_rel, res_size, meta):
        self.num, self.main_rel, self.main_size, self.res_rel, self.res_size, self.meta = num, main_rel, main_size, res_rel, res_size, meta

    @property
    def type(self):
        return struct.unpack_from('<H', self.meta, 2)[0]

    @property
    def name_id(self):
        return struct.unpack_from('<I', self.meta, 4)[0]


class Bundle(object):
    @classmethod
    def parse(cls, data):
        data = bytes(data)
        if data[:9] != b'Pipeworks':
            raise ValueError('not a Pipeworks bundle')
        self = cls()
        self.header = bytearray(data[:HDR])
        n = struct.unpack_from('<H', data, 0x62)[0]
        so, mo, ma, re_ = [struct.unpack_from('<I', data, o)[0] for o in (0x34, 0x64, 0x68, 0x70)]
        toc_end = HDR + TOC * n
        if not (toc_end <= so <= mo <= ma <= re_ <= len(data)):
            raise ValueError('section offsets out of order')
        self.pool = data[toc_end:so]                       # pool records etc. between the TOC and the name table
        cnt = struct.unpack_from('<I', data, so)[0]
        offs = struct.unpack_from('<%dI' % cnt, data, so + 4)
        names = []
        for o in offs:
            e = data.index(b'\0', so + o)
            names.append(data[so + o:e].decode('latin-1'))
        self.names = names
        self._name_raw = data[so:mo]
        self._name_dirty = False
        meta = data[mo:mo + 16 * n]
        self.meta_tail = data[mo + 16 * n:ma]
        self.entries = []
        for i in range(n):
            t = struct.unpack_from('<HIIII', data, HDR + TOC * i)
            self.entries.append(Entry(t[0], t[1], t[2], t[3], t[4], bytearray(meta[16 * i:16 * i + 16])))
        main_end = max([e.main_rel + e.main_size for e in self.entries] + [0])
        res_end = max([e.res_rel + e.res_size for e in self.entries] + [0])
        self.main = bytearray(data[ma:re_])
        self.res = bytearray(data[re_:re_ + res_end])
        self.tail = data[re_ + res_end:]
        p0 = struct.unpack_from('<I', data, 0x3C)[0]
        self.pool_end = struct.unpack_from('<I', data, p0 + 4)[0] if p0 + 8 <= len(data) else 0
        self.res_len0 = len(self.res)
        if len(self.main) < main_end:
            raise ValueError('main section shorter than the TOC says')
        self._dirty = False
        return self

    # ---- reading
    def name(self, e):
        return self.names[e.name_id] if e.name_id < len(self.names) else None

    def find(self, name):
        low = name.lower()
        for e in self.entries:
            nm = self.name(e)
            if nm is not None and nm.lower() == low:
                return e
        return None

    def get_main(self, e):
        return bytes(self.main[e.main_rel:e.main_rel + e.main_size])

    def get_res(self, e):
        return bytes(self.res[e.res_rel:e.res_rel + e.res_size])

    # ---- editing
    def _put(self, buf, rel, size, data, align=ALIGN):
        if len(data) <= size and size:
            buf[rel:rel + len(data)] = data
            if len(data) < size:
                buf[rel + len(data):rel + size] = b'\0' * (size - len(data))
            return rel, len(data)
        pos = _align(len(buf), align)
        buf.extend(b'\xff' * (pos - len(buf)))
        buf.extend(data)
        return pos, len(data)

    def set_main(self, e, data):
        e.main_rel, e.main_size = self._put(self.main, e.main_rel, e.main_size, bytes(data)) if data else (e.main_rel, 0)
        if e.type == 2:                                       # text entries keep their byte size in the metadata (4th dword)
            struct.pack_into('<I', e.meta, 12, e.main_size)
        self._dirty = True

    def set_res(self, e, data, align=ALIGN):
        e.res_rel, e.res_size = self._put(self.res, e.res_rel, e.res_size, bytes(data), align) if data else (e.res_rel, 0)
        self._dirty = True

    def add_entry(self, name, etype, main=b'', res=b'', text_size=None, res_align=ALIGN):
        low = name.lower()
        if self.names and low <= max(n.lower() for n in self.names):
            raise ValueError('new name %r must sort after every existing name (use a Zz_ prefix)' % name)
        nid = len(self.names)
        self.names.append(name)
        self._name_dirty = True
        num = max([e.num for e in self.entries] + [-1]) + 1
        meta = bytearray(struct.pack('<HHIII', num, etype, nid, 1, text_size if text_size else 0))
        e = Entry(num, 0, 0, 0, 0, meta)
        self.entries.append(e)
        # a new entry starts empty; the data goes through the append path
        e.main_rel = _align(len(self.main)); e.res_rel = _align(len(self.res), res_align)
        if main:
            self.main.extend(b'\xff' * (e.main_rel - len(self.main))); self.main.extend(main); e.main_size = len(main)
        if res:
            self.res.extend(b'\xff' * (e.res_rel - len(self.res))); self.res.extend(res); e.res_size = len(res)
        if etype == 2:
            struct.pack_into('<I', e.meta, 12, e.main_size)
        self._dirty = True
        return e

    def clone_entry(self, src, name, main=None, res=None, res_align=ALIGN):
        """Add `name` as a copy of entry `src` (optionally with other main/res bytes)."""
        return self.add_entry(name, src.type, self.get_main(src) if main is None else main,
                              self.get_res(src) if res is None else res,
                              struct.unpack_from('<I', src.meta, 12)[0], res_align)

    # ---- writing
    def _name_blob(self):
        if not self._name_dirty:
            return self._name_raw
        n = len(self.names)
        body = b''.join(s.encode('latin-1') + b'\0' for s in self.names)
        offs, p = [], 4 + 4 * n
        for s in self.names:
            offs.append(p); p += len(s) + 1
        blob = struct.pack('<I', n) + struct.pack('<%dI' % n, *offs) + body
        return blob + b'\xff' * (_align(len(blob)) - len(blob))

    def emit(self):
        n = len(self.entries)
        hdr = bytearray(self.header)
        toc = b''.join(struct.pack('<HIIII', e.num, e.main_rel, e.main_size, e.res_rel, e.res_size) for e in self.entries)
        old_n = struct.unpack_from('<H', self.header, 0x62)[0]
        delta = TOC * (n - old_n)
        for o in range(0x3C, 0x60, 4):                       # pool pointers follow the TOC
            v = struct.unpack_from('<I', hdr, o)[0]
            if delta and v >= HDR + TOC * old_n:
                struct.pack_into('<I', hdr, o, v + delta)
        pool = self.pool
        names = self._name_blob()
        meta = b''.join(bytes(e.meta) for e in self.entries) + self.meta_tail
        so = HDR + len(toc) + len(pool)
        mo = so + len(names)
        ma = mo + len(meta)
        re_ = ma + len(self.main)
        struct.pack_into('<H', hdr, 0x62, n)
        for o, v in ((0x34, so), (0x64, mo), (0x68, ma), (0x70, re_)):
            struct.pack_into('<I', hdr, o, v)
        pool = bytearray(pool)
        if len(self.res) > self.res_len0 and self.pool_end:
            # the nine pool records hold the end of the resource section; a stale end makes the title ignore appended resources
            new_end = _align(re_ + len(self.res))
            for k in range(9):
                po = struct.unpack_from('<I', hdr, 0x3C + 4 * k)[0] - (HDR + len(toc))
                for q in (4, 8, 16):
                    v = struct.unpack_from('<I', pool, po + q)[0]
                    if v == self.pool_end: struct.pack_into('<I', pool, po + q, new_end)
                    elif v == self.pool_end + 0x40: struct.pack_into('<I', pool, po + q, new_end + 0x40)
        return bytes(hdr) + toc + bytes(pool) + names + meta + bytes(self.main) + bytes(self.res) + self.tail


def _selftest(paths):
    ok = True
    for p in paths:
        d = open(p, 'rb').read()
        b = Bundle.parse(d)
        same = b.emit() == d
        print('%s: %d entries, %d names, round trip %s' % (p, len(b.entries), len(b.names), 'IDENTICAL' if same else 'DIFFERENT'))
        ok &= same
    return ok


if __name__ == '__main__':
    if len(sys.argv) < 3 or sys.argv[1] not in ('check', 'list'):
        print('usage: pw_bundle.py check <bundle>...   (round trip)\n       pw_bundle.py list <bundle>'); sys.exit(2)
    if sys.argv[1] == 'check':
        sys.exit(0 if _selftest(sys.argv[2:]) else 1)
    b = Bundle.parse(open(sys.argv[2], 'rb').read())
    for e in b.entries:
        print('%4d type %2d main %8d res %9d  %s' % (e.num, e.type, e.main_size, e.res_size, b.name(e)))
