#!/usr/bin/env python3
"""Levels of a DEBUG mixer capture (sdcard/gtasads_aud_*.wav, see AUDTEST).

    python3 tools/audwav.py sdcard/gtasads_aud_run.wav [--ms 100] [--png out.png]

Prints peak / RMS (dBFS) per channel overall and per window, the number of
transients (window RMS jumping > 6 dB over the previous one) and clipped
samples; --png draws the waveform (L top, R bottom) for a quick look.
"""
import argparse
import math
import struct
import sys
import zlib


def read(path):
    d = open(path, 'rb').read()
    if d[:4] != b'RIFF' or d[8:12] != b'WAVE':
        sys.exit('not a wav')
    rate = struct.unpack_from('<I', d, 24)[0]
    i = 12
    while i < len(d) - 8:
        cid, n = struct.unpack_from('<4sI', d, i)
        if cid == b'data':
            pcm = struct.unpack_from('<%dh' % (n // 2), d, i + 8)
            return rate, pcm[0::2], pcm[1::2]
        i += 8 + n
    sys.exit('no data')


def db(v):
    return 20 * math.log10(v / 32768) if v > 0 else -120.0


def rms(x):
    return math.sqrt(sum(v * v for v in x) / len(x)) if x else 0.0


def png(path, l, r, w=800, h=200):
    rows = []
    half = h // 2
    for y in range(h):
        rows.append(bytearray(b'\x10' * w))
    for ch, sig, top in ((0, l, 0), (1, r, half)):
        n = len(sig)
        for x in range(w):
            seg = sig[x * n // w:(x + 1) * n // w] or [0]
            lo, hi = min(seg), max(seg)
            y0 = top + half // 2 - hi * (half // 2) // 32768
            y1 = top + half // 2 - lo * (half // 2) // 32768
            for y in range(max(top, y0), min(top + half, y1 + 1)):
                rows[y][x] = 0xE0
        for x in range(w):
            rows[top + half // 2][x] = 0x60
    raw = b''.join(b'\0' + bytes(row) for row in rows)
    def chunk(t, b):
        return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xFFFFFFFF)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 0, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('wav')
    ap.add_argument('--ms', type=int, default=100)
    ap.add_argument('--png')
    a = ap.parse_args()
    rate, l, r = read(a.wav)
    print(f'{a.wav}: {len(l)} samples @ {rate} Hz ({len(l) / rate:.2f} s)')
    for name, x in (('L', l), ('R', r)):
        peak = max(abs(v) for v in x) if x else 0
        clip = sum(1 for v in x if abs(v) >= 32767)
        print(f'  {name}: peak {db(peak):6.1f} dBFS  rms {db(rms(x)):6.1f} dBFS  clipped {clip}')
    win = max(1, rate * a.ms // 1000)
    line, prev, jumps = [], None, 0
    for i in range(0, len(l) - win + 1, win):
        v = db(rms(l[i:i + win] + r[i:i + win]))
        if prev is not None and v - prev > 6:
            jumps += 1
        prev = v
        line.append(f'{v:5.0f}')
    print(f'  rms per {a.ms} ms (L+R):', ' '.join(line))
    print(f'  transients (+6 dB jumps): {jumps}')
    if a.png:
        png(a.png, l, r)
        print(f'  waveform -> {a.png}')


if __name__ == '__main__':
    main()
