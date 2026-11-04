"""RenderWare binary stream chunks."""
import struct

STRUCT = 0x01
STRING = 0x02
EXTENSION = 0x03
TEXTURE = 0x06
MATERIAL = 0x07
MATLIST = 0x08
FRAMELIST = 0x0E
GEOMETRY = 0x0F
CLUMP = 0x10
ATOMIC = 0x14
TEXNATIVE = 0x15
TEXDICT = 0x16
GEOMETRYLIST = 0x1A
SKIN = 0x116
HANIM = 0x11E
FRAME_NAME = 0x253F2FE


def chunks(data, start=0, end=None):
    """Yields (type, body_start, body_end, version) for sibling chunks."""
    end = len(data) if end is None else end
    o = start
    while o + 12 <= end:
        t, s, v = struct.unpack_from('<III', data, o)
        yield t, o + 12, min(o + 12 + s, end), v
        o += 12 + s


def find(data, start, end, typ):
    for t, s, e, _ in chunks(data, start, end):
        if t == typ:
            return s, e
    return None


def find_all(data, start, end, typ):
    return [(s, e) for t, s, e, _ in chunks(data, start, end) if t == typ]


def cstr(b):
    return bytes(b).split(b'\0', 1)[0].decode('latin1')


def string(data, s, e):
    return cstr(data[s:e])
