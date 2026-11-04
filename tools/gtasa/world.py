"""gta.dat + IDE + IPL (text and binary stream IPLs inside gta3.img) -> DWLD.

The DS never parses map text files: CFileLoader::LoadLevel / LoadObjectTypes /
LoadScene are run here, and the runtime gets a sector grid of pre-transformed
instances (fixed-point position + rotation matrix) plus the model and TXD
tables it needs to stream them from gta3.dspk. Layout: src/assets/ds_formats.h.
"""
import math
import struct
from pathlib import Path

from . import dff, img
from .dsfmt import fx, fx16, name as fname, pad4

VERSION = 1
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
    r = max((math.sqrt(s[0] ** 2 + s[1] ** 2 + s[2] ** 2) + s[3] for s in (g.sphere for g in m.geometries) if s),
            default=8.0)
    return max(1, min(0xFFFF, math.ceil(r)))


def build(game, sec):
    """Returns (blob, stats)."""
    game = Path(game)
    dat = resolve(game, sec.get('dat', 'data/gta.dat'))
    imgp = resolve(game, sec.get('img', 'models/gta3.img'))
    if not dat or not imgp:
        raise FileNotFoundError('need data/gta.dat and models/gta3.img (run: tools/assets.py sync)')
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
        while f'{base}_stream{k}.ipl' in toc:
            o, s = toc[f'{base}_stream{k}.ipl']
            stream += parse_ipl_binary(img.read_entry(imgp, o, s))
            k += 1
        # lod indices of both the text and the stream IPLs point into the text list
        lod_of = {t[9] for t in text + stream if t[9] >= 0}
        for i, t in enumerate(text + stream):
            mid, x, y, z, qx, qy, qz, qw, area, _lod = t
            o = objs.get(mid)
            if (area & 0xFF) not in (0, AREA_ANY):
                skipped['area'] += 1
                continue
            if o is None or o[0] + '.dff' not in toc:
                skipped['model'] += 1
                continue
            is_lod = (i < len(text) and i in lod_of) or o[0].startswith('lod')
            if is_lod and not keep_lod:
                skipped['lod'] += 1
                continue
            if bounds and not (bounds[0] <= x <= bounds[2] and bounds[1] <= y <= bounds[3]):
                skipped['bounds'] += 1
                continue
            insts.append((mid, x, y, z, rotation(qx, qy, qz, qw)))

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
        if par and par != n and par + '.txd' in toc:
            txds[tidx[n]][1] = txd_index(par)
        return tidx[n]

    models = []
    for m in mids:
        name, txd, dd, flags, on, off = objs[m]
        models.append((name, txd_index(txd) if txd + '.txd' in toc else 0xFFFF, dd, flags, on, off,
                       _radius(imgp, *toc[name + '.dff'])))

    size = int(sec.get('sector', 50))
    xs = [t[1] for t in insts] or [0.0]
    ys = [t[2] for t in insts] or [0.0]
    ox, oy = math.floor(min(xs) / size) * size, math.floor(min(ys) / size) * size
    gw, gh = int((max(xs) - ox) // size) + 1, int((max(ys) - oy) // size) + 1
    cells = [[] for _ in range(gw * gh)]
    for t in insts:
        cx, cy = int((t[1] - ox) // size), int((t[2] - oy) // size)
        cells[cy * gw + cx].append(t)

    hdr = 36
    models_off = hdr
    txds_off = models_off + 36 * len(models)
    sectors_off = txds_off + 28 * len(txds)
    inst_off = sectors_off + 8 * gw * gh
    out = bytearray(struct.pack('<4sHHhhHHHHIIII', b'DWLD', VERSION, size, ox, oy, gw, gh, len(models), len(txds),
                                len(insts), models_off, sectors_off, inst_off))
    assert len(out) == hdr
    for name, ti, dd, flags, on, off, rad in models:
        out += fname(name, 24) + struct.pack('<HHBBHI', ti, min(0xFFFF, int(dd)), on, off, rad, flags & 0xFFFFFFFF)
    for n, par in txds:
        out += fname(n, 24) + struct.pack('<hH', par, 0)
    first = 0
    for c in cells:
        out += struct.pack('<IHH', first, len(c), 0)
        first += len(c)
    for c in cells:
        for mid, x, y, z, rot in c:
            out += struct.pack('<3i9hH', fx(x), fx(y), fx(z), *(fx16(r) for r in rot), midx[mid])
    stats = {'instances': len(insts), 'models': len(models), 'txds': len(txds), 'sectors': gw * gh,
             **{'skip_' + k: v for k, v in skipped.items() if v}}
    return pad4(bytes(out)), stats
