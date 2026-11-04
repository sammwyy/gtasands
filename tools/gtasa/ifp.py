"""IFP animation packages (ANP3). Frames are normalised to the compressed int form:
quat * 4096, time * 60 (absolute), translation * 1024."""
import struct


class Seq:
    __slots__ = ('name', 'bone', 'has_trans', 'frames')


class Anim:
    __slots__ = ('name', 'seqs')


def parse(data):
    magic, _size, block, n = struct.unpack_from('<4sI24sI', data, 0)
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


def _s(b):
    return bytes(b).split(b'\0', 1)[0].decode('latin1')
