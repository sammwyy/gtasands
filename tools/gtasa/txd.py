"""TXD texture dictionaries (PC D3D8/D3D9 natives) and pixel decoding to RGBA8."""
import struct

from . import rw

FMT_1555, FMT_565, FMT_4444, FMT_LUM8, FMT_8888, FMT_888, FMT_555 = 0x100, 0x200, 0x300, 0x400, 0x500, 0x600, 0xA00
PAL8, PAL4 = 0x2000, 0x4000


class Texture:
    __slots__ = ('name', 'mask', 'width', 'height', 'raster', 'dxt', 'has_alpha', 'palette', 'levels')

    def __repr__(self):
        kind = f'DXT{self.dxt}' if self.dxt else hex(self.raster & 0xF00 | self.raster & (PAL8 | PAL4))
        return f'<{self.name} {self.width}x{self.height} {kind} alpha={int(self.has_alpha)} mips={len(self.levels)}>'


def parse(data):
    out = []
    for t, s, e, _ in rw.chunks(data):
        if t == rw.TEXDICT:
            for t2, s2, e2, _ in rw.chunks(data, s, e):
                if t2 == rw.TEXNATIVE:
                    out.append(_native(data, s2, e2))
    return out


def _native(data, s, e):
    o, _ = rw.find(data, s, e, rw.STRUCT)
    tx = Texture()
    plat, = struct.unpack_from('<I', data, o)
    tx.name = rw.cstr(data[o + 8:o + 40])
    tx.mask = rw.cstr(data[o + 40:o + 72])
    tx.raster, fmt = struct.unpack_from('<II', data, o + 72)
    tx.width, tx.height, _depth, nlev, _rtype, flags = struct.unpack_from('<HHBBBB', data, o + 80)
    o += 88
    if plat == 9:
        four = fmt.to_bytes(4, 'little')
        tx.dxt = int(four[3:]) if four[:3] == b'DXT' else 0
        tx.has_alpha = bool(flags & 1)
    elif plat == 8:
        tx.dxt = flags
        tx.has_alpha = bool(fmt)
    else:
        raise ValueError(f'unsupported texture platform {plat}')
    tx.palette = None
    if tx.raster & PAL8:
        tx.palette = data[o:o + 1024]
        o += 1024
    elif tx.raster & PAL4:
        tx.palette = data[o:o + 128]
        o += 128
    tx.levels = []
    for _ in range(nlev):
        sz, = struct.unpack_from('<I', data, o)
        tx.levels.append(data[o + 4:o + 4 + sz])
        o += 4 + sz
    return tx


def level_size(tx, level):
    return max(1, tx.width >> level), max(1, tx.height >> level)


def decode(tx, level=0):
    """Returns (w, h, bytearray RGBA8)."""
    w, h = level_size(tx, level)
    d = tx.levels[level]
    if tx.dxt:
        return w, h, _dxt(d, w, h, tx.dxt)
    n = w * h
    out = bytearray(n * 4)
    if tx.palette is not None:
        pal = tx.palette
        if tx.raster & PAL4:
            idx = [(d[i >> 1] >> (4 * (i & 1))) & 15 for i in range(n)]
        else:
            idx = d[:n]
        for i, c in enumerate(idx):
            out[i * 4:i * 4 + 4] = pal[c * 4:c * 4 + 4]
        return w, h, out
    f = tx.raster & 0xF00
    if f in (FMT_8888, FMT_888):
        out[0::4], out[1::4], out[2::4] = d[2:n * 4:4], d[1:n * 4:4], d[0:n * 4:4]
        out[3::4] = d[3:n * 4:4] if f == FMT_8888 and tx.has_alpha else b'\xff' * n
        return w, h, out
    if f == FMT_LUM8:
        out[0::4] = out[1::4] = out[2::4] = d[:n]
        out[3::4] = b'\xff' * n
        return w, h, out
    px = struct.unpack_from(f'<{n}H', d)
    for i, p in enumerate(px):
        if f == FMT_565:
            r, g, b, a = (p >> 11) << 3, ((p >> 5) & 63) << 2, (p & 31) << 3, 255
        elif f == FMT_4444:
            r, g, b, a = ((p >> 8) & 15) * 17, ((p >> 4) & 15) * 17, (p & 15) * 17, (p >> 12) * 17
        else:
            r, g, b = ((p >> 10) & 31) << 3, ((p >> 5) & 31) << 3, (p & 31) << 3
            a = 255 if f == FMT_555 or p & 0x8000 else 0
        out[i * 4:i * 4 + 4] = bytes((r, g, b, a))
    return w, h, out


def _c565(c):
    r, g, b = c >> 11, (c >> 5) & 63, c & 31
    return (r << 3 | r >> 2), (g << 2 | g >> 4), (b << 3 | b >> 2)


def _dxt(d, w, h, kind):
    out = bytearray(w * h * 4)
    bsz = 8 if kind == 1 else 16
    o = 0
    for by in range((h + 3) // 4):
        for bx in range((w + 3) // 4):
            if o + bsz > len(d):
                return out
            co = o if kind == 1 else o + 8
            c0, c1, bits = struct.unpack_from('<HHI', d, co)
            r0, g0, b0 = _c565(c0)
            r1, g1, b1 = _c565(c1)
            if c0 > c1 or kind != 1:
                pal = ((r0, g0, b0, 255), (r1, g1, b1, 255),
                       ((2 * r0 + r1) // 3, (2 * g0 + g1) // 3, (2 * b0 + b1) // 3, 255),
                       ((r0 + 2 * r1) // 3, (g0 + 2 * g1) // 3, (b0 + 2 * b1) // 3, 255))
            else:
                pal = ((r0, g0, b0, 255), (r1, g1, b1, 255),
                       ((r0 + r1) // 2, (g0 + g1) // 2, (b0 + b1) // 2, 255), (0, 0, 0, 0))
            alpha = None
            if kind == 3:
                a64 = int.from_bytes(d[o:o + 8], 'little')
                alpha = [((a64 >> (4 * i)) & 15) * 17 for i in range(16)]
            elif kind == 5:
                a0, a1 = d[o], d[o + 1]
                if a0 > a1:
                    at = [a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
                else:
                    at = [a0, a1] + [((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)] + [0, 255]
                a48 = int.from_bytes(d[o + 2:o + 8], 'little')
                alpha = [at[(a48 >> (3 * i)) & 7] for i in range(16)]
            for i in range(16):
                x, y = bx * 4 + (i & 3), by * 4 + (i >> 2)
                if x < w and y < h:
                    c = pal[(bits >> (2 * i)) & 3]
                    p = (y * w + x) * 4
                    out[p:p + 4] = bytes((c[0], c[1], c[2], alpha[i] if alpha else c[3]))
            o += bsz
    return out
