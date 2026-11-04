"""vehicles.ide + handling.cfg + carcols.dat + surface.dat/surfinfo.dat -> DVEH.

CFileLoader::LoadVehicleObject, cHandlingDataMgr::LoadHandlingData (+
ConvertDataToGameUnits), CVehicleModelInfo::LoadVehicleColours and
SurfaceInfos_c::LoadAdhesiveLimits / LoadSurfaceInfos run here, so the DS
reads one table of fixed records (handling already in game units: speeds per
1/50 s step, accelerations per step^2). Layout: src/assets/ds_formats.h.
"""
import struct
from pathlib import Path

from .dsfmt import name as fname
from .world import resolve

VERSION = 1
MAX_COLOURS = 8

# CFileLoader::LoadVehicleObject type names -> eVehicleType
VEH_TYPES = ['car', 'mtruck', 'quad', 'heli', 'plane', 'boat', 'train', 'f_heli', 'f_plane', 'bike', 'bmx', 'trailer']
ADHESION_GROUPS = ['RUBBER', 'HARD', 'ROAD', 'LOOSE', 'SAND', 'WET']
SKIDMARKS = ['DEFAULT', 'SANDY', 'MUDDY']

ACCEL_CONST = 1.0 / (50.0 * 50.0)
VELOCITY_CONST = 0.277778 / 50.0


def _lines(path):
    for line in path.read_text(errors='replace').splitlines():
        line = line.split('#', 1)[0].strip()
        if line:
            yield line


def parse_ide(path):
    """cars section: id, model, txd, type, handlingId, gameName, anims, class, freq, flags, comprules
    [, wheelModel, wheelScaleF, wheelScaleR (cars) | steerAngle, wheelScale (bikes)]."""
    out, sec = [], None
    for line in _lines(path):
        low = line.lower()
        if ',' not in line:
            sec = None if low == 'end' else low
            continue
        if sec != 'cars':
            continue
        a = [x.strip() for x in line.split(',')]
        if len(a) < 11:
            continue
        try:
            vt = a[3].lower()
            wf = wr = 0.7
            if vt in ('car', 'mtruck', 'quad', 'trailer') and len(a) >= 14:
                wf, wr = float(a[12]), float(a[13])
            elif vt in ('bike', 'bmx') and len(a) >= 13:
                wf = wr = float(a[12])
            out.append(dict(id=int(a[0]), model=a[1].lower(), txd=a[2].lower(),
                            type=VEH_TYPES.index(vt) if vt in VEH_TYPES else 0xFF,
                            handling=a[4].upper(), gxt=a[5], anims=a[6].lower(), flags=int(a[9], 0) if a[9] else 0,
                            wheelF=wf, wheelR=wr))
        except ValueError:
            continue
    return out


def parse_handling(path):
    """tHandlingData::InitFromData + cHandlingDataMgr::ConvertDataToGameUnits (standard lines only)."""
    out = {}
    for line in path.read_text(errors='replace').splitlines():
        line = line.split(';', 1)[0].strip()
        if not line or line[0] in '!$%^':
            continue
        a = line.split()
        if len(a) < 36:
            continue
        try:
            v = dict(
                mass=float(a[1]), turnMass=float(a[2]), drag=float(a[3]),
                com=(float(a[4]), float(a[5]), float(a[6])), submerged=int(a[7]),
                tractionMult=float(a[8]), tractionLoss=float(a[9]), tractionBias=float(a[10]),
                gears=int(a[11]), maxVel=float(a[12]), engineAccel=float(a[13]), engineInertia=float(a[14]),
                drive=a[15][0].upper(), engine=a[16][0].upper(),
                brakeDecel=float(a[17]), brakeBias=float(a[18]), abs=int(a[19]), steeringLock=float(a[20]),
                suspForce=float(a[21]), suspDamp=float(a[22]), suspHighSpd=float(a[23]),
                suspUpper=float(a[24]), suspLower=float(a[25]), suspBias=float(a[26]), antiDive=float(a[27]),
                seatOffset=float(a[28]), damageMult=float(a[29]), value=int(float(a[30])),
                modelFlags=int(a[31], 16), handlingFlags=int(a[32], 16),
                frontLights=int(a[33]), rearLights=int(a[34]), animGroup=int(a[35]))
        except (ValueError, IndexError):
            continue
        # InitFromData: m_EngineAcceleration *= 0.4 before the conversion
        v['engineAccel'] *= 0.4
        _convert(v)
        out[a[0].upper()] = v
    return out


def _convert(h):
    """cHandlingDataMgr::ConvertDataToGameUnits (+ the max flat / reverse velocity search)."""
    h['engineAccel'] *= ACCEL_CONST
    h['maxVel'] *= VELOCITY_CONST
    h['brakeDecel'] *= ACCEL_CONST
    h['damageMult'] *= 2000.0 / h['mass']
    limit = h['engineAccel'] / 6.0
    v = h['maxVel']
    while v > 0.0:
        v -= 0.01
        if h['drag'] >= 0.01:
            if v * v * (h['drag'] / 2.0) / 1000.0 <= limit:
                break
            continue
        if (1.0 / (v * v * h['drag'] + 1.0) - 1.0) * -v <= limit:
            break
    use_limit = bool(h['handlingFlags'] & 0x01000000)   # USE_MAXSP_LIMIT (7th digit, 1)
    if use_limit:
        h['maxFlat'] = v / 1.2
        h['maxRev'] = min(-h['maxFlat'] / 4.0, -0.2)
    else:
        h['maxVel'] = v * 1.2
        h['maxFlat'] = v
        h['maxRev'] = min(-v * 0.3, -0.2)
    h['engineAccel'] /= 4.0 if h['drive'] == '4' else 2.0


def parse_carcols(path):
    """CVehicleModelInfo::LoadVehicleColours: col table + car (2 colour) / car4 (first two used)."""
    table, cars, sec = [], {}, None
    for line in path.read_text(errors='replace').splitlines():
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        low = line.lower()
        if low in ('col', 'car', 'car4', 'end'):
            sec = None if low == 'end' else low
            continue
        if sec == 'col':
            p = [x for x in line.replace(',', ' ').replace('.', ' ').split() if x]
            if len(p) >= 3:
                table.append(tuple(min(255, int(x)) for x in p[:3]))
        elif sec in ('car', 'car4'):
            p = [x.strip() for x in line.split(',') if x.strip()]
            nums = [int(x) for x in p[1:] if x.isdigit()]
            step = 2 if sec == 'car' else 4
            pairs = [(nums[i], nums[i + 1]) for i in range(0, len(nums) - step + 1, step)]
            if pairs:
                cars[p[0].lower()] = pairs[:MAX_COLOURS]
    return table, cars


def parse_surfaces(surface_dat, surfinfo_dat):
    """Adhesive limit matrix (lower half mirrored) + per surface (adhesion group, wet grip %, skidmark)."""
    lim = [[0.0] * 6 for _ in range(6)]
    row = 0
    for line in surface_dat.read_text(errors='replace').splitlines():
        line = line.strip()
        if not line or line.startswith(';') or row >= 6:
            continue
        p = line.split()[1:]
        for col in range(row + 1):
            v = 0.0 if col >= len(p) or p[col] == '-' else float(p[col])
            lim[row][col] = lim[col][row] = v
        row += 1
    surf = []
    for line in surfinfo_dat.read_text(errors='replace').splitlines():
        line = line.split('#', 1)[0].strip()
        p = line.split()
        if len(p) < 5:
            continue
        g = ADHESION_GROUPS.index(p[1].upper()) if p[1].upper() in ADHESION_GROUPS else 2
        try:
            wet = round(float(p[3]) * 100.0)
        except ValueError:
            wet = 0
        sk = SKIDMARKS.index(p[4].upper()) if p[4].upper() in SKIDMARKS else 0
        surf.append((g, max(-128, min(127, wet)), sk))
    return lim, surf


def build(game, sec):
    game = Path(game)
    paths = {k: resolve(game, sec.get(k, d)) for k, d in (
        ('ide', 'data/vehicles.ide'), ('handling', 'data/handling.cfg'), ('carcols', 'data/carcols.dat'),
        ('surface', 'data/surface.dat'), ('surfinfo', 'data/surfinfo.dat'))}
    missing = [k for k, p in paths.items() if not p]
    if missing:
        raise FileNotFoundError(f'vehicles: missing {missing} (run: tools/assets.py sync)')
    ide = parse_ide(paths['ide'])
    hand = parse_handling(paths['handling'])
    table, cars = parse_carcols(paths['carcols'])
    lim, surf = parse_surfaces(paths['surface'], paths['surfinfo'])

    recs = bytearray()
    no_handling = 0
    for v in ide:
        h = hand.get(v['handling'])
        if h is None:
            no_handling += 1
            h = hand.get('LANDSTAL') or next(iter(hand.values()))
        cols = cars.get(v['model'], [(1, 1)])
        cp = bytearray()
        for i in range(MAX_COLOURS):
            a, b = cols[i] if i < len(cols) else (0, 0)
            cp += struct.pack('<BB', min(255, a), min(255, b))
        recs += fname(v['model'], 24) + fname(v['txd'], 24) + fname(v['gxt'], 8) + fname(v['anims'], 16)
        recs += struct.pack('<HBBI', v['id'], v['type'], len(cols), v['flags'])
        recs += cp
        recs += struct.pack('<2f', v['wheelF'], v['wheelR'])
        recs += struct.pack('<6f', h['mass'], h['turnMass'], h['drag'], *h['com'])
        recs += struct.pack('<3f', h['tractionMult'], h['tractionLoss'], h['tractionBias'])
        recs += struct.pack('<5f', h['maxVel'], h['maxFlat'], h['maxRev'], h['engineAccel'], h['engineInertia'])
        recs += struct.pack('<4B', h['gears'], ord(h['drive']), ord(h['engine']), h['abs'])
        recs += struct.pack('<3f', h['brakeDecel'], h['brakeBias'], h['steeringLock'])
        recs += struct.pack('<7f', h['suspForce'], h['suspDamp'], h['suspHighSpd'], h['suspUpper'], h['suspLower'],
                            h['suspBias'], h['antiDive'])
        recs += struct.pack('<2f', h['seatOffset'], h['damageMult'])
        recs += struct.pack('<3I', h['modelFlags'] & 0xFFFFFFFF, h['handlingFlags'] & 0xFFFFFFFF, max(0, h['value']))
        recs += struct.pack('<4B', h['frontLights'], h['rearLights'], h['animGroup'], h['submerged'] & 0xFF)
    rec_size = len(recs) // max(1, len(ide))

    hdr_size = 24 + 36 * 4
    veh_off = hdr_size
    col_off = veh_off + len(recs)
    surf_off = col_off + 4 * len(table)
    out = bytearray(struct.pack('<4sHHHHIII', b'DVEH', VERSION, len(ide), len(table), len(surf),
                                veh_off, col_off, surf_off))
    for r in lim:
        out += struct.pack('<6f', *r)
    assert len(out) == hdr_size
    out += recs
    for r, g, b in table:
        out += struct.pack('<4B', r, g, b, 0)
    for g, wet, sk in surf:
        out += struct.pack('<BbBB', g, wet, sk, 0)
    stats = dict(vehicles=len(ide), rec=rec_size, colours=len(table), surfaces=len(surf), no_handling=no_handling)
    return bytes(out), stats
