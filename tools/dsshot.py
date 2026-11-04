#!/usr/bin/env python3
"""DEBUG-build screenshot dumps (video::requestCapture, "DSCR" + RGB15) -> PNG.

  tools/dsshot.py sdcard/gtasads_shot0.dscr [out.png] [--scale N]   (default: next to the input, 1x)
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from assets import write_png  # noqa: E402


def convert(src, dst, scale=1):
    data = Path(src).read_bytes()
    if data[:4] != b'DSCR':
        sys.exit(f'{src}: not a DSCR dump')
    w, h = struct.unpack_from('<HH', data, 4)
    px = struct.unpack_from(f'<{w * h}H', data, 8)
    rgba = bytearray()
    for c in px:
        r, g, b = c & 31, (c >> 5) & 31, (c >> 10) & 31
        rgba += bytes(((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2), 255))
    if scale > 1:   # nearest neighbour, to look at single pixels (outlines, lines)
        rows = [bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h)]
        big = bytearray()
        for row in rows:
            line = b''.join(row[x * 4:x * 4 + 4] * scale for x in range(w))
            big += line * scale
        rgba, w, h = big, w * scale, h * scale
    write_png(dst, w, h, rgba)
    print(f'{src} -> {dst}')


if __name__ == '__main__':
    args = sys.argv[1:]
    scale = 1
    if '--scale' in args:
        i = args.index('--scale')
        scale = int(args[i + 1])
        del args[i:i + 2]
    if not args:
        sys.exit(__doc__)
    convert(args[0], args[1] if len(args) > 1 else str(Path(args[0]).with_suffix('.png')), scale)
