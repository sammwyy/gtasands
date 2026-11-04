"""COL collision archives (COLL / COL2 / COL3 / COL4 entries)."""
import struct


class ColModel:
    __slots__ = ('version', 'name', 'model_id', 'bmin', 'bmax', 'center', 'radius', 'spheres', 'boxes', 'verts', 'faces')


def parse(data):
    out = []
    o = 0
    while o + 32 <= len(data):
        four, size = struct.unpack_from('<4sI', data, o)
        if four[:3] != b'COL':
            nxt = data.find(b'COL', max(0, o - 8), o + 16)  # some stock files have off-by-N entry sizes
            if nxt < 0 or nxt == o:
                break
            o = nxt
            continue
        out.append(_entry(data, o, four))
        o += 8 + size
    return out


def _entry(d, base, four):
    c = ColModel()
    c.version = 1 if four == b'COLL' else int(four[3:])
    c.name = d[base + 8:base + 30].split(b'\0', 1)[0].decode('latin1')
    c.model_id, = struct.unpack_from('<H', d, base + 30)
    o = base + 32
    c.spheres, c.boxes, c.verts, c.faces = [], [], [], []
    if c.version == 1:
        end = base + 8 + struct.unpack_from('<I', d, base + 4)[0]

        def count(o, sz):  # stock COLL files contain garbage counts; ignore ones that overflow the entry
            n, = struct.unpack_from('<I', d, o)
            return (n if o + 4 + n * sz <= end else 0), o + 4
        c.radius, *rest = struct.unpack_from('<10f', d, o)
        c.center, c.bmin, c.bmax = tuple(rest[0:3]), tuple(rest[3:6]), tuple(rest[6:9])
        o += 40
        n, o = count(o, 20)
        for i in range(n):
            r, x, y, z = struct.unpack_from('<4f', d, o)
            c.spheres.append(((x, y, z), r, tuple(d[o + 16:o + 20]))); o += 20
        n, o = count(o, 24)
        o += n * 24
        n, o = count(o, 28)
        for i in range(n):
            v = struct.unpack_from('<6f', d, o)
            c.boxes.append((v[:3], v[3:], tuple(d[o + 24:o + 28]))); o += 28
        n, o = count(o, 12)
        v = struct.unpack_from(f'<{n * 3}f', d, o)
        c.verts = list(zip(v[0::3], v[1::3], v[2::3])); o += n * 12
        n, o = count(o, 16)
        for i in range(n):
            a, b, cc = struct.unpack_from('<3I', d, o)
            c.faces.append((a, b, cc, d[o + 12], d[o + 15])); o += 16
        return c
    v = struct.unpack_from('<10f', d, o)
    c.bmin, c.bmax, c.center, c.radius = v[0:3], v[3:6], v[6:9], v[9]
    nsph, nbox, nface, _nline, _pad, _flags = struct.unpack_from('<HHHBBI', d, o + 40)
    osph, obox, _oline, overt, oface, _otri = struct.unpack_from('<6I', d, o + 52)
    rel = base + 4
    for i in range(nsph):
        p = rel + osph + i * 20
        x, y, z, r = struct.unpack_from('<4f', d, p)
        c.spheres.append(((x, y, z), r, tuple(d[p + 16:p + 20])))
    for i in range(nbox):
        p = rel + obox + i * 28
        b = struct.unpack_from('<6f', d, p)
        c.boxes.append((b[:3], b[3:], tuple(d[p + 24:p + 28])))
    maxv = -1
    for i in range(nface):
        a, b, cc, mat, light = struct.unpack_from('<3HBB', d, rel + oface + i * 8)
        c.faces.append((a, b, cc, mat, light))
        maxv = max(maxv, a, b, cc)
    if maxv >= 0:
        v = struct.unpack_from(f'<{(maxv + 1) * 3}h', d, rel + overt)
        c.verts = [(v[i] / 128, v[i + 1] / 128, v[i + 2] / 128) for i in range(0, len(v), 3)]
    return c
