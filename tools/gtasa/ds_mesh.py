"""DFF -> DMDL (quantised DS meshes) with vertex-clustering decimation."""
import math
import struct
from fnmatch import fnmatchcase

from . import dff
from .dsfmt import fx, fx16, name, pad4, rgb15

VF_UV, VF_NORMAL, VF_COLOR, VF_BONE = 1, 2, 4, 8
MF_SKINNED = 1

DEFAULTS = dict(ratio=1.0, min_tris=64, max_tris=0, decimate='cluster', min_area=0.0, max_shift=0.0, ground_qem=False,
                uvs=True, normals=False, vertex_colors=True, skin=True, drop_damage=False, drop_vlo=True,
                drop_frames=[], rigid_skin=False)


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


def _area(tris, P):
    s = 0.0
    for t in tris:
        a, b, c = P[t[0]], P[t[1]], P[t[2]]
        u = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
        w = (c[0] - a[0], c[1] - a[1], c[2] - a[2])
        n = (u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0])
        s += (n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) ** 0.5
    return s * 0.5


def is_flat(verts, tris):
    """Mostly horizontal (by area): ground / road / pavement / bridge deck pieces."""
    up = total = 0.0
    for t in tris:
        a, b, c = verts[t[0]], verts[t[1]], verts[t[2]]
        u = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
        w = (c[0] - a[0], c[1] - a[1], c[2] - a[2])
        nz = u[0] * w[1] - u[1] * w[0]
        ln = ((u[1] * w[2] - u[2] * w[1]) ** 2 + (u[2] * w[0] - u[0] * w[2]) ** 2 + nz * nz) ** 0.5
        total += ln
        if ln and abs(nz) / ln > 0.7:
            up += ln
    return total > 0 and up >= 0.6 * total


def decimate(verts, tris, target, min_area=0.0, max_shift=0.0, max_output=0):
    """Vertex clustering on a uniform grid; binary-searches the grid resolution so the
    surviving triangle count is the largest <= target. With min_area, the target grows
    (x1.25) until that fraction of the surface area survives (small details vanish
    first otherwise). Returns (vertex->rep position, tris)."""
    if (min_area > 0 or max_shift > 0) and target < len(tris) and tris:
        full = _area(tris, verts) if min_area > 0 else 0
        fallback = None
        while True:
            rep, out = decimate(verts, tris, target)
            area_ok = min_area <= 0 or rep is None or _area(out, rep) >= min_area * full
            if area_ok and max_output and len(out) > max_output and fallback:
                return fallback
            if area_ok:
                fallback = rep, out
            shift_ok = max_shift <= 0 or rep is None or all(
                sum((verts[i][k] - rep[i][k]) ** 2 for k in range(3)) <= max_shift * max_shift
                for i in {v for t in out for v in t[:3]})
            if area_ok and (shift_ok or max_output and len(out) > max_output):
                return rep, out
            target = int(target * 1.25) + 1
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


def decimate_qem(verts, tris, target, bone=None):
    """Garland-Heckbert quadric decimation with half-edge collapses (u -> v, v keeps
    its exact position/uv/bone, so no attribute is ever interpolated). Vertices on
    an open edge of the index topology are never removed: that keeps part seams
    (CJ's clothes meet the head/hands/legs there) and UV seams bit-exact. With
    `bone`, only same-bone collapses are allowed so joints still bend cleanly
    under dominant-bone skinning. Returns (None, tris) like decimate()."""
    import heapq
    if target >= len(tris) or not tris:
        return None, tris
    tris = [list(t) for t in tris]
    nv = len(verts)
    vt = [set() for _ in range(nv)]
    edges = {}
    for ti, t in enumerate(tris):
        for k in range(3):
            vt[t[k]].add(ti)
            e = (min(t[k], t[(k + 1) % 3]), max(t[k], t[(k + 1) % 3]))
            edges[e] = edges.get(e, 0) + 1
    locked = [False] * nv
    for (a, b), n in edges.items():
        if n != 2:
            locked[a] = locked[b] = True

    def sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
    def cross(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
    def dot(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]

    def tri_n(t, ov=-1, nvx=-1):
        p = [verts[nvx if i == ov else i] for i in t[:3]]
        return cross(sub(p[1], p[0]), sub(p[2], p[0]))

    Q = [[0.0] * 10 for _ in range(nv)]
    for t in tris:
        n = tri_n(t)
        ln = dot(n, n) ** 0.5
        if ln < 1e-12:
            continue
        a, b, c = n[0] / ln, n[1] / ln, n[2] / ln
        d = -dot((a, b, c), verts[t[0]])
        w = ln * 0.5
        q = (a * a, a * b, a * c, a * d, b * b, b * c, b * d, c * c, c * d, d * d)
        for i in t[:3]:
            Qi = Q[i]
            for k in range(10):
                Qi[k] += q[k] * w

    def cost(u, v):
        q = [Q[u][k] + Q[v][k] for k in range(10)]
        x, y, z = verts[v]
        return (q[0] * x * x + 2 * q[1] * x * y + 2 * q[2] * x * z + 2 * q[3] * x + q[4] * y * y
                + 2 * q[5] * y * z + 2 * q[6] * y + q[7] * z * z + 2 * q[8] * z + q[9])

    def nbrs(u):
        return {i for ti in vt[u] for i in tris[ti][:3]} - {u}

    stamp = [0] * nv
    heap = []

    def push(u):
        if locked[u]:
            return
        for v in nbrs(u):
            if bone is None or bone[u] == bone[v]:
                heapq.heappush(heap, (cost(u, v), u, v, stamp[u], stamp[v]))

    for u in range(nv):
        push(u)
    alive = len(tris)
    while alive > target and heap:
        _c, u, v, su, sv = heapq.heappop(heap)
        if su != stamp[u] or sv != stamp[v] or not vt[u]:
            continue
        shared = vt[u] & vt[v]
        if not shared:
            continue
        # link condition: u and v may only share the apexes of their shared tris
        if len(nbrs(u) & nbrs(v)) != len(shared):
            continue
        ok = True
        for ti in vt[u] - shared:
            n0, n1 = tri_n(tris[ti]), tri_n(tris[ti], u, v)
            l1 = dot(n1, n1)
            if l1 < 1e-14 or dot(n0, n1) <= 0.3 * (dot(n0, n0) * l1) ** 0.5:
                ok = False
                break
        if not ok:
            continue
        for ti in shared:
            for i in tris[ti][:3]:
                vt[i].discard(ti)
            alive -= 1
        for ti in vt[u]:
            t = tris[ti]
            t[t.index(u)] = v
            vt[v].add(ti)
        vt[u] = set()
        for k in range(10):
            Q[v][k] += Q[u][k]
        touched = nbrs(v) | {v}
        for i in touched:
            stamp[i] += 1
        for i in touched:
            push(i)
    live = set()
    for i in range(nv):
        live |= vt[i]
    return None, [tuple(tris[ti]) for ti in sorted(live)]


def decimate_wedge(verts, tris, target):
    """Quadric half-edge collapses on the position-welded mesh (map ground).

    Vertices that share a position are "wedges" of one position vertex (UV /
    prelit / material seams). The topology is the welded one, so seams can
    collapse too, but only when every wedge of u has a matching wedge of v
    across a shared triangle (u1->v1, u2->v2 along a seam): attributes are
    never interpolated or dragged across a seam. Open edges of the welded mesh
    (the borders between adjacent map pieces) are locked and folds are
    rejected, so ground stays hole- and crack-free (vertex clustering is not:
    its folded triangles kept the area but left the sky showing through).
    Returns (None, tris) like decimate()."""
    import heapq
    if target >= len(tris) or not tris:
        return None, tris
    pid, P, wpos = {}, [], []
    for v in verts:
        k = (round(v[0], 3), round(v[1], 3), round(v[2], 3))
        if k not in pid:
            pid[k] = len(P)
            P.append(v)
        wpos.append(pid[k])
    tris = [list(t) for t in tris if len({wpos[t[0]], wpos[t[1]], wpos[t[2]]}) == 3]
    n = len(P)
    vt = [set() for _ in range(n)]
    edges = {}
    for ti, t in enumerate(tris):
        for k in range(3):
            a, b = wpos[t[k]], wpos[t[(k + 1) % 3]]
            vt[a].add(ti)
            e = (min(a, b), max(a, b))
            edges[e] = edges.get(e, 0) + 1
    locked = [False] * n
    for (a, b), c in edges.items():
        if c != 2:
            locked[a] = locked[b] = True

    def sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
    def cross(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
    def dot(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]

    def tri_n(t, u=-1, v=-1):
        p = [P[v if wpos[i] == u else wpos[i]] for i in t[:3]]
        return cross(sub(p[1], p[0]), sub(p[2], p[0]))

    Q = [[0.0] * 10 for _ in range(n)]
    for t in tris:
        nn = tri_n(t)
        ln = dot(nn, nn) ** 0.5
        if ln < 1e-12:
            continue
        a, b, c = nn[0] / ln, nn[1] / ln, nn[2] / ln
        d = -dot((a, b, c), P[wpos[t[0]]])
        w = ln * 0.5
        q = (a * a, a * b, a * c, a * d, b * b, b * c, b * d, c * c, c * d, d * d)
        for i in t[:3]:
            Qi = Q[wpos[i]]
            for k in range(10):
                Qi[k] += q[k] * w

    def cost(u, v):
        q = [Q[u][k] + Q[v][k] for k in range(10)]
        x, y, z = P[v]
        return (q[0] * x * x + 2 * q[1] * x * y + 2 * q[2] * x * z + 2 * q[3] * x + q[4] * y * y
                + 2 * q[5] * y * z + 2 * q[6] * y + q[7] * z * z + 2 * q[8] * z + q[9])

    def nbrs(u):
        return {wpos[i] for ti in vt[u] for i in tris[ti][:3]} - {u}

    stamp = [0] * n
    heap = []

    def push(u):
        if locked[u]:
            return
        for v in nbrs(u):
            heapq.heappush(heap, (cost(u, v), u, v, stamp[u], stamp[v]))

    for u in range(n):
        push(u)
    alive = len(tris)
    while alive > target and heap:
        _c, u, v, su, sv = heapq.heappop(heap)
        if su != stamp[u] or sv != stamp[v] or not vt[u]:
            continue
        shared = vt[u] & vt[v]
        if not shared or len(nbrs(u) & nbrs(v)) != len(shared):
            continue
        # wedge map from the shared triangles; every wedge of u must be covered
        wmap, ok = {}, True
        for ti in shared:
            t = tris[ti]
            wu = next(i for i in t[:3] if wpos[i] == u)
            wv = next(i for i in t[:3] if wpos[i] == v)
            if wmap.setdefault(wu, wv) != wv:
                ok = False
                break
        if ok:
            for ti in vt[u] - shared:
                t = tris[ti]
                if next(i for i in t[:3] if wpos[i] == u) not in wmap:
                    ok = False
                    break
                n0, n1 = tri_n(t), tri_n(t, u, v)
                l1 = dot(n1, n1)
                if l1 < 1e-14 or dot(n0, n1) <= 0.3 * (dot(n0, n0) * l1) ** 0.5:
                    ok = False
                    break
        if not ok:
            continue
        for ti in shared:
            for i in tris[ti][:3]:
                vt[wpos[i]].discard(ti)
            alive -= 1
        for ti in vt[u]:
            t = tris[ti]
            for k in range(3):
                if wpos[t[k]] == u:
                    t[k] = wmap[t[k]]
            vt[v].add(ti)
        vt[u] = set()
        for k in range(10):
            Q[v][k] += Q[u][k]
        touched = nbrs(v) | {v}
        for i in touched:
            stamp[i] += 1
        for i in touched:
            push(i)
    live = set()
    for i in range(n):
        live |= vt[i]
    return None, [tuple(tris[ti]) for ti in sorted(live)]


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
    use_uv = bool(p['uvs'] and g.uvs)
    use_n = bool(p['normals'] and g.normals)
    use_c = bool(p['vertex_colors'] and g.colors)
    use_b = bool(p['skin'] and g.skin)
    bone = None
    if use_b:
        sk = g.skin
        bone = [sk.indices[i * 4 + max(range(4), key=lambda k: sk.weights[i * 4 + k])] for i in range(nv)]
    if p['decimate'] == 'qem':
        rep, tris = decimate_qem(g.verts, tris, target, bone)
    elif p['decimate'] == 'wedge' or (p['ground_qem'] and is_flat(g.verts, tris)):
        # ground pieces: a hole in the ground shows the sky, a lost wall detail
        # does not matter much -> crack/hole-free collapses there
        rep, tris = decimate_wedge(g.verts, tris, target)
    else:
        rep, tris = decimate(g.verts, tris, target, p['min_area'], p['max_shift'], p['max_tris'])
    pos = rep or g.verts

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


def _synth_rigid_skin(m, atoms):
    if not m.hanim or len(m.hanim) > 64 or len({g for _, g in atoms}) != len(atoms):
        raise ValueError('rigid cutscene model needs a unique geometry per atomic and an HAnim hierarchy')
    world = []
    for f in m.frames:
        local = list(f.rot) + list(f.pos)
        world.append(_mm(local, world[f.parent]) if f.parent >= 0 else local)
    inv = [_invm(world[frame]) for _, frame, _ in m.hanim]
    bones = {frame: i for i, (_, frame, _) in enumerate(m.hanim)}
    for frame, gi in atoms:
        g = m.geometries[gi]
        if g.skin:
            raise ValueError('rigid cutscene model unexpectedly has skin')
        node = frame
        while node not in bones and node >= 0:
            node = m.frames[node].parent
        if node < 0:
            raise ValueError('rigid atomic has no HAnim parent')
        sk = dff.Skin()
        sk.num_bones = len(m.hanim)
        sk.inv_matrices = [(a[0], a[1], a[2], 0, a[3], a[4], a[5], 0,
                            a[6], a[7], a[8], 0, a[9], a[10], a[11], 1) for a in inv]
        sk.indices = bytes((bones[node], 0, 0, 0)) * len(g.verts)
        sk.weights = [1.0, 0.0, 0.0, 0.0] * len(g.verts)
        g.verts = [tuple(_xf(world[frame], v)) for v in g.verts]
        g.skin = sk


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
    if p.get('rigid_skin'):
        _synth_rigid_skin(m, atoms)
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
