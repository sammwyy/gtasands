"""SA audio: SFX paks (raw PCM16 banks) and encrypted OGG stream packs -> DS sound blobs."""
import os
import struct
import subprocess
from array import array

from .dsfmt import pad4

KEY = bytes([0xEA, 0x3A, 0xC4, 0xA1, 0x9A, 0xA8, 0x14, 0xF3, 0x48, 0xB0, 0xD7, 0x23, 0x9D, 0xE8, 0xFF, 0xF1])
BANK_HEADER = 0x12C4
TRACK_HEADER = 0x1F84
FMT = {'pcm8': 0, 'pcm16': 1, 'adpcm': 2}

DEFAULTS = dict(sample_rate=11025, format='adpcm', max_seconds=0, volume=1.0)

STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
        107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
        876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
        5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
        27086, 29794, 32767]
IDX = [-1, -1, -1, -1, 2, 4, 6, 8]


def read_names(path, size):
    d = open(path, 'rb').read()
    return [d[i:i + size].split(b'\0', 1)[0].decode('latin1') for i in range(0, len(d) - size + 1, size)]


def read_lookup(path):
    """BankLkup.dat / TrakLkup.dat: [(pack, offset, size)]."""
    d = open(path, 'rb').read()
    return [struct.unpack_from('<B3xII', d, i) for i in range(0, len(d) - 11, 12)]


def sfx_tables(cfg_dir):
    return read_names(os.path.join(cfg_dir, 'PakFiles.dat'), 52), read_lookup(os.path.join(cfg_dir, 'BankLkup.dat'))


def stream_tables(cfg_dir):
    return read_names(os.path.join(cfg_dir, 'StrmPaks.dat'), 16), read_lookup(os.path.join(cfg_dir, 'TrakLkup.dat'))


def decrypt(buf, pos):
    k = (KEY * (len(buf) // 16 + 2))[pos & 15:]
    return bytes(a ^ b for a, b in zip(buf, k))

def aligned_track_offset(path, expected, window=32768):
    start = max(0, expected - window)
    with open(path, 'rb') as f:
        f.seek(start)
        raw = f.read(expected + window + TRACK_HEADER + 4 - start)
    dec = decrypt(raw, start)
    signature = b'\xff\xff\xff\xff\x00\x00\x00\x00' * 2
    found = []
    pos = 0
    while True:
        pos = dec.find(signature, pos)
        if pos < 0:
            break
        if dec[pos + TRACK_HEADER:pos + TRACK_HEADER + 4] == b'OggS':
            found.append(start + pos)
        pos += 8
    if not found:
        raise ValueError(f'no valid OGG track near offset {expected}')
    return min(found, key=lambda x: abs(x - expected))


def resample(s, sr, dr):
    if dr >= sr or not s:
        return s, sr
    r = sr / dr
    n = int(len(s) / r)
    out = array('h', bytes(n * 2))
    for i in range(n):
        a = int(i * r)
        b = max(a + 1, int((i + 1) * r))
        seg = s[a:b]
        out[i] = sum(seg) // len(seg)
    return out, dr


def adpcm_encode(s, checkpoints=None):
    pred = s[0] if s else 0
    idx = 0
    out = bytearray(struct.pack('<hBB', pred, 0, 0))
    lo = -1
    for i, x in enumerate(s):
        if checkpoints is not None and i % 8192 == 0:
            checkpoints.extend(struct.pack('<hBB', pred, idx, 0))
        step = STEP[idx]
        d = x - pred
        code = 0
        if d < 0:
            code, d = 8, -d
        vp = step >> 3
        if d >= step: code |= 4; d -= step; vp += step
        step >>= 1
        if d >= step: code |= 2; d -= step; vp += step
        step >>= 1
        if d >= step: code |= 1; vp += step
        pred = max(-32767, min(32767, pred - vp if code & 8 else pred + vp))
        idx = max(0, min(88, idx + IDX[code & 7]))
        if lo < 0:
            lo = code
        else:
            out.append(lo | code << 4)
            lo = -1
    if lo >= 0:
        out.append(lo)
    return bytes(out)


def make_sound(pcm, rate, p, loop=-1, headroom=0):
    """PCM16 mono array -> DSND blob."""
    if p['volume'] != 1.0:
        v = p['volume']
        pcm = array('h', (max(-32768, min(32767, int(x * v))) for x in pcm))
    pcm, new_rate = resample(pcm, rate, p['sample_rate'])
    if loop >= 0:
        loop = int(loop * new_rate / rate)
    if p['max_seconds']:
        pcm = pcm[:int(p['max_seconds'] * new_rate)]
    fmt = FMT[p['format']]
    if fmt == 0:
        data = bytes((x >> 8) & 0xFF for x in pcm)
    elif fmt == 1:
        data = pcm.tobytes()
    else:
        data = adpcm_encode(pcm, p.get('checkpoints'))
    hdr = struct.pack('<4sBBHIihHI', b'DSND', fmt, 0, new_rate, len(pcm), loop if loop < len(pcm) else -1, headroom, 0, len(data))
    return hdr + pad4(data) + (p.get('checkpoints') or b'')


def convert_bank(raw, p):
    """raw = bank header (0x12C4) + PCM16 data. Returns DSBK blob."""
    n, = struct.unpack_from('<h', raw, 0)
    n = 1 if n < 0 else n
    items = [struct.unpack_from('<IiHh', raw, 4 + i * 12) for i in range(n)]
    data_len = len(raw) - BANK_HEADER
    sounds = []
    for i, (off, loop, rate, head) in enumerate(items):
        end = items[i + 1][0] if i + 1 < n else data_len
        seg = raw[BANK_HEADER + off:BANK_HEADER + min(end, data_len)]
        pcm = array('h', seg[:len(seg) & ~1])
        sounds.append(make_sound(pcm, rate or 22050, p, loop, head))
    out = bytearray(struct.pack('<4sHH', b'DSBK', 1, len(sounds)))
    off = len(out) + 4 * len(sounds)
    for s in sounds:
        out += struct.pack('<I', off)
        off += len(s)
    for s in sounds:
        out += s
    return bytes(out), {'sounds': len(sounds), 'bytes_in': len(raw)}


def convert_track(enc, file_pos, p):
    """enc = encrypted bytes from file_pos covering tTrackInfo + OGG."""
    ogg = decrypt(enc, file_pos)[TRACK_HEADER:]
    r = subprocess.run(['ffmpeg', '-v', 'error', '-i', 'pipe:0', '-f', 's16le', '-ac', '1', '-ar', str(p['sample_rate']), 'pipe:1'],
                       input=ogg, capture_output=True)
    if r.returncode:
        raise RuntimeError(r.stderr.decode(errors='replace').strip()[:200])
    pcm = array('h', r.stdout[:len(r.stdout) & ~1])
    if p.get('seekable'):
        p = {**p, 'checkpoints': bytearray()}
    return make_sound(pcm, p['sample_rate'], p), {'bytes_in': len(enc)}


def pack_streams(items, seekable=False):
    items = sorted(items)
    blob = bytearray(struct.pack('<4sHH', b'DSST', 2 if seekable else 1, len(items)))
    off = len(blob) + (20 if seekable else 12) * len(items)
    for ti, b in items:
        if seekable:
            data_size, = struct.unpack_from('<I', b, 20)
            seek_at = 24 + ((data_size + 3) & ~3)
            blob += struct.pack('<IIIII', ti, off, len(b), off + seek_at, (len(b) - seek_at) // 4)
        else:
            blob += struct.pack('<III', ti, off, len(b))
        off += len(b)
    for _, b in items:
        blob += b
    return bytes(blob)
