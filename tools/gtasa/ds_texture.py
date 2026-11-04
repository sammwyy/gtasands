"""TXD -> DTXD (DS paletted/direct textures)."""
import struct

from . import txd
from .dsfmt import name, pad4, rgb15

# GL_TEXTURE_TYPE_ENUM (libnds videoGL.h)
A3I5, PAL4, PAL16, PAL256, A5I3, DIRECT = 1, 2, 3, 4, 6, 7
FORMATS = {'a3i5': A3I5, 'pal4': PAL4, 'pal16': PAL16, 'pal256': PAL256, 'a5i3': A5I3, 'direct': DIRECT}
BPP = {A3I5: 8, PAL4: 2, PAL16: 4, PAL256: 8, A5I3: 8, DIRECT: 16}
FLAG_COLOR0_TRANSPARENT, FLAG_ALPHA = 1, 2

DEFAULTS = dict(scale=1.0, max_size=256, min_size=8, format='auto', auto_colors=16,
                alpha_threshold=24, opaque=False, additive=False)


def _pow2_floor(n):
    p = 1
    while p * 2 <= n:
        p *= 2
    return p


def target_size(w, h, p):
    def one(n):
        return max(p['min_size'], min(p['max_size'], _pow2_floor(max(1, int(n * p['scale'])))))
    return one(w), one(h)


def pick_level(tx, tw, th):
    lv = 0
    while lv + 1 < len(tx.levels):
        w, h = txd.level_size(tx, lv + 1)
        if w < tw or h < th:
            break
        lv += 1
    return lv


def resize(px, sw, sh, tw, th):
    """Alpha-weighted box filter when shrinking by an integer factor, nearest otherwise."""
    if (sw, sh) == (tw, th):
        return px
    out = bytearray(tw * th * 4)
    if sw % tw == 0 and sh % th == 0:
        fx, fy = sw // tw, sh // th
        for y in range(th):
            for x in range(tw):
                r = g = b = a = 0
                for yy in range(y * fy, y * fy + fy):
                    row = yy * sw
                    for xx in range(x * fx, x * fx + fx):
                        p = (row + xx) * 4
                        al = px[p + 3]
                        r += px[p] * al; g += px[p + 1] * al; b += px[p + 2] * al; a += al
                o = (y * tw + x) * 4
                if a:
                    out[o:o + 4] = bytes((r // a, g // a, b // a, a // (fx * fy)))
        return out
    for y in range(th):
        sy = min(sh - 1, y * sh // th)
        for x in range(tw):
            sx = min(sw - 1, x * sw // tw)
            p = (sy * sw + sx) * 4
            o = (y * tw + x) * 4
            out[o:o + 4] = px[p:p + 4]
    return out


def median_cut(hist, n):
    """hist: {rgb15: count} -> list of rgb15 palette entries (<= n)."""
    def box(cs):
        total = sum(c[3] for c in cs)
        if len(cs) < 2:
            return (0, 0, total, cs)
        rngs = [max(c[ax] for c in cs) - min(c[ax] for c in cs) for ax in range(3)]
        ax = max(range(3), key=rngs.__getitem__)
        return (rngs[ax] * total, ax, total, cs)

    boxes = [box([((c & 31), (c >> 5) & 31, (c >> 10) & 31, k) for c, k in hist.items()])]
    while len(boxes) < n:
        bi = max(range(len(boxes)), key=lambda i: boxes[i][0])
        score, axis, total, cs = boxes[bi]
        if score <= 0:
            break
        boxes.pop(bi)
        cs = sorted(cs, key=lambda c: c[axis])
        acc, cut = 0, 1
        for j, c in enumerate(cs):
            acc += c[3]
            if acc * 2 >= total:
                cut = max(1, min(len(cs) - 1, j + 1))
                break
        boxes += [box(cs[:cut]), box(cs[cut:])]
    pal = []
    for *_, bx in boxes:
        t = sum(c[3] for c in bx) or 1
        r, g, b = (round(sum(c[k] * c[3] for c in bx) / t) for k in range(3))
        pal.append(r | g << 5 | b << 10)
    return pal


def _mapper(pal):
    comps = [(p & 31, (p >> 5) & 31, (p >> 10) & 31) for p in pal]
    cache = {}

    def m(c):
        i = cache.get(c)
        if i is None:
            r, g, b = c & 31, (c >> 5) & 31, (c >> 10) & 31
            i = min(range(len(comps)), key=lambda k: (comps[k][0] - r) ** 2 + (comps[k][1] - g) ** 2 + (comps[k][2] - b) ** 2)
            cache[c] = i
        return i
    return m


def additive_to_alpha(px):
    """PC additive sprites (blend ONE/ONE: coronas, moon, clouds) -> alpha-blend
    equivalent: alpha = brightest channel, colour normalised to full intensity.
    The DS has no additive blending; over a dark sky a*c + (1-a)*dst ~ dst + c."""
    out = bytearray(px)
    for i in range(0, len(px), 4):
        r, g, b, a = px[i:i + 4]
        m = max(r, g, b)
        if m:
            out[i:i + 4] = bytes((r * 255 // m, g * 255 // m, b * 255 // m, m * a // 255))
        else:
            out[i:i + 4] = b'\0\0\0\0'
    return out


def encode(px, w, h, p):
    """RGBA8 -> (format, flags, texel bytes, palette list)."""
    n = w * h
    if p.get('additive'):
        px = additive_to_alpha(px)
    cols = [rgb15(px[i * 4], px[i * 4 + 1], px[i * 4 + 2]) for i in range(n)]
    alpha = px[3::4] if not p['opaque'] else b'\xff' * n
    th = p['alpha_threshold']
    has_alpha = any(a < 255 - th for a in alpha)
    binary = all(a <= th or a >= 255 - th for a in alpha)
    fmt = p['format']
    if fmt == 'auto':
        uniq = len(set(cols)) + (1 if has_alpha else 0)
        if has_alpha and not binary:
            fmt = 'a3i5'
        elif uniq <= 4:
            fmt = 'pal4'
        else:
            fmt = 'pal256' if p['auto_colors'] > 16 else 'pal16'
    f = FORMATS[fmt]
    flags = FLAG_ALPHA if has_alpha else 0
    if f == DIRECT:
        tex = struct.pack(f'<{n}H', *(c | (0x8000 if a > th else 0) for c, a in zip(cols, alpha)))
        return f, flags, tex, []
    if f in (A3I5, A5I3):
        ncol, abits = (32, 3) if f == A3I5 else (8, 5)
        hist = {}
        for c, a in zip(cols, alpha):
            if a > th:
                hist[c] = hist.get(c, 0) + 1
        pal = median_cut(hist or {0: 1}, ncol)
        m = _mapper(pal)
        sh = 8 - abits
        tex = bytes(m(c) | (a >> sh) << (8 - abits) if a > th else 0 for c, a in zip(cols, alpha))
        return f, flags, tex, pal
    ncol = {PAL4: 4, PAL16: 16, PAL256: 256}[f]
    transparent = has_alpha
    hist = {}
    for c, a in zip(cols, alpha):
        if not transparent or a > th:
            hist[c] = hist.get(c, 0) + 1
    pal = median_cut(hist or {0: 1}, ncol - (1 if transparent else 0))
    m = _mapper(pal)
    off = 0
    if transparent:
        pal = [0] + pal
        flags |= FLAG_COLOR0_TRANSPARENT
        off = 1
    idx = [0 if transparent and a <= th else m(c) + off for c, a in zip(cols, alpha)]
    if f == PAL256:
        tex = bytes(idx)
    elif f == PAL16:
        tex = bytes(idx[i] | idx[i + 1] << 4 for i in range(0, n, 2))
    else:
        tex = bytes(idx[i] | idx[i + 1] << 2 | idx[i + 2] << 4 | idx[i + 3] << 6 for i in range(0, n, 4))
    return f, flags, tex, pal


def convert_texture(tx, p):
    tw, th = target_size(tx.width, tx.height, p)
    lv = pick_level(tx, tw, th)
    sw, sh, px = txd.decode(tx, lv)
    px = resize(px, sw, sh, tw, th)
    f, flags, tex, pal = encode(px, tw, th, p)
    hdr = struct.pack('<4sHHBBHI', b'DTEX', tw, th, f, flags, len(pal), len(tex))
    return hdr + pad4(tex) + struct.pack(f'<{len(pal)}H', *pal)


def convert_txd(data, p, overrides=()):
    """overrides: [(fnmatch_pattern, params)] matched against texture names."""
    from fnmatch import fnmatchcase
    texs = txd.parse(data)
    blobs = []
    for tx in texs:
        tp = p
        for pat, op in overrides:
            if fnmatchcase(tx.name.lower(), pat.lower()):
                tp = {**tp, **op}
        blobs.append((tx.name, convert_texture(tx, tp)))
    out = bytearray(struct.pack('<4sHH', b'DTXD', 1, len(blobs)))
    off = len(out) + 40 * len(blobs)
    for nm, b in blobs:
        out += name(nm, 32) + struct.pack('<II', off, len(b))
        off += len(pad4(b))
    for _, b in blobs:
        out += pad4(b)
    return bytes(out)


def decode_dtex(b):
    """DTEX -> (w, h, RGBA8) for previews."""
    _, w, h, f, flags, npal, ntex = struct.unpack_from('<4sHHBBHI', b)
    tex = b[16:16 + ntex]
    pal = struct.unpack_from(f'<{npal}H', b, 16 + ntex + (-ntex % 4))
    out = bytearray(w * h * 4)

    def c(v, a=255):
        return bytes(((v & 31) << 3, ((v >> 5) & 31) << 3, ((v >> 10) & 31) << 3, a))
    for i in range(w * h):
        if f == DIRECT:
            v = tex[i * 2] | tex[i * 2 + 1] << 8
            px = c(v, 255 if v & 0x8000 else 0)
        elif f in (A3I5, A5I3):
            v = tex[i]
            ib = 5 if f == A3I5 else 3
            ab = 8 - ib
            px = c(pal[v & ((1 << ib) - 1)], ((v >> ib) * 255) // ((1 << ab) - 1))
        else:
            bpp = BPP[f]
            v = (tex[i * bpp // 8] >> ((i * bpp) % 8)) & ((1 << bpp) - 1)
            px = c(pal[v], 0 if v == 0 and flags & FLAG_COLOR0_TRANSPARENT else 255)
        out[i * 4:i * 4 + 4] = px
    return w, h, out
