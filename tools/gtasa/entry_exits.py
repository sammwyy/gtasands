"""Compile IPL enex records into a compact, versioned entrance table."""
import struct
from pathlib import Path

from . import world

RECORD = struct.Struct("<11fHh8sBBBBHH")
HEADER = struct.Struct("<4sHH")


def parse(path):
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
        fields = [field.strip().strip('"') for field in line.split(",")]
        if len(fields) != 18:
            continue
        try:
            values = tuple(map(float, fields[:11]))
            area, flags = int(fields[11]), int(fields[12])
            sky, peds, on, off = map(int, fields[14:18])
        except ValueError:
            continue
        yield values, flags, area, fields[13].lower(), sky, peds, on, off


def build(game, _sec):
    dat = world.resolve(game, "data/gta.dat")
    if dat is None:
        raise FileNotFoundError("missing data/gta.dat (run: tools/assets.py sync)")
    records = []
    for line in world._lines(dat):
        kind, _, name = line.partition(" ")
        if kind.upper() != "IPL" or not name.lower().endswith(".ipl"):
            continue
        path = world.resolve(game, name.strip())
        if path:
            placed = world.parse_ipl_text(path)
            for record in parse(path):
                values, _, area, _, *_ = record
                x, y, z = values[:3]
                candidates = [((px - x) ** 2 + (py - y) ** 2 + (pz - z) ** 2, inst_area & 0xFF)
                              for _, px, py, pz, *tail in placed
                              for inst_area in [tail[-2]] if (inst_area & 0xFF) not in (0, 13)]
                nearest = min(candidates) if candidates else None
                source_area = nearest[1] if nearest and nearest[0] < 12 ** 2 else (
                    area if z > 900 and area not in (0, 13) else 0)
                records.append((record, source_area))
    for i, (record, source_area) in enumerate(records):
        if source_area or record[0][2] < 900 or not record[3]:
            continue
        matches = [r[2] for r, src in records if src == 0 and r[3] == record[3] and r[2] not in (0, 13)]
        if matches:
            records[i] = (record, matches[0])
    if len(records) > 0xFFFF:
        raise ValueError("too many IPL entry exits")
    out = bytearray(HEADER.pack(b"DENX", 3, len(records)))
    links = []
    for i, (record, src) in enumerate(records):
        _, flags, area, name, *_ = record
        matches = (j for j, (other, other_src) in enumerate(records) if j != i and other[3] == name and
                   other_src != src and (flags & 4) != (other[1] & 4))
        links.append(next(matches, 0xFFFF) if name else 0xFFFF)
    for ((values, flags, area, name, sky, peds, on, off), src), link in zip(records, links):
        out += RECORD.pack(*values, flags & 0xFFFF, area, name.encode("ascii", errors="replace")[:8],
                           sky, peds, on, off, src, link)
    return bytes(out), {"entries": len(records), "exterior": sum(src == 0 for _, src in records),
                        "linked": sum(link != 0xFFFF for link in links)}
