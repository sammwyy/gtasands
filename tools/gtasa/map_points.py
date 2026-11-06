"""Exterior map markers from GTA SA IPL entrance/exit (enex) records."""
import struct
from pathlib import Path


TYPES = (
    ("AMMUN", 0), ("BARBER", 1), ("FDBURG", 2), ("FDCHICK", 3),
    ("FDPIZA", 4), ("FDREST", 5), ("DINER", 5), ("GYM", 6),
    ("CLOTH", 7), ("POLICE", 8), ("TATTO", 9), ("SV", 10),
    ("CARMOD", 11),
)


def build(game, _sec):
    root = Path(game) / "data/maps"
    if not root.is_dir():
        raise FileNotFoundError(f"missing {root}")
    points = set()
    for path in sorted(root.rglob("*")):
        if path.suffix.lower() != ".ipl" or "interior" in path.parts:
            continue
        section = ""
        for raw in path.read_text(errors="replace").splitlines():
            line = raw.split("#", 1)[0].strip()
            if line.lower() == "enex":
                section = "enex"
                continue
            if line.lower() == "end":
                section = ""
                continue
            if section != "enex" or not line:
                continue
            fields = [x.strip().strip('"') for x in line.split(",")]
            if len(fields) < 14:
                continue
            name = fields[13].upper()
            kind = next((index for prefix, index in TYPES if name.startswith(prefix)), None)
            if kind is None:
                continue
            try:
                x, y = round(float(fields[0])), round(float(fields[1]))
            except ValueError:
                continue
            if -3000 <= x < 3000 and -3000 <= y < 3000:
                points.add((x, y, kind))
    out = bytearray(struct.pack("<4sHH", b"DPOI", 1, len(points)))
    for x, y, kind in sorted(points):
        out += struct.pack("<hhBB", x, y, kind, 0)
    return bytes(out), {"entrances": len(points)}
