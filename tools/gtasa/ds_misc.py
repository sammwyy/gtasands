"""COL -> DCOL and IFP -> DANM converters."""
import struct
from fnmatch import fnmatchcase

from . import col, ifp
from .ds_mesh import decimate
from .dsfmt import fx, name, pad4

COL_DEFAULTS = dict(mode='full', face_ratio=1.0, min_faces=16)
ANIM_DEFAULTS = dict(keep_every=1, max_frames=0, drop_translation=False, remove=[])


def convert_col(data, p):
    models = col.parse(data)
    mode = p['mode']
    out = bytearray(struct.pack('<4sHH', b'DCOL', 1, len(models)))
    fin = fout = 0
    for c in models:
        verts, faces = c.verts, c.faces
        fin += len(faces)
        if mode == 'bounds':
            verts, faces = [], []
        elif mode == 'simple' and faces:
            tris = [(a, b, cc, (m, l)) for a, b, cc, m, l in faces]
            rep, tris = decimate(verts, tris, max(p['min_faces'], int(len(tris) * p['face_ratio'])))
            used = sorted({i for t in tris for i in t[:3]})
            remap = {v: i for i, v in enumerate(used)}
            src = rep or verts
            verts = [src[i] for i in used]
            faces = [(remap[a], remap[b], remap[cc], ml[0], ml[1]) for a, b, cc, ml in tris]
        fout += len(faces)
        rec = bytearray(name(c.name, 24) + struct.pack('<HBB', c.model_id, c.version, 0))
        rec += struct.pack('<10i', *(fx(v) for v in (*c.bmin, *c.bmax, *c.center, c.radius)))
        rec += struct.pack('<4H', len(c.spheres), len(c.boxes), len(verts), len(faces))
        for (ctr, r, surf) in c.spheres:
            rec += struct.pack('<4i4B', *(fx(v) for v in ctr), fx(r), *surf)
        for (mn, mx, surf) in c.boxes:
            rec += struct.pack('<6i4B', *(fx(v) for v in (*mn, *mx)), *surf)
        rec += pad4(b''.join(struct.pack('<3h', *(max(-32768, min(32767, round(x * 128))) for x in v)) for v in verts))
        for a, b, cc, m, l in faces:
            rec += struct.pack('<3HBB', a, b, cc, m, l)
        out += struct.pack('<I', len(rec)) + rec
    return bytes(out), {'faces_in': fin, 'faces_out': fout}


def convert_ifp(data, p):
    block, anims = ifp.parse(data)
    anims = [a for a in anims if not any(fnmatchcase(a.name.lower(), r.lower()) for r in p['remove'])]
    out = bytearray(struct.pack('<4sHH', b'DANM', 1, len(anims)) + name(block, 24))
    kin = kout = 0
    step = max(1, int(p['keep_every']))
    for a in anims:
        out += name(a.name, 24) + struct.pack('<HH', len(a.seqs), 0)
        for si, s in enumerate(a.seqs):
            fr = s.frames
            kin += len(fr)
            if len(fr) > 2 and step > 1:
                fr = fr[::step] + ([fr[-1]] if (len(fr) - 1) % step else [])
            if p['max_frames'] and len(fr) > p['max_frames']:
                k = p['max_frames']
                fr = [fr[round(i * (len(fr) - 1) / (k - 1))] for i in range(k)] if k > 1 else fr[:1]
            trans = s.has_trans and not (p['drop_translation'] and si > 0)
            kout += len(fr)
            out += name(s.name, 24) + struct.pack('<hBBHH', s.bone, int(trans), 0, len(fr), 0)
            body = b''.join(struct.pack('<5h', *f[:5]) + (struct.pack('<3h', *f[5:8]) if trans else b'') for f in fr)
            out += pad4(body)
    return bytes(out), {'keys_in': kin, 'keys_out': kout}
