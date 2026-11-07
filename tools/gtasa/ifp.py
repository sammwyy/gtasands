"""IFP animation packages (ANP3/ANPK). Frames are normalised to the compressed int form:
quat * 4096, time * 60 (absolute), translation * 1024."""
import math
import struct


class Seq:
    __slots__ = ('name', 'bone', 'has_trans', 'frames')


class Anim:
    __slots__ = ('name', 'seqs')


def parse(data):
    magic, _size, block, n = struct.unpack_from('<4sI24sI', data, 0)
    if magic == b'ANPK':
        return _parse_anpk(data)
    if magic != b'ANP3':
        raise ValueError(f'unsupported IFP {magic!r}')
    o = 36
    anims = []
    for _ in range(n):
        a = Anim()
        a.name = _s(data[o:o + 24])
        nseq, _fsize, _flags = struct.unpack_from('<3I', data, o + 24)
        o += 36
        a.seqs = []
        for _ in range(nseq):
            s = Seq()
            s.name = _s(data[o:o + 24])
            ftype, nfr, s.bone = struct.unpack_from('<IIi', data, o + 24)
            o += 36
            s.has_trans = ftype in (2, 4)
            s.frames = []
            if ftype in (3, 4):
                k = 8 if ftype == 4 else 5
                v = struct.unpack_from(f'<{nfr * k}h', data, o)
                s.frames = [v[i:i + k] for i in range(0, len(v), k)]
                o += nfr * k * 2
            else:
                k = 8 if ftype == 2 else 5
                v = struct.unpack_from(f'<{nfr * k}f', data, o)
                for i in range(0, len(v), k):
                    f = v[i:i + k]
                    q = [round(x * 4096) for x in f[:4]] + [round(f[4] * 60)]
                    if ftype == 2:
                        q += [round(x * 1024) for x in f[5:8]]
                    s.frames.append(tuple(max(-32768, min(32767, x)) for x in q))
                o += nfr * k * 4
            a.seqs.append(s)
        anims.append(a)
    return _s(block), anims


def _parse_anpk(data):
    def section(offset, tag):
        kind, size = struct.unpack_from('<4sI', data, offset)
        if kind != tag or offset + 8 + size > len(data):
            raise ValueError(f'invalid {tag!r} at {offset}')
        return data[offset + 8:offset + 8 + size], offset + 8 + (size + 3 & ~3)

    info, o = section(8, b'INFO')
    count = struct.unpack_from('<I', info)[0]
    block = _s(info[4:])
    anims = []
    for _ in range(count):
        label, o = section(o, b'NAME')
        kind, size = struct.unpack_from('<4sI', data, o)
        if kind != b'DGAN' or o + 8 + size > len(data):
            raise ValueError('invalid DGAN')
        end = o + 8 + (size + 3 & ~3)
        details, o = section(o + 8, b'INFO')
        numseq = struct.unpack_from('<I', details)[0]
        a = Anim()
        a.name = _s(label)
        a.seqs = []
        for _ in range(numseq):
            tag, size = struct.unpack_from('<4sI', data, o)
            if tag != b'CPAN' or o + 8 + size > end:
                raise ValueError('invalid CPAN')
            o += 8
            desc, o = section(o, b'ANIM')
            name = _s(desc[:28])
            numframes = struct.unpack_from('<I', desc, 28)[0]
            bone = struct.unpack_from('<i', desc, 40)[0] if len(desc) >= 44 else -1
            s = Seq()
            s.name, s.bone = name, bone
            s.frames = []
            s.has_trans = False
            if numframes:
                tag, size = struct.unpack_from('<4sI', data, o)
                if tag not in (b'KR00', b'KRT0', b'KRTS'):
                    raise ValueError(f'unsupported frame type {tag!r}')
                payload, o = section(o, tag)
                stride = {b'KR00': 20, b'KRT0': 32, b'KRTS': 44}[tag]
                if len(payload) < numframes * stride:
                    raise ValueError('truncated animation frames')
                s.has_trans = tag != b'KR00'
                for i in range(numframes):
                    q = struct.unpack_from('<4f', payload, i * stride)
                    pos = struct.unpack_from('<3f', payload, i * stride + 16) if s.has_trans else ()
                    t = struct.unpack_from('<f', payload, i * stride + stride - 4)[0]
                    if not math.isfinite(t) or t < 0:
                        raise ValueError('invalid animation timestamp')
                    raw = [round(v * (4096 if j == 3 else -4096)) if math.isfinite(v) else 0
                           for j, v in enumerate(q)]
                    raw.append(round(t * 60))
                    raw.extend(round(v * 1024) if math.isfinite(v) and abs(v) <= 512 else 0 for v in pos)
                    s.frames.append(tuple(max(-32768, min(32767, x)) for x in raw[:5]) + tuple(raw[5:]))
            a.seqs.append(s)
        if o > end:
            raise ValueError('animation exceeds DGAN')
        o = end
        anims.append(a)
    return block, anims


def _s(b):
    return bytes(b).split(b'\0', 1)[0].decode('latin1')
