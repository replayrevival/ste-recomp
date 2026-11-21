# SPDX-License-Identifier: GPL-2.0-or-later
# Minimal Xbox 16-bit linear texture helpers (no Pillow): A4R4G4B4 / A1R5G5B5 decode/encode + PNG writer.
import struct, zlib

def dec(fmt, raw, w, h, pitch):
    px = []
    for y in range(h):
        row = []
        for x in range(w):
            v = struct.unpack_from('<H', raw, y * pitch + x * 2)[0]
            if fmt == 'a4r4g4b4':
                a, r, g, b = (v >> 12) & 15, (v >> 8) & 15, (v >> 4) & 15, v & 15
                row.append((r * 17, g * 17, b * 17, a * 17))
            else:
                a, r, g, b = (v >> 15) & 1, (v >> 10) & 31, (v >> 5) & 31, v & 31
                row.append((r * 255 // 31, g * 255 // 31, b * 255 // 31, a * 255))
        px.append(row)
    return px

def enc(fmt, px, pitch):
    h = len(px); w = len(px[0]); out = bytearray(pitch * h)
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[y][x]
            if fmt == 'a4r4g4b4':
                v = ((a + 8) // 17 << 12) | ((r + 8) // 17 << 8) | ((g + 8) // 17 << 4) | ((b + 8) // 17)
            else:
                v = (1 << 15 if a >= 128 else 0) | (round(r * 31 / 255) << 10) | (round(g * 31 / 255) << 5) | round(b * 31 / 255)
            struct.pack_into('<H', out, y * pitch + x * 2, v)
    return bytes(out)

def png(path, px, bg=(40, 40, 40), scale=1):
    h = len(px); w = len(px[0]); rows = []
    for y in range(h):
        r = bytearray()
        for x in range(w):
            R, G, B, A = px[y][x]
            R = (R * A + bg[0] * (255 - A)) // 255; G = (G * A + bg[1] * (255 - A)) // 255; B = (B * A + bg[2] * (255 - A)) // 255
            r += bytes((R, G, B)) * scale
        for _ in range(scale): rows.append(b'\0' + bytes(r))
    def ch(t, c): return struct.pack('>I', len(c)) + t + c + struct.pack('>I', zlib.crc32(t + c) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', w * scale, h * scale, 8, 2, 0, 0, 0)) + ch(b'IDAT', zlib.compress(b''.join(rows), 6)) + ch(b'IEND', b''))

def hcat(imgs, gap=4):
    h = max(len(i) for i in imgs); out = []
    for y in range(h):
        row = []
        for k, i in enumerate(imgs):
            w = len(i[0])
            row += i[y] if y < len(i) else [(0, 0, 0, 0)] * w
            if k < len(imgs) - 1: row += [(0, 0, 0, 0)] * gap
        out.append(row)
    return out
