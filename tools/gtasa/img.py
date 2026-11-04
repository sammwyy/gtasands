"""IMG v2 ("VER2") archives."""
import struct

SECTOR = 2048


def read_toc(path):
    """Returns [(name, byte_offset, byte_size)]."""
    with open(path, 'rb') as f:
        magic, n = struct.unpack('<4sI', f.read(8))
        if magic != b'VER2':
            raise ValueError(f'{path}: not an IMG v2 archive')
        toc = f.read(n * 32)
    out = []
    for i in range(n):
        off, ssz, asz, name = struct.unpack_from('<IHH24s', toc, i * 32)
        size = (asz or ssz) * SECTOR
        out.append((name.split(b'\0', 1)[0].decode('latin1'), off * SECTOR, size))
    return out


def read_entry(path, offset, size):
    with open(path, 'rb') as f:
        f.seek(offset)
        return f.read(size)
