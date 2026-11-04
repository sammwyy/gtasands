"""DFF -> DMDL (quantised DS meshes) with vertex-clustering decimation."""
import math
import struct
from fnmatch import fnmatchcase

from . import dff
from .dsfmt import fx, fx16, name, pad4, rgb15

VF_UV, VF_NORMAL, VF_COLOR, VF_BONE = 1, 2, 4, 8
MF_SKINNED = 1

DEFAULTS = dict(ratio=1.0, min_tris=64, max_tris=0, uvs=True, normals=False, vertex_colors=True,
                skin=True, drop_damage=False, drop_vlo=True, drop_frames=[])


# --- RW skinning (validated against gta-reversed / librw semantics) ----------
# Points are row vectors: p' = p * M. Matrix product is left-to-right:
# LTM[node] = local * LTM[parent], skin matrix = invBone * LTM.
def _mm(a, b):
    """Row-vector affine product a*b: R=Ra.Rb, t=ta.Rb+tb."""
    out = [0.0] * 12
    for r in range(3):
        for c in range(3):
            out[r * 3 + c] = sum(a[r * 3 + k] * b[k * 3 + c] for k in range(3))
    for r in range(3):
        out[9 + r] = sum(a[9 + c] * b[c * 3 + r] for c in range(3)) + b[9 + r]
    return out


def _invm(m):
    """Row-vector affine inverse (m stores rows right/up/at/pos, 12 floats)."""
    a, b_, c = m[0], m[1], m[2]
    d, e, f = m[3], m[4], m[5]
    g, h, i2 = m[6], m[7], m[8]
    det = a * (e * i2 - f * h) - b_ * (d * i2 - f * g) + c * (d * h - e * g)
    r = [[(e * i2 - f * h) / det, (c * h - b_ * i2) / det, (b_ * f - c * e) / det],
         [(f * g - d * i2) / det, (a * i2 - c * g) / det, (c * d - a * f) / det],
         [(d * h - e * g) / det, (b_ * g - a * h) / det, (a * e - b_ * d) / det]]
    t = [-sum(m[9 + c] * r[c][j] for c in range(3)) for j in range(3)]
    return [r[0][0], r[0][1], r[0][2], r[1][0], r[1][1], r[1][2],
            r[2][0], r[2][1], r[2][2]] + t


def _xf(m, v):
    """Row-vector point transform: v * M."""
    return [sum(v[c] * m[c * 3 + r] for c in range(3)) + m[9 + r] for r in range(3)]


def _ib4(mtx):
    """16-float RW matrix -> 12-float row layout (drop the pad column)."""
    return [mtx[0], mtx[1], mtx[2], mtx[4], mtx[5], mtx[6],
            mtx[8], mtx[9], mtx[10], mtx[12], mtx[13], mtx[14]]


def _hanim_table(m, num_bones):
    """Node table in hAnim order: (bone_id, parent_node). The node list lives
    in the hAnim chunk; children delimit via push(2)/pop(1) flags."""
    parents, stack, parent = [], [], -1
    for _nid, _fidx, flags in m.hanim:
        parents.append(parent)
        if flags & 2:
            stack.append(parent)
        parent = len(parents) - 1
        if flags & 1:
            parent = stack.pop() if stack else -1
    table = [(nid, parents[i]) for i, (nid, _f, _fl) in enumerate(m.hanim)]
    if len(table) < num_bones:  # pad degenerate tail (never hit for SA peds)
        table += [(table[-1][0] + 1, len(table) - 1)] * (num_bones - len(table))
    return table[:num_bones]


def decimate(verts, tris, target):
    """Vertex clustering on a uniform grid; binary-searches the grid resolution so the
    surviving triangle count is the largest <= target. Returns (vertex->rep position, tris)."""
    if target >= len(tris) or not tris:
        return None, tris
    xs, ys, zs = zip(*verts)
    lo = (min(xs), min(ys), min(zs))
    ext = max(max(xs) - lo[0], max(ys) - lo[1], max(zs) - lo[2]) or 1.0

    def run(res):
        cell = ext / res
        key = [(int((x - lo[0]) / cell), int((y - lo[1]) / cell), int((z - lo[2]) / cell)) for x, y, z in verts]
        seen, out = set(), []
        for t in tris:
            a, b, c = key[t[0]], key[t[1]], key[t[2]]
            if a == b or b == c or a == c:
                continue
            k = (min(a, b, c), max(a, b, c), a, b, c, t[3])
            if k in seen:
                continue
            seen.add(k)
            out.append(t)
        return key, out

    lo_r, hi_r, best = 1, 2048, None
    while lo_r <= hi_r:
        mid = (lo_r + hi_r) // 2
        key, out = run(mid)
        if len(out) <= target:
            best = (key, out)
            lo_r = mid + 1
        else:
            hi_r = mid - 1
    if best is None:
        best = run(1)
    key, out = best
    acc = {}
    for k, v in zip(key, verts):
        s = acc.setdefault(k, [0.0, 0.0, 0.0, 0])
        s[0] += v[0]; s[1] += v[1]; s[2] += v[2]; s[3] += 1
    rep = [tuple(acc[k][i] / acc[k][3] for i in range(3)) for k in key]
    return rep, out


def _exp_for(maxabs):
    """Smallest e so that maxabs*4096/2^e fits in s16."""
    e = math.ceil(math.log2(max(maxabs, 1e-6) * 4096 / 32767)) if maxabs > 0 else -4
    return max(-4, min(12, e))


def convert_geometry(g, p):
    nv = len(g.verts)
    tris = [t for t in g.tris if max(t[:3]) < nv]
    target = max(p['min_tris'], int(len(tris) * p['ratio']))
    if p['max_tris']:
        target = min(target, p['max_tris'])
    rep, tris = decimate(g.verts, tris, target)
    pos = rep or g.verts
    use_uv = bool(p['uvs'] and g.uvs)
    use_n = bool(p['normals'] and g.normals)
    use_c = bool(p['vertex_colors'] and g.colors)
    use_b = bool(p['skin'] and g.skin)
    bone = None
    if use_b:
        sk = g.skin
        bone = [sk.indices[i * 4 + max(range(4), key=lambda k: sk.weights[i * 4 + k])] for i in range(nv)]

    def attrs(i):
        return (pos[i],
                g.uvs[i] if use_uv else None,
                g.colors[i * 4:i * 4 + 4] if use_c else None,
                bone[i] if use_b else None,
                g.normals[i] if use_n else None)

    remap, verts = {}, []
    for t in tris:
        for i in t[:3]:
            if i not in remap:
                a = attrs(i)
                j = remap.get(a)
                if j is None:
                    j = len(verts)
                    verts.append((i, a))
                    remap[a] = j
                remap[i] = j
    tris = sorted(tris, key=lambda t: t[3])
    mats, idx = [], []
    for mi, m in enumerate(g.materials or [None]):
        first = len(idx)
        for t in tris:
            if t[3] == mi or (m is None):
                idx += [remap[t[0]], remap[t[1]], remap[t[2]]]
        if len(idx) > first:
            col = m.color if m else (255, 255, 255, 255)
            mats.append((m.texture if m else '', rgb15(*col[:3]), col[3], first, len(idx) - first))

    P = [a[0] for _, a in verts] or [(0.0, 0.0, 0.0)]
    lo = [min(v[k] for v in P) for k in range(3)]
    hi = [max(v[k] for v in P) for k in range(3)]
    ctr = [(lo[k] + hi[k]) / 2 for k in range(3)]
    pexp = _exp_for(max(max(abs(v[k] - ctr[k]) for k in range(3)) for v in P))
    ps = 4096 / 2 ** pexp
    uexp = 0
    if use_uv:
        uexp = max(0, _exp_for(max((max(abs(a[1][0]), abs(a[1][1])) for _, a in verts), default=0)))
    us = 4096 / 2 ** uexp
    vf = (VF_UV if use_uv else 0) | (VF_NORMAL if use_n else 0) | (VF_COLOR if use_c else 0) | (VF_BONE if use_b else 0)

    out = bytearray(struct.pack('<HHIBbbB3i', len(verts), len(mats), len(idx), vf, pexp, uexp, 0, *(fx(c) for c in ctr)))
    for tex, c15, alpha, first, cnt in mats:
        out += name(tex, 32) + struct.pack('<HBBII', c15, alpha, 0, first, cnt)
    out += pad4(b''.join(struct.pack('<3h', *(max(-32768, min(32767, round((a[0][k] - ctr[k]) * ps))) for k in range(3))) for _, a in verts))
    if use_uv:
        out += b''.join(struct.pack('<2h', *(max(-32768, min(32767, round(u * us))) for u in a[1])) for _, a in verts)
    if use_n:
        def pk(n):
            q = [max(-512, min(511, int(c * 512))) & 0x3FF for c in n]
            return q[0] | q[1] << 10 | q[2] << 20
        out += b''.join(struct.pack('<I', pk(a[4])) for _, a in verts)
    if use_c:
        out += pad4(b''.join(struct.pack('<H', rgb15(*a[2][:3])) for _, a in verts))
    if use_b:
        out += pad4(bytes(a[3] for _, a in verts))
    out += pad4(struct.pack(f'<{len(idx)}H', *idx))
    return bytes(out), len(g.tris), len(idx) // 3


def convert_dff(data, p):
    m = dff.parse(data)
    drops = list(p['drop_frames']) + (['*_dam'] if p['drop_damage'] else []) + (['*_vlo'] if p['drop_vlo'] else [])

    def dropped(fi):
        while 0 <= fi < len(m.frames):
            if any(fnmatchcase(m.frames[fi].name.lower(), d.lower()) for d in drops):
                return True
            fi = m.frames[fi].parent
        return False

    atoms = [(f, g) for f, g in m.atomics if g < len(m.geometries) and not dropped(f)]
    used = sorted({g for _, g in atoms})
    gmap = {g: i for i, g in enumerate(used)}
    geoms, tin, tout = [], 0, 0
    for gi in used:
        b, a, o = convert_geometry(m.geometries[gi], p)
        geoms.append(b)
        tin += a
        tout += o
    skin = next((m.geometries[g].skin for g in used if m.geometries[g].skin), None) if p['skin'] else None
    sph = max((m.geometries[g].sphere for g in used), key=lambda s: s[3], default=(0, 0, 0, 0))

    out = bytearray(struct.pack('<4sHHHHHH4i', b'DMDL', 2, MF_SKINNED if skin else 0, len(m.frames), len(geoms),
                                len(atoms), skin.num_bones if skin else 0, *(fx(v) for v in sph)))
    for f in m.frames:
        out += name(f.name, 24) + struct.pack('<hh9hH3i', f.parent, f.bone_id, *(fx16(r) for r in f.rot), 0, *(fx(v) for v in f.pos))
    for f, g in atoms:
        out += struct.pack('<HH', f, gmap[g])
    if skin:
        ib = [_ib4(mtx) for mtx in skin.inv_matrices]
        for mtx in ib:
            out += struct.pack('<12i', *(fx(v) for v in mtx))
        # hAnim node order (verts' skin index == node index): parent chain +
        # bone id for seq matching, plus the two derived locals the runtime
        # needs so it never inverts matrices on the ARM9.
        nodes = _hanim_table(m, skin.num_bones)
        for nid, parent in nodes:
            out += struct.pack('<hh', parent, nid)
        bindw = [_invm(i) for i in ib]
        for i, (_nid, parent) in enumerate(nodes):
            loc = bindw[i] if parent < 0 else _mm(bindw[i], ib[parent])
            out += struct.pack('<12i', *(fx(v) for v in loc))
        for i, (_nid, parent) in enumerate(nodes):
            bp = (0.0, 0.0, 0.0) if parent < 0 else _xf(ib[parent], bindw[i][9:12])
            out += struct.pack('<3i', *(fx(v) for v in bp))
    base = len(out) + 4 * len(geoms)
    offs = []
    for b in geoms:
        offs.append(base)
        base += len(b)
    out += struct.pack(f'<{len(geoms)}I', *offs)
    for b in geoms:
        out += b
    return bytes(out), {'tris_in': tin, 'tris_out': tout}
