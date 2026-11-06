#!/usr/bin/env python3
"""Lists the baked map collision (data/world.dwld, ds::WorldCol) around a point.

  tools/colnear.py X Y Z [R=1.0] [--dwld nitrofs/data/world.dwld]

Prints every instance whose collision has faces/boxes within R metres (2D) of
(X, Y) and between Z - 0.5 and Z + 2.5: model, position, world normal z and
the world-space triangles / boxes. Handy to explain what stopped CJ in a
headless collision test (gtasads_coltest.txt).
"""
import math
import struct
import sys
from pathlib import Path


def main():
    args = sys.argv[1:]
    dwld = Path(__file__).resolve().parent.parent / 'nitrofs/data/world.dwld'
    if '--dwld' in args:
        i = args.index('--dwld')
        dwld = Path(args[i + 1])
        del args[i:i + 2]
    if len(args) < 3:
        sys.exit(__doc__)
    X, Y, Z = map(float, args[:3])
    R = float(args[3]) if len(args) > 3 else 1.0
    d = dwld.read_bytes()
    _, ver, _, _, _, _, _, nm, _, ni, mo, _, io, co = struct.unpack_from('<4sHHhhHHHHIIIII', d)
    if ver != 5:
        sys.exit(f'{dwld}: DWLD v{ver}, expected 5')
    names = [d[mo + i * 36:mo + i * 36 + 24].split(b'\0')[0].decode() for i in range(nm)]
    for i in range(ni):
        p = struct.unpack_from('<3i9hH', d, io + i * 36)
        pos = [v / 4096 for v in p[:3]]
        rot = [v / 4096 for v in p[3:12]]
        off, _, rad, _ = struct.unpack_from('<IIHH', d, co + p[12] * 12)
        if not off or math.hypot(pos[0] - X, pos[1] - Y) - rad / 16 > R:
            continue
        ns, nb, nn, nv, nf = struct.unpack_from('<6h6H', d, off)[6:11]
        o = off + 24 + ns * 12
        boxes = [struct.unpack_from('<6h4B', d, o + k * 16) for k in range(nb)]
        o += nb * 16 + nn * 16
        faces = [struct.unpack_from('<3H2B3hH', d, o + k * 16) for k in range(nf)]
        o += nf * 16
        verts = [struct.unpack_from('<3h', d, o + k * 6) for k in range(nv)]

        def world(v):
            lx, ly, lz = (c / 128 for c in v)
            return tuple(round(pos[j] + lx * rot[j] + ly * rot[3 + j] + lz * rot[6 + j], 2) for j in range(3))

        def near(pts):
            dx = max(min(q[0] for q in pts) - X, 0, X - max(q[0] for q in pts))
            dy = max(min(q[1] for q in pts) - Y, 0, Y - max(q[1] for q in pts))
            return math.hypot(dx, dy) < R and max(q[2] for q in pts) > Z - 0.5 and min(q[2] for q in pts) < Z + 2.5

        tris = []
        for f in faces:
            t = [world(verts[f[k]]) for k in range(3)]
            if near(t):
                nl = [c / 4096 for c in f[5:8]]
                tris.append((round(nl[0] * rot[2] + nl[1] * rot[5] + nl[2] * rot[8], 2), t))
        bx = [(world(b[0:3]), world(b[3:6])) for b in boxes if near([world(b[0:3]), world(b[3:6])])]
        if tris or bx:
            print(f'{names[p[12]]} pos {pos[0]:.1f},{pos[1]:.1f},{pos[2]:.2f} faces {len(tris)} boxes {bx}')
            for nz, t in tris[:16]:
                print(f'   nz {nz:5.2f} {t}')


if __name__ == '__main__':
    main()
