#!/usr/bin/env python3
"""DEBUG-build screenshot dumps (video::requestCapture, "DSCR" + RGB15) -> PNG.

  tools/dsshot.py sdcard/gtasads_shot0.dscr [out.png]   (default: next to the input)
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from assets import write_png  # noqa: E402


def convert(src, dst):
    data = Path(src).read_bytes()
    if data[:4] != b'DSCR':
        sys.exit(f'{src}: not a DSCR dump')
    w, h = struct.unpack_from('<HH', data, 4)
    px = struct.unpack_from(f'<{w * h}H', data, 8)
    rgba = bytearray()
    for c in px:
        r, g, b = c & 31, (c >> 5) & 31, (c >> 10) & 31
        rgba += bytes(((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2), 255))
    write_png(dst, w, h, rgba)
    print(f'{src} -> {dst}')


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for s in sys.argv[1:2]:
        convert(s, sys.argv[2] if len(sys.argv) > 2 else str(Path(s).with_suffix('.png')))
