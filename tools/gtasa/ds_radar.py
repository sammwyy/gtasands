"""radarNN.txd -> pre-baked radar tile (DSRT record): RGB15 64x64 + 32x32 mip.

The bottom-screen radar samples tiles per pixel on the CPU. Baking them here
(area-filtered from the PC source, opaque bit set, mip included) means the
runtime streams a tile with one fread and never decodes or filters anything.
"""
import struct

from . import txd
from .ds_texture import pick_level, resize

FULL, MIP = 64, 32
DEFAULTS = dict()


def _rgb15(px, n):
    return struct.pack(f'<{n}H', *((px[i * 4] >> 3) | (px[i * 4 + 1] >> 3) << 5 | (px[i * 4 + 2] >> 3) << 10 | 0x8000
                                   for i in range(n)))


def _opaque(px):
    px = bytearray(px)
    px[3::4] = b'\xff' * (len(px) // 4)
    return px


def _fit(px, w, h, size):
    """Box filter down by integer factors (nearest if the source is odd-sized)."""
    while w > size and h > size and w % 2 == 0 and h % 2 == 0:
        px, w, h = resize(px, w, h, w // 2, h // 2), w // 2, h // 2
    return resize(px, w, h, size, size)


def convert_radar(data, p):
    texs = txd.parse(data)
    if not texs:
        raise ValueError('empty txd')
    tx = texs[0]
    w, h, px = txd.decode(tx, pick_level(tx, FULL, FULL))
    full = _fit(_opaque(px), w, h, FULL)
    mip = resize(full, FULL, FULL, MIP, MIP)
    return _rgb15(full, FULL * FULL) + _rgb15(mip, MIP * MIP), {'tiles': 1}
