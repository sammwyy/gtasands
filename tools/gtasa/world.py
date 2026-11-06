"""gta.dat + IDE + IPL (text and binary stream IPLs inside gta3.img) -> DWLD.

The DS never parses map text files: CFileLoader::LoadLevel / LoadObjectTypes /
LoadScene are run here, and the runtime gets a sector grid of pre-transformed
instances (fixed-point position + rotation matrix) plus the model and TXD
tables it needs to stream them from gta3.dspk. Layout: src/assets/ds_formats.h.
"""
import math
import struct
from pathlib import Path

from . import col, dff, img
from .dsfmt import fx, fx16, name as fname, pad4

VERSION = 6
COL_UNIT = 128        # collision coordinates: s16 at 1/128 m (as COL2+ vertices)
COL_LEAF = 6          # faces per BVH leaf
NO_TIME = 24          # timeOn/timeOff for always-visible objects
IDE_SECTIONS = {'objs', 'tobj', 'anim', 'txdp'}
AREA_ANY = 13         # eAreaCodes::AREA_CODE_13: visible in every area


def _lines(path):
    for line in path.read_text(errors='replace').splitlines():
        line = line.split('#', 1)[0].strip()
        if line:
            yield line


def resolve(base, rel):
    """Case-insensitive path lookup (data files use DOS paths)."""
    p = Path(base)
    for part in rel.replace('\\', '/').split('/'):
        if not part:
            continue
        hit = next((c for c in p.iterdir() if c.name.lower() == part.lower()), None) if p.is_dir() else None
        if hit is None:
            return None
        p = hit
    return p


def _f(a):
    return [x.strip() for x in a.split(',')]


def parse_ide(path, objs, parents):
    """CFileLoader::LoadObjectTypes: objs/tobj/anim -> objs[id] = (model, txd, drawDist, flags, on, off)."""
    sec = None
    for line in _lines(path):
        low = line.lower()
        if low in IDE_SECTIONS or low in ('end', 'path', '2dfx', 'weap', 'hier', 'cars', 'peds'):
            sec = None if low == 'end' else low
            continue
        a = _f(line)
        try:
            if sec == 'txdp' and len(a) >= 2:
                parents[a[0].lower()] = a[1].lower()
            elif sec in ('objs', 'tobj') and len(a) >= 5:
                on = off = NO_TIME
                if sec == 'tobj':
                    on, off, a = int(a[-2]), int(a[-1]), a[:-2]
                if len(a) == 5:                       # id, model, txd, drawDist, flags
                    dd, flags = float(a[3]), int(a[4])
                else:                                 # id, model, txd, meshCount, dd[meshCount], flags
                    n = int(a[3])
                    dd, flags = float(a[4]), int(a[4 + n])
                objs[int(a[0])] = (a[1].lower(), a[2].lower(), dd, flags, on, off)
            elif sec == 'anim' and len(a) >= 6:       # id, model, txd, anim, drawDist, flags
                objs[int(a[0])] = (a[1].lower(), a[2].lower(), float(a[4]), int(a[5]), NO_TIME, NO_TIME)
        except ValueError:
            continue


def parse_ipl_text(path):
    """CFileLoader::LoadObjectInstance (text): [(id, x, y, z, qx, qy, qz, qw, area, lod)]."""
    out, sec = [], None
    for line in _lines(path):
        low = line.lower()
        if ',' not in line:
            sec = None if low == 'end' else low
            continue
        if sec != 'inst':
            continue
        a = _f(line)
        if len(a) < 11:
            continue
        try:
            out.append((int(a[0]), *map(float, a[3:10]), int(a[2]), int(a[10])))
        except ValueError:
            continue
    return out


def parse_ipl_binary(data):
    """'bnry' stream IPL (CIplStore::LoadIpl): 40-byte instances."""
    if data[:4] != b'bnry':
        return []
    n, = struct.unpack_from('<I', data, 4)
    off, = struct.unpack_from('<I', data, 28)
    out = []
    for i in range(n):
        x, y, z, qx, qy, qz, qw, mid, area, lod = struct.unpack_from('<7fiii', data, off + i * 40)
        out.append((mid, x, y, z, qx, qy, qz, qw, area, lod))
    return out


def rotation(qx, qy, qz, qw):
    """LoadObjectInstance negates the imaginary part, then CMatrix::SetRotate.
    The heading-only branch of the original is the same rotation. Rows:
    right, forward, up (SA world = x*right + y*forward + z*up + pos)."""
    qx, qy, qz = -qx, -qy, -qz
    n = math.sqrt(qx * qx + qy * qy + qz * qz + qw * qw) or 1.0
    qx, qy, qz, qw = qx / n, qy / n, qz / n, qw / n
    x2, y2, z2 = qx + qx, qy + qy, qz + qz
    xx, yx, zx = x2 * qx, y2 * qx, z2 * qx
    yy, zy, zz = y2 * qy, z2 * qy, z2 * qz
    xr, yr, zr = x2 * qw, y2 * qw, z2 * qw
    return (1 - (zz + yy), zr + yx, zx - yr,
            yx - zr, 1 - (zz + xx), xr + zy,
            yr + zx, zy - xr, 1 - (yy + xx))


def _radius(imgp, off, size):
    """Bounding radius around the model origin (|center| + r over all geometries),
    so the runtime can cull/request big ground meshes before loading them."""
    try:
        m = dff.parse(img.read_entry(imgp, off, size))
    except Exception:  # noqa: BLE001 - a broken dff just gets a generous radius
        return 64
    matrices = [None] * len(m.frames)

    def matrix(i):
        if matrices[i] is not None:
            return matrices[i]
        f = m.frames[i]
        rot, pos = list(f.rot), list(f.pos)
        if 0 <= f.parent < len(m.frames) and f.parent != i:
            parent = matrix(f.parent)
            rot = [sum(f.rot[row * 3 + k] * parent[k * 3 + col] for k in range(3))
                   for row in range(3) for col in range(3)]
            pos = [sum(f.pos[k] * parent[k * 3 + col] for k in range(3)) + parent[9 + col]
                   for col in range(3)]
        matrices[i] = rot + pos
        return matrices[i]

    radii = []
    for frame, geom in m.atomics:
        if frame >= len(m.frames) or geom >= len(m.geometries):
            continue
        s = m.geometries[geom].sphere
        mat = matrix(frame)
        center = [sum(s[k] * mat[k * 3 + col] for k in range(3)) + mat[9 + col] for col in range(3)]
        scale = max(math.sqrt(sum(mat[row * 3 + col] ** 2 for col in range(3))) for row in range(3))
        radii.append(math.sqrt(sum(c * c for c in center)) + s[3] * scale)
    r = max(radii, default=8.0)
    return max(1, min(0xFFFF, math.ceil(r)))


def _col_models(imgp, toc, wanted):
    """CFileLoader::LoadCollisionFile over every .col archive in the img: model name -> ColModel
    (first definition wins, like a model whose col is already set)."""
    out, dups = {}, 0
    for n, (o, s) in toc.items():
        if not n.endswith('.col'):
            continue
        try:
            models = col.parse(img.read_entry(imgp, o, s))
        except Exception:  # noqa: BLE001 - a broken archive just loses its collision
            continue
        for c in models:
            k = c.name.lower()
            if k not in wanted:
                continue
            if k in out:
                dups += 1
                continue
            out[k] = c
    return out, dups


def _q(v):
    return max(-32768, min(32767, round(v * COL_UNIT)))


def _col_record(c):
    """ColModel -> WorldCol record (ds_formats.h). Faces are re-wound so that
    n = (v1 - v0) x (v2 - v0) is the front normal (CColTrianglePlane uses
    (c - a) x (b - a)), degenerate ones dropped, then put in a bbox tree (BVH).
    Returns (bytes, radius in 1/16 m, faces)."""
    verts = [tuple(_q(x) for x in v) for v in c.verts]
    faces = []
    for a, b, cc, mat, light in c.faces:
        if max(a, b, cc) >= len(verts):
            continue
        v0, v1, v2 = verts[a], verts[cc], verts[b]
        e1 = [v1[i] - v0[i] for i in range(3)]
        e2 = [v2[i] - v0[i] for i in range(3)]
        n = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
        ln = math.sqrt(n[0] ** 2 + n[1] ** 2 + n[2] ** 2)
        if ln < 1e-6:
            continue
        faces.append((a, cc, b, mat, light, tuple(max(-4096, min(4096, round(k / ln * 4096))) for k in n)))
    spheres = [(tuple(_q(x) for x in ctr), max(1, min(0xFFFF, round(r * COL_UNIT))), surf) for ctr, r, surf in c.spheres]
    boxes = [(tuple(_q(min(mn[i], mx[i])) for i in range(3)), tuple(_q(max(mn[i], mx[i])) for i in range(3)), surf)
             for mn, mx, surf in c.boxes]

    pts = [verts[i] for f in faces for i in f[:3]] + [b[0] for b in boxes] + [b[1] for b in boxes]
    pts += [tuple(s[0][i] + d * s[1] for i in range(3)) for s in spheres for d in (-1, 1)]
    if not pts:
        return None, 0, 0
    bmin = [min(p[i] for p in pts) for i in range(3)]
    bmax = [max(p[i] for p in pts) for i in range(3)]
    rad = max(math.sqrt(sum(max(abs(bmin[i]), abs(bmax[i])) ** 2 for i in range(3))) / COL_UNIT, 0.5)

    # BVH over the faces (median split on the longest centroid axis, <= COL_LEAF
    # faces per leaf), nodes in depth-first order: faces get reordered so every
    # leaf is one contiguous range.
    fbox = [([min(verts[f[k]][j] for k in range(3)) for j in range(3)],
             [max(verts[f[k]][j] for k in range(3)) for j in range(3)]) for f in faces]
    order, nodes = [], []

    def build_node(ids):
        mn = [min(fbox[i][0][j] for i in ids) for j in range(3)]
        mx = [max(fbox[i][1][j] for i in ids) for j in range(3)]
        me = len(nodes)
        nodes.append(None)
        if len(ids) <= COL_LEAF:
            nodes[me] = (mn, mx, len(order), 0x8000 | len(ids))
            order.extend(ids)
            return me
        cen = {i: [fbox[i][0][j] + fbox[i][1][j] for j in range(3)] for i in ids}
        ext = [max(cen[i][j] for i in ids) - min(cen[i][j] for i in ids) for j in range(3)]
        ax = ext.index(max(ext))
        ids = sorted(ids, key=lambda i: cen[i][ax])
        half = len(ids) // 2
        left = build_node(ids[:half])
        right = build_node(ids[half:])
        nodes[me] = (mn, mx, left, right)
        return me
    if faces:
        build_node(list(range(len(faces))))
    faces = [faces[i] for i in order]
    used = sorted({k for f in faces for k in f[:3]})
    remap = {v: i for i, v in enumerate(used)}

    out = bytearray(struct.pack('<6h6H', *bmin, *bmax, len(spheres), len(boxes), len(nodes), len(used), len(faces), 0))
    for ctr, r, surf in spheres:
        out += struct.pack('<3hH4B', *ctr, r, surf[0], surf[3], 0, 0)
    for mn, mx, surf in boxes:
        out += struct.pack('<6h4B', *mn, *mx, surf[0], surf[3], 0, 0)
    for mn, mx, a, b in nodes:
        out += struct.pack('<6hHH', *mn, *mx, a, b)
    for a, b, cc, mat, light, n in faces:
        out += struct.pack('<3H2B3hH', remap[a], remap[b], remap[cc], mat, light, *n, 0)
    out += pad4(b''.join(struct.pack('<3h', *verts[i]) for i in used))
    return bytes(out), min(0xFFFF, math.ceil(rad * 16)), len(faces)


def build(game, sec):
    """Returns (blob, stats)."""
    game = Path(game)
    dat = resolve(game, sec.get('dat', 'data/gta.dat'))
    imgp = resolve(game, sec.get('img', 'models/gta3.img'))
    intp = resolve(game, sec.get('interior_img', 'models/gta_int.img'))
    if not dat or not imgp or not intp:
        raise FileNotFoundError('need data/gta.dat, models/gta3.img and models/gta_int.img (run: tools/assets.py sync)')
    ides, ipls = [], []
    for line in _lines(dat):
        k, _, v = line.partition(' ')
        k, v = k.upper(), v.strip()
        if k == 'IDE':
            ides.append(v)
        elif k == 'IPL' and v.lower().endswith('.ipl'):
            ipls.append(v)
    objs, parents = {}, {}
    for f in ['data/maps/txd.ide'] + ides:
        p = resolve(game, f)
        if p:
            parse_ide(p, objs, parents)

    toc = {n.lower(): (o, s) for n, o, s in img.read_toc(imgp)}
    int_toc = {n.lower(): (o, s) for n, o, s in img.read_toc(intp)}

    def source(name):
        if name in toc:
            return imgp, toc[name], 0
        if name in int_toc:
            return intp, int_toc[name], 1
        return None, None, 0
    bounds = sec.get('bounds')       # [minX, minY, maxX, maxY] or None = everything
    keep_lod = sec.get('lods', False)
    insts = []
    skipped = {'area': 0, 'lod': 0, 'model': 0, 'bounds': 0}
    for f in ipls:
        p = resolve(game, f)
        if not p:
            continue
        base = p.stem.lower()
        text = parse_ipl_text(p)
        stream, k = [], 0
        while source(f'{base}_stream{k}.ipl')[0] is not None:
            path, (o, s), _ = source(f'{base}_stream{k}.ipl')
            stream += parse_ipl_binary(img.read_entry(path, o, s))
            k += 1
        # lod indices of both the text and the stream IPLs point into the text list
        entries = text + stream
        lod_of = {t[9] for t in entries if 0 <= t[9] < len(text)}
        hd_of = {t[9]: t[0] for t in entries if 0 <= t[9] < len(text)}
        for i, t in enumerate(entries):
            mid, x, y, z, qx, qy, qz, qw, area, lod = t
            o = objs.get(mid)
            if o is None or source(o[0] + '.dff')[0] is None:
                skipped['model'] += 1
                continue
            is_lod = (i < len(text) and i in lod_of) or o[0].startswith('lod')
            if is_lod and not keep_lod:
                skipped['lod'] += 1
                continue
            if bounds and not (bounds[0] <= x <= bounds[2] and bounds[1] <= y <= bounds[3]):
                skipped['bounds'] += 1
                continue
            linked = hd_of.get(i) if is_lod and i < len(text) else text[lod][0] if not is_lod and 0 <= lod < len(text) else None
            insts.append((mid, x, y, z, rotation(qx, qy, qz, qw), is_lod, linked, area & 0xFF))

    # tables: only what the instances reference
    mids = sorted({t[0] for t in insts})
    midx = {m: i for i, m in enumerate(mids)}
    txds = []
    tidx = {}

    def txd_index(n):
        if n in tidx:
            return tidx[n]
        tidx[n] = len(txds)
        txds.append([n, -1])
        par = parents.get(n)
        if par and par != n and source(par + '.txd')[0] is not None:
            txds[tidx[n]][1] = txd_index(par)
        return tidx[n]

    models = []
    for m in mids:
        name, txd, dd, flags, on, off = objs[m]
        path, location, pack = source(name + '.dff')
        models.append((name, txd_index(txd) if source(txd + '.txd')[0] else 0xFFFF, dd,
                       flags | (pack << 31), on, off, _radius(path, *location)))

    size = int(sec.get('sector', 50))
    xs = [t[1] for t in insts] or [0.0]
    ys = [t[2] for t in insts] or [0.0]
    ox, oy = math.floor(min(xs) / size) * size, math.floor(min(ys) / size) * size
    gw, gh = int((max(xs) - ox) // size) + 1, int((max(ys) - oy) // size) + 1
    cells = [[] for _ in range(gw * gh)]
    for t in insts:
        cx, cy = int((t[1] - ox) // size), int((t[2] - oy) // size)
        cells[cy * gw + cx].append(t)

    hdr = 40
    models_off = hdr
    txds_off = models_off + 36 * len(models)
    sectors_off = txds_off + 28 * len(txds)
    inst_off = sectors_off + 8 * gw * gh
    col_off = inst_off + 40 * len(insts)
    out = bytearray(struct.pack('<4sHHhhHHHHIIIII', b'DWLD', VERSION, size, ox, oy, gw, gh, len(models), len(txds),
                                len(insts), models_off, sectors_off, inst_off, col_off))
    assert len(out) == hdr
    for name, ti, dd, flags, on, off, rad in models:
        out += fname(name, 24) + struct.pack('<HHBBHI', ti, min(0xFFFF, int(dd)), on, off, rad, flags & 0xFFFFFFFF)
    for n, par in txds:
        out += fname(n, 24) + struct.pack('<hH', par, source(n + '.txd')[2])
    first = 0
    for c in cells:
        out += struct.pack('<IHH', first, len(c), 0)
        first += len(c)
    for c in cells:
        for mid, x, y, z, rot, is_lod, linked, area in c:
            switch = 100 if linked in midx else 0
            if is_lod:
                switch |= 0x8000
            out += struct.pack('<3i9hHHHHH', fx(x), fx(y), fx(z), *(fx16(r) for r in rot), midx[mid],
                               switch, midx.get(linked, 0xFFFF), area, 0)
    assert len(out) == col_off

    # collision: WorldColIndex[numModels], then the records (model space)
    cols, dups = _col_models(imgp, toc, {m[0] for m in models})
    other, other_dups = _col_models(intp, int_toc, {m[0] for m in models} - cols.keys())
    cols.update(other)
    dups += other_dups
    index, recs = bytearray(), bytearray()
    rec_base = col_off + 12 * len(models)
    nfaces = ncol = 0
    for m in models:
        c = cols.get(m[0])
        rec, rad, nf = _col_record(c) if c else (None, 0, 0)
        if rec is None:
            index += struct.pack('<IIHH', 0, 0, 0, 0)
            continue
        index += struct.pack('<IIHH', rec_base + len(recs), len(rec), rad, 0)
        recs += rec
        nfaces += nf
        ncol += 1
    out += index + recs
    stats = {'instances': len(insts), 'models': len(models), 'txds': len(txds), 'sectors': gw * gh,
             'col_models': ncol, 'col_faces': nfaces, 'col_kb': len(recs) >> 10, 'col_dups': dups,
             **{'skip_' + k: v for k, v in skipped.items() if v}}
    return pad4(bytes(out)), stats
