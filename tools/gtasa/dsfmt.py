"""Shared DS binary helpers + DSPK pack writer (see src/assets/ds_formats.h)."""
import os
import struct


def fx(v, frac=12):
    """float -> s32 fixed point (20.12 by default). NaN -> 0."""
    v *= 1 << frac
    return max(-0x80000000, min(0x7FFFFFFF, round(v))) if v == v and abs(v) != float('inf') else 0


def fx16(v, frac=12):
    v *= 1 << frac
    return max(-32768, min(32767, round(v))) if v == v and abs(v) != float('inf') else 0


def rgb15(r, g, b):
    return (r >> 3) | (g >> 3) << 5 | (b >> 3) << 10


def name(s, n):
    b = s.encode('latin1', 'replace')[:n - 1]
    return b + b'\0' * (n - len(b))


def pad4(b):
    return b + b'\0' * (-len(b) % 4)


def fnv1a(s):
    h = 0x811C9DC5
    for c in s.lower().encode('latin1', 'replace'):
        h = ((h ^ c) * 0x01000193) & 0xFFFFFFFF
    return h


def write_pack(path, entries, align=4):
    """entries: {name: bytes}. Header, sorted-by-hash TOC, names blob, data."""
    items = sorted(entries.items(), key=lambda kv: (fnv1a(kv[0]), kv[0].lower()))
    names = bytearray()
    name_offs = []
    for n, _ in items:
        name_offs.append(len(names))
        names += n.encode('latin1', 'replace') + b'\0'
    names = pad4(bytes(names))
    hdr = 16
    toc = 16 * len(items)
    names_off = hdr + toc
    data_off = names_off + len(names)
    toc_b = bytearray()
    blobs = bytearray()
    for (n, b), no in zip(items, name_offs):
        blobs += b'\0' * (-(data_off + len(blobs)) % align)
        toc_b += struct.pack('<IIII', fnv1a(n), no, data_off + len(blobs), len(b))
        blobs += b
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(struct.pack('<4sHHII', b'DSPK', 1, 0, len(items), names_off))
        f.write(toc_b)
        f.write(names)
        f.write(blobs)
