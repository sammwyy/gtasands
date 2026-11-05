"""peds.ide + pedstats.dat + pedgrp.dat + popcycle.dat + animgrp.dat + ped path nodes + main.scm zone setup -> DPED.

CFileLoader::LoadPedObject, CPedStats::LoadPedStats, CPopulation::LoadPedGroups,
CPopCycle::Initialise, CAnimManager::ReadAnimAssociationDefinitions (the walkcycle
groups the peds use) and CPathFind::LoadPathFindData (ped nodes only, inside the
map bounds) run here. The zone population setup that main.scm does on a new game
(SET_ZONE_POPULATION_TYPE / SET_ZONE_GANG_STRENGTH / SET_ZONE_DEALER_STRENGTH /
SET_ZONE_POPULATION_RACE in the start-up thread) is found by scanning the script
for those opcodes with an inline 8-byte zone label, and baked per info.zon zone.
Layout: src/assets/ds_formats.h (DPED).
"""
import re
import struct
from pathlib import Path

from .dsfmt import name as fname
from .world import resolve

VERSION = 1
CELL = 50                 # path node grid cell (m)
MARGIN = 60               # nodes kept around the bounds (m)
ZONE_TYPES = 20           # eZonePopulationType::COUNT
POP_TIMES = 12            # 2-hour slots
POP_GROUPS = 18           # ePopcycleGroupPerc
NUM_PEDGROUPS = 57        # ePopcyclePedGroup (pedgrp.dat lines)

PED_TYPES = ['PLAYER1', 'PLAYER2', 'PLAYER_NETWORK', 'PLAYER_UNUSED', 'CIVMALE', 'CIVFEMALE', 'COP',
             'GANG1', 'GANG2', 'GANG3', 'GANG4', 'GANG5', 'GANG6', 'GANG7', 'GANG8', 'GANG9', 'GANG10',
             'DEALER', 'MEDIC', 'FIREMAN', 'CRIMINAL', 'BUM', 'PROSTITUTE', 'SPECIAL']

# main.scm opcodes (eScriptCommands)
OP_POP_TYPE, OP_DEALER, OP_GANG, OP_RACE = 0x0767, 0x076A, 0x076C, 0x0874


def _lines(path, comment='#'):
    for line in path.read_text(errors='replace').splitlines():
        line = line.split(comment, 1)[0].strip()
        if line:
            yield line


def race_of(model):
    """CPopulation::FindPedRaceFromName: the first of the two leading letters that names one."""
    for c in model[:2].upper():
        if c == 'B':
            return 1
        if c == 'W':
            return 2
        if c in 'OI':
            return 3
        if c == 'H':
            return 4
    return 0


def parse_peds_ide(path):
    """peds section: id, model, txd, pedType, stats, animGroup, carsMask (hex), flags, animFile, radio1, radio2, ..."""
    out = []
    in_peds = False
    for line in _lines(path):
        low = line.lower()
        if ',' not in line:
            in_peds = low == 'peds'
            continue
        if not in_peds:
            continue
        a = [x.strip() for x in line.split(',')]
        if len(a) < 8:
            continue
        try:
            out.append(dict(id=int(a[0]), model=a[1].lower(), txd=a[2].lower(), type=a[3].upper(), stat=a[4].upper(),
                            anim=a[5].lower(), cars=int(a[6], 16) if a[6] else 0, flags=int(a[7], 0) if a[7] else 0))
        except ValueError:
            continue
    return [p for p in out if p['model'] != 'null']


def parse_pedstats(path):
    """name fleeDistance headingChangeRate fear temper lawfulness sexiness attackStrength defendWeakness shootingRate decisionMaker"""
    out = []
    for line in _lines(path):
        a = line.split()
        if len(a) < 10 or not a[0].upper().startswith('STAT_'):
            continue
        try:
            v = [float(x) for x in a[1:11]] + [0.0] * (10 - len(a[1:11]))
        except ValueError:
            continue
        out.append((a[0].upper(), v))
    return out


def parse_animgrp(path):
    """name, file, type, count / anim names / end -> {name: [walk, run, sprint, idle, roadcross, walkstart]}."""
    groups, cur = {}, None
    for line in _lines(path):
        if ',' in line:
            a = [x.strip() for x in line.split(',')]
            cur = (a[0].lower(), a[2].lower() if len(a) > 2 else '', [])
            continue
        if cur is None:
            continue
        if line.lower() == 'end':
            if cur[1] == 'walkcycle':
                groups[cur[0]] = cur[2]
            cur = None
        else:
            cur[2].append(line.split()[0])
    return groups


def parse_pedgrp(path, by_name):
    """LoadGroup: one group per line with models (blank lines skipped), models by name."""
    groups = []
    for line in path.read_text(errors='replace').splitlines():
        line = line.split('#', 1)[0]
        names = [x for x in re.split(r'[,\s]+', line) if x]
        ids = [by_name[n.lower()] for n in names if n.lower() in by_name]
        if not names:
            continue
        groups.append(ids)
        if len(groups) == NUM_PEDGROUPS:
            break
    while len(groups) < NUM_PEDGROUPS:
        groups.append([])
    return groups


def parse_popcycle(path):
    """CPopCycle::Initialise: [zone][weekday/weekend][2h slot] = 24 numbers (lines starting with // skipped)."""
    rows = []
    for line in path.read_text(errors='replace').splitlines():
        line = line.split('//', 1)[0].strip()
        if not line:
            continue
        try:
            v = [int(x) for x in line.split()[:24]]
        except ValueError:
            continue
        if len(v) == 24:
            rows.append(v)
    want = ZONE_TYPES * 2 * POP_TIMES
    rows = rows[:want] + [[0] * 24] * (want - len(rows[:want]))
    # the original rescales the group percentages to 100 when they add up to less
    for r in rows:
        s = sum(r[6:])
        if 0 < s < 100:
            r[6:] = [p * 100 // s for p in r[6:]]
    return rows


def parse_zones(path):
    """info.zon navigation zones: name, type, x1, y1, z1, x2, y2, z2, level, gxt."""
    out = []
    for line in _lines(path):
        a = [x.strip() for x in line.split(',')]
        if len(a) < 9:
            continue
        try:
            t = int(a[1])
            x1, y1, x2, y2 = float(a[2]), float(a[3]), float(a[5]), float(a[6])
        except ValueError:
            continue
        if t in (0, 1):
            out.append((a[0].upper(), min(x1, x2), min(y1, y2), max(x1, x2), max(y1, y2)))
    return out


def scm_zone_setup(scm):
    """The new-game zone setup: SET_ZONE_* commands with an inline 8-byte label (type 0x09)
    in the first dense run of SET_ZONE_POPULATION_TYPE (the start-up thread). Later writes
    (missions, gang wars) are ignored."""
    def val(i):
        t = scm[i]
        if t == 4:
            return struct.unpack('<b', scm[i + 1:i + 2])[0], i + 2
        if t == 5:
            return struct.unpack('<h', scm[i + 1:i + 3])[0], i + 3
        if t == 1:
            return struct.unpack('<i', scm[i + 1:i + 5])[0], i + 5
        return None, i + 1

    def scan(op):
        pat = struct.pack('<H', op) + b'\x09'
        i = scm.find(pat)
        while i >= 0:
            yield i, scm[i + 3:i + 11].split(b'\0')[0].decode('latin1').upper()
            i = scm.find(pat, i + 1)

    pops = list(scan(OP_POP_TYPE))
    if not pops:
        return {}, (0, 0)
    lo = hi = pops[0][0]
    for off, _ in pops:
        if off - hi > 4096:
            break
        hi = off
    lo, hi = lo - 8192, hi + 8192
    zones = {}

    def z(label):
        return zones.setdefault(label, dict(pop=None, gang=[0] * 10, dealer=0, races=None))
    for off, label in pops:
        if lo <= off <= hi:
            v, _ = val(off + 11)
            if v is not None and z(label)['pop'] is None:
                z(label)['pop'] = v
    seen = set()
    for off, label in scan(OP_GANG):
        if lo <= off <= hi:
            g, j = val(off + 11)
            s, _ = val(j)
            if g is not None and s is not None and 0 <= g < 10 and (label, g) not in seen:
                seen.add((label, g))
                z(label)['gang'][g] = max(0, min(255, s))
    for off, label in scan(OP_DEALER):
        if lo <= off <= hi:
            v, _ = val(off + 11)
            if v is not None:
                z(label)['dealer'] = max(0, min(255, v))
    for off, label in scan(OP_RACE):
        if lo <= off <= hi:
            v, _ = val(off + 11)
            if v is not None and z(label)['races'] is None:
                z(label)['races'] = v & 15
    return zones, (lo, hi)


def read_ped_nodes(paths_dir, bounds):
    """CPathFind::LoadPathFindData for the 64 map areas: ped nodes (after the vehicle nodes) inside
    bounds + MARGIN, with their links to other kept ped nodes. Returns nodes [(x8, y8, z8, width, flags)],
    links per node [global keys]."""
    x0, y0, x1, y1 = bounds[0] - MARGIN, bounds[1] - MARGIN, bounds[2] + MARGIN, bounds[3] + MARGIN
    nodes = {}      # (area, id) -> (x8, y8, z8, width, flags, [link keys])
    for area in range(64):
        p = resolve(paths_dir, f'nodes{area}.dat')
        if not p:
            continue
        d = p.read_bytes()
        n, nveh, nped, nnavi, nlinks = struct.unpack_from('<5I', d, 0)
        node_off = 20
        link_off = node_off + n * 28 + nnavi * 14
        for i in range(nveh, n):
            o = node_off + i * 28 + 8
            x, y, z, _, base, ar, nid, width, _, flags = struct.unpack_from('<3hhhHHBBI', d, o)
            if not (x0 * 8 <= x <= x1 * 8 and y0 * 8 <= y <= y1 * 8):
                continue
            links = []
            for k in range(flags & 15):
                la, ln = struct.unpack_from('<HH', d, link_off + (base + k) * 4)
                links.append((la, ln))
            nodes[(area, i)] = (x, y, z, width, flags, links)
    return nodes


def build(game, sec, world_bounds=None):
    """Returns (blob, stats)."""
    game = Path(game)
    ide = resolve(game, 'data/peds.ide')
    st_path = resolve(game, 'data/pedstats.dat')
    grp_path = resolve(game, 'data/pedgrp.dat')
    pop_path = resolve(game, 'data/popcycle.dat')
    anim_path = resolve(game, 'data/animgrp.dat')
    zon_path = resolve(game, 'data/info.zon')
    paths_dir = resolve(game, 'data/paths')
    if not all((ide, st_path, grp_path, pop_path, anim_path, zon_path, paths_dir)):
        raise FileNotFoundError('need data/peds.ide, pedstats.dat, pedgrp.dat, popcycle.dat, animgrp.dat, info.zon, '
                                'paths/nodes*.dat (run: tools/assets.py sync)')
    bounds = sec.get('bounds') or world_bounds or [-3000, -3000, 3000, 3000]

    peds = parse_peds_ide(ide)
    stats = parse_pedstats(st_path)
    stat_idx = {n: i for i, (n, _) in enumerate(stats)}
    agroups_all = parse_animgrp(anim_path)
    used_anims = sorted({p['anim'] for p in peds if p['anim'] in agroups_all})
    if 'man' in used_anims:          # group 0 = the default (unknown groups fall back to it)
        used_anims.remove('man')
    used_anims.insert(0, 'man')
    anim_idx = {n: i for i, n in enumerate(used_anims)}
    by_name = {p['model']: i for i, p in enumerate(peds)}
    groups = parse_pedgrp(grp_path, by_name)
    pop = parse_popcycle(pop_path)

    # zones: info.zon rects that touch the bounds, with the script's population setup
    scm = resolve(game, 'data/script/main.scm')
    setup, scm_range = scm_zone_setup(scm.read_bytes()) if scm else ({}, (0, 0))
    zones = []
    for label, zx1, zy1, zx2, zy2 in parse_zones(zon_path):
        if zx2 < bounds[0] - MARGIN or zx1 > bounds[2] + MARGIN or zy2 < bounds[1] - MARGIN or zy1 > bounds[3] + MARGIN:
            continue
        s = setup.get(label, {})
        pop_type = s.get('pop')
        zones.append((max(-32768, int(zx1)), max(-32768, int(zy1)), min(32767, int(zx2)), min(32767, int(zy2)),
                      5 if pop_type is None or not 0 <= pop_type < ZONE_TYPES else pop_type,   # RESIDENTIAL_AVERAGE
                      15 if s.get('races') is None else s['races'], s.get('dealer', 0), s.get('gang', [0] * 10), label))

    # path nodes: sorted by grid cell, links remapped to local indices
    raw = read_ped_nodes(paths_dir, bounds)
    gx0, gy0 = int(bounds[0] - MARGIN) // CELL * CELL, int(bounds[1] - MARGIN) // CELL * CELL
    gw = (int(bounds[2] + MARGIN) - gx0) // CELL + 1
    gh = (int(bounds[3] + MARGIN) - gy0) // CELL + 1

    def cell(n):
        cx = min(gw - 1, max(0, (n[0] // 8 - gx0) // CELL))
        cy = min(gh - 1, max(0, (n[1] // 8 - gy0) // CELL))
        return cy * gw + cx
    keys = sorted(raw, key=lambda k: (cell(raw[k]), k))
    local = {k: i for i, k in enumerate(keys)}
    node_blob, link_blob, nlinks = bytearray(), bytearray(), 0
    cells = [[0, 0] for _ in range(gw * gh)]
    for i, k in enumerate(keys):
        x, y, z, width, flags, links = raw[k]
        c = cell(raw[k])
        if cells[c][1] == 0:
            cells[c][0] = i
        cells[c][1] += 1
        ll = [local[t] for t in links if t in local]
        # flags: bit0 dontWander, bit1 switchedOff, bit2 deadEnd, high nibble spawn probability
        f = (1 if flags & (1 << 10) else 0) | (2 if flags & (1 << 5) else 0) | (4 if flags & (1 << 4) else 0) | \
            (((flags >> 16) & 15) << 4)
        node_blob += struct.pack('<3hHBBBB', x, y, z, nlinks, len(ll), width, f, 0)
        for t in ll:
            link_blob += struct.pack('<H', t)
        nlinks += len(ll)
    if len(link_blob) % 4:
        link_blob += b'\0\0'

    # tables
    model_blob, name_blob = bytearray(), bytearray()
    for p in peds:
        pt = PED_TYPES.index(p['type']) if p['type'] in PED_TYPES else 4
        model_blob += struct.pack('<HBBBBBBHH', p['id'], pt, stat_idx.get(p['stat'], 0), anim_idx.get(p['anim'], 0),
                                  race_of(p['model']), p['flags'] & 0xFF, 0, p['cars'] & 0xFFFF, 0)
        name_blob += fname(p['model'], 24) + fname(p['txd'], 24)
    stat_blob = bytearray()
    for _, v in stats:
        stat_blob += struct.pack('<10B2x', min(255, int(v[0])), min(255, int(v[1] * 10)), *[min(255, int(x)) for x in v[2:6]],
                                 min(255, int(v[6] * 20)), min(255, int(v[7] * 20)), min(255, int(v[8])), min(255, int(v[9])))
    anim_blob = bytearray()
    for n in used_anims:
        clips = (agroups_all[n] + [''] * 6)[:6]
        anim_blob += fname(n, 16) + b''.join(fname(c, 24) for c in clips)
    grp_blob, members = bytearray(), bytearray()
    first = 0
    for g in groups:
        grp_blob += struct.pack('<HH', first, len(g))
        for m in g:
            members += struct.pack('<H', m)
        first += len(g)
    grp_blob += members
    if len(grp_blob) % 4:
        grp_blob += b'\0\0'
    pop_blob = b''.join(struct.pack('<24B', *[max(0, min(255, x)) for x in r]) for r in pop)
    zone_blob = b''.join(struct.pack('<4hBBBB10B2x', x1, y1, x2, y2, pt, races, dealer, 0, *gang)
                         for x1, y1, x2, y2, pt, races, dealer, gang, _ in zones)
    cell_blob = b''.join(struct.pack('<HH', a, b) for a, b in cells)

    hdr_size = 4 + 2 * 6 + 4 * 2 + 2 * 6 + 4 * 10
    parts = [model_blob, name_blob, stat_blob, anim_blob, grp_blob, pop_blob, zone_blob, cell_blob, node_blob, link_blob]
    offs, at = [], hdr_size
    for b in parts:
        offs.append(at)
        at += len(b)
    out = bytearray(struct.pack('<4sHHHHHH', b'DPED', VERSION, len(peds), len(stats), len(used_anims),
                                NUM_PEDGROUPS, len(zones)))
    out += struct.pack('<II', len(keys), nlinks)
    out += struct.pack('<hhHHHH', gx0, gy0, gw, gh, CELL, 0)
    out += struct.pack('<10I', *offs)
    assert len(out) == hdr_size, len(out)
    for b in parts:
        out += b
    st = dict(models=len(peds), stats=len(stats), anim_groups=len(used_anims), groups=sum(1 for g in groups if g),
              zones=len(zones), scripted=sum(1 for z in zones if z[8] in setup), nodes=len(keys), links=nlinks,
              grid=f'{gw}x{gh}', scm=f'{scm_range[0]}..{scm_range[1]}')
    return bytes(out), st
