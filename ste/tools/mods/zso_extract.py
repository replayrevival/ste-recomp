# SPDX-License-Identifier: GPL-2.0-or-later
"""zso_extract.py: read a PS2 ZISO (.zso, LZ4 blocks) disc image READ-ONLY and extract chosen files or the whole ISO9660 file list, without any other tool.
Own code (ZISO layout: header 'ZISO', header size u32, total size u64, block size u32, version u8, align u8; index of u32 per block + 1, bit 31 = stored raw).
usage:  zso_extract.py <image.zso> --list
        zso_extract.py <image.zso> --extract <path inside the disc> <output file>   (e.g. --extract /GZ3PS2.VOL F:\\work\\GZ3PS2.VOL)
        zso_extract.py <image.zso> --iso <output.iso>                               (full conversion, large)
Never writes next to the image; the outputs go where you say."""
import struct, sys

class Zso:
    def __init__(self, path):
        self.f = open(path, 'rb')
        h = self.f.read(0x18)
        if h[:4] != b'ZISO': raise ValueError('not a ZISO image')
        hs, self.size, self.bs, ver, self.align = struct.unpack_from('<IQIBB', h, 4)
        self.nb = (self.size + self.bs - 1) // self.bs
        self.f.seek(hs); self.idx = struct.unpack('<%dI' % (self.nb + 1), self.f.read(4 * (self.nb + 1)))
        self.cache = {}

    @staticmethod
    def lz4_block(src, out_size):
        out = bytearray(); i = 0; n = len(src)
        while i < n:
            tok = src[i]; i += 1
            ll = tok >> 4
            if ll == 15:
                while True:
                    b = src[i]; i += 1; ll += b
                    if b != 255: break
            out += src[i:i + ll]; i += ll
            if i >= n or len(out) >= out_size: break   # the last sequence has literals only; blocks are padded to the alignment
            off = src[i] | (src[i + 1] << 8); i += 2
            ml = tok & 15
            if ml == 15:
                while True:
                    b = src[i]; i += 1; ml += b
                    if b != 255: break
            ml += 4
            p = len(out) - off
            if off >= ml: out += out[p:p + ml]
            else:
                for k in range(ml): out.append(out[p + k])
        return bytes(out[:out_size])

    def block(self, k):
        if k in self.cache: return self.cache[k]
        a = self.idx[k]; b = self.idx[k + 1]
        raw = bool(a & 0x80000000)
        a = (a & 0x7FFFFFFF) << self.align; b = (b & 0x7FFFFFFF) << self.align
        self.f.seek(a); d = self.f.read(b - a)
        out = d if raw else self.lz4_block(d, self.bs)
        if len(self.cache) > 64: self.cache.clear()
        self.cache[k] = out
        return out

    def read(self, off, n):
        out = bytearray()
        while n > 0 and off < self.size:
            k = off // self.bs; o = off % self.bs
            blk = self.block(k); take = min(n, len(blk) - o)
            if take <= 0: break
            out += blk[o:o + take]; off += take; n -= take
        return bytes(out)

def iso_entries(z, lba, size, path=''):
    data = z.read(lba * 2048, size); i = 0
    while i < len(data):
        l = data[i]
        if l == 0:
            i = (i // 2048 + 1) * 2048; continue
        e_lba, = struct.unpack_from('<I', data, i + 2); e_len, = struct.unpack_from('<I', data, i + 10)
        fl = data[i + 25]; nl = data[i + 32]; name = data[i + 33:i + 33 + nl].decode('latin-1')
        i += l
        if name in ('\0', '\1'): continue
        name = name.split(';')[0]
        full = path + '/' + name
        if fl & 2: yield from iso_entries(z, e_lba, e_len, full)
        else: yield (full, e_lba, e_len)

def entries(z):
    pvd = z.read(16 * 2048, 2048)
    if pvd[1:6] != b'CD001': raise ValueError('no ISO9660 volume descriptor')
    root_lba, = struct.unpack_from('<I', pvd, 156 + 2); root_len, = struct.unpack_from('<I', pvd, 156 + 10)
    return list(iso_entries(z, root_lba, root_len))

def main(a):
    z = Zso(a[0])
    if a[1] == '--list':
        for p, l, n in entries(z): print('%10d  lba %7d  %s' % (n, l, p))
    elif a[1] == '--extract':
        want = a[2].lower()
        for p, l, n in entries(z):
            if p.lower() == want:
                with open(a[3], 'wb') as o:
                    done = 0
                    while done < n:
                        c = min(1 << 24, n - done); o.write(z.read(l * 2048 + done, c)); done += c
                print('extracted', p, n); return
        print('not found'); sys.exit(2)
    elif a[1] == '--iso':
        with open(a[2], 'wb') as o:
            for k in range(z.nb): o.write(z.block(k))
        print('written', a[2])

if __name__ == '__main__':
    main(sys.argv[1:])
