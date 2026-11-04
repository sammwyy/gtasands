"""DFF clumps: frames, geometries (PC, non-native), materials, skin, atomics."""
import struct

from . import rw


class Frame:
    __slots__ = ('name', 'rot', 'pos', 'parent', 'bone_id')


class Material:
    __slots__ = ('color', 'texture')


class Skin:
    __slots__ = ('num_bones', 'indices', 'weights', 'inv_matrices')


class Geometry:
    __slots__ = ('flags', 'verts', 'normals', 'colors', 'uvs', 'tris', 'materials', 'skin', 'sphere')


class Model:
    __slots__ = ('frames', 'geometries', 'atomics', 'hanim')


def _finite(vs):
    """Some stock models contain NaN/inf vertices; zero them."""
    return [tuple(c if c == c and abs(c) != float('inf') else 0.0 for c in v) for v in vs]


def parse(data):
    clump = rw.find(data, 0, len(data), rw.CLUMP)
    if not clump:
        raise ValueError('no clump')
    m = Model()
    m.frames, m.geometries, m.atomics, m.hanim = [], [], [], []
    for t, s, e, _ in rw.chunks(data, *clump):
        if t == rw.FRAMELIST:
            m.frames, m.hanim = _frames(data, s, e)
        elif t == rw.GEOMETRYLIST:
            m.geometries = [_geometry(data, gs, ge) for gs, ge in rw.find_all(data, s, e, rw.GEOMETRY)]
        elif t == rw.ATOMIC:
            o, _ = rw.find(data, s, e, rw.STRUCT)
            m.atomics.append(struct.unpack_from('<II', data, o))
    return m


def _frames(data, s, e):
    o, _ = rw.find(data, s, e, rw.STRUCT)
    n, = struct.unpack_from('<I', data, o)
    frames = []
    hanim = []
    for i in range(n):
        v = struct.unpack_from('<12fiI', data, o + 4 + i * 56)
        f = Frame()
        f.rot, f.pos, f.parent, f.name, f.bone_id = v[:9], v[9:12], v[12], '', -1
        frames.append(f)
    for f, (xs, xe) in zip(frames, rw.find_all(data, s, e, rw.EXTENSION)):
        for t, cs, ce, _ in rw.chunks(data, xs, xe):
            if t == rw.FRAME_NAME:
                f.name = rw.string(data, cs, ce)
            elif t == rw.HANIM:
                f.bone_id = struct.unpack_from('<Ii', data, cs)[1]
                nodes = _hanim_nodes(data, cs, ce)
                if nodes:
                    hanim.extend(nodes)
    return frames, hanim


def _hanim_nodes(data, s, e):
    """The one frame carrying the hierarchy list. rw.hanim header: version?/id/
    numNodes/flags/maxKeySize then nodes {nodeID, frameIndex, flags}."""
    version, _id, n = struct.unpack_from('<iIi', data, s)
    if n <= 0 or s + 20 + n * 12 > e:
        return None
    return [struct.unpack_from('<iIi', data, s + 20 + i * 12) for i in range(n)]


def _geometry(data, s, e):
    o, _ = rw.find(data, s, e, rw.STRUCT)
    g = Geometry()
    flags, ntris, nverts, nmorph = struct.unpack_from('<IIII', data, o)
    o += 16
    g.flags = flags
    ntex = (flags >> 16) & 0xFF or (2 if flags & 0x80 else 1 if flags & 0x04 else 0)
    g.colors = g.uvs = g.normals = g.skin = None
    g.tris = []
    if not flags & 0x01000000:
        if flags & 0x08:
            g.colors = data[o:o + nverts * 4]
            o += nverts * 4
        for i in range(ntex):
            if i == 0:
                uv = struct.unpack_from(f'<{nverts * 2}f', data, o)
                g.uvs = _finite(zip(uv[0::2], uv[1::2]))
            o += nverts * 8
        t = struct.unpack_from(f'<{ntris * 4}H', data, o)
        g.tris = [(t[i + 1], t[i], t[i + 3], t[i + 2]) for i in range(0, len(t), 4)]
        o += ntris * 8
    g.sphere = (0.0, 0.0, 0.0, 0.0)
    g.verts = []
    if nmorph:
        g.sphere = _finite([struct.unpack_from('<4f', data, o)])[0]
        has_v, has_n = struct.unpack_from('<II', data, o + 16)
        o += 24
        if has_v:
            v = struct.unpack_from(f'<{nverts * 3}f', data, o)
            g.verts = _finite(zip(v[0::3], v[1::3], v[2::3]))
            o += nverts * 12
        if has_n:
            v = struct.unpack_from(f'<{nverts * 3}f', data, o)
            g.normals = _finite(zip(v[0::3], v[1::3], v[2::3]))
    g.materials = []
    ml = rw.find(data, s, e, rw.MATLIST)
    if ml:
        so, _ = rw.find(data, *ml, rw.STRUCT)
        n, = struct.unpack_from('<I', data, so)
        refs = struct.unpack_from(f'<{n}i', data, so + 4)
        mats = [_material(data, ms, me) for ms, me in rw.find_all(data, *ml, rw.MATERIAL)]
        it = iter(mats)
        for r in refs:
            g.materials.append(next(it) if r < 0 else g.materials[r])
    ext = rw.find(data, s, e, rw.EXTENSION)
    if ext:
        sk = rw.find(data, *ext, rw.SKIN)
        if sk:
            g.skin = _skin(data, sk[0], nverts)
    return g


def _material(data, s, e):
    o, _ = rw.find(data, s, e, rw.STRUCT)
    m = Material()
    m.color = tuple(data[o + 4:o + 8])
    m.texture = ''
    tex = rw.find(data, s, e, rw.TEXTURE)
    if tex:
        strs = rw.find_all(data, *tex, rw.STRING)
        if strs:
            m.texture = rw.string(data, *strs[0])
    return m


def _skin(data, o, nverts):
    sk = Skin()
    nb, nused, _maxw = data[o], data[o + 1], data[o + 2]
    o += 4 + nused
    sk.num_bones = nb
    sk.indices = data[o:o + nverts * 4]
    o += nverts * 4
    sk.weights = struct.unpack_from(f'<{nverts * 4}f', data, o)
    o += nverts * 16
    sk.inv_matrices = [struct.unpack_from('<16f', data, o + i * 64) for i in range(nb)]
    return sk
