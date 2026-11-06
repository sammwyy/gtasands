"""Exterior map markers from GTA SA IPL entrance/exit (enex) records."""
import struct

from . import entry_exits


TYPES = (
    ("AMMUN", 0), ("BARBER", 1), ("FDBURG", 2), ("FDCHICK", 3),
    ("FDPIZA", 4), ("FDREST", 5), ("DINER", 5), ("GYM", 6),
    ("CLOTH", 7), ("POLICE", 8), ("TATTO", 9), ("SV", 10),
    ("CARMOD", 11),
)


def build(game, _sec):
    blob, _ = entry_exits.build(game, {})
    points = set()
    for offset in range(entry_exits.HEADER.size, len(blob), entry_exits.RECORD.size):
        fields = entry_exits.RECORD.unpack_from(blob, offset)
        if fields[18] != 0:
            continue
        name = fields[13].decode("ascii").rstrip("\0").upper()
        kind = next((index for prefix, index in TYPES if name.startswith(prefix)), 12)
        x, y = round(fields[0]), round(fields[1])
        if -3000 <= x < 3000 and -3000 <= y < 3000:
            points.add((x, y, kind))
    out = bytearray(struct.pack("<4sHH", b"DPOI", 2, len(points)))
    for x, y, kind in sorted(points):
        out += struct.pack("<hhBB", x, y, kind, 0)
    return bytes(out), {"entrances": len(points)}
