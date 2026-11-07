import struct
import unittest

from . import ifp
from .ds_misc import ANIM_DEFAULTS, convert_ifp


def chunk(tag, data):
    return tag + struct.pack('<I', len(data)) + data + b'\0' * (-len(data) % 4)


class AnpkTest(unittest.TestCase):
    def test_cutscene_frames_and_streamable_clip(self):
        frames = (struct.pack('<4f3ff', 0, 0, 0, 1, 1, 2, 3, 0)
                  + struct.pack('<4f3ff', 0, 0, 0, 1, 2, 3, 4, 0.5))
        seq = chunk(b'CPAN', chunk(b'ANIM', b'root'.ljust(28, b'\0')
                                            + struct.pack('<4I', 2, 0, 0, 0))
                    + chunk(b'KRT0', frames))
        data = chunk(b'ANPK', chunk(b'INFO', struct.pack('<I', 1) + b'PROLOG3\0')
                     + chunk(b'NAME', b'csplay\0')
                     + chunk(b'DGAN', chunk(b'INFO', struct.pack('<II', 1, 0)) + seq))
        block, anims = ifp.parse(data)
        self.assertEqual(block, 'PROLOG3')
        self.assertEqual(anims[0].name, 'csplay')
        self.assertEqual(anims[0].seqs[0].frames,
                         [(0, 0, 0, 4096, 0, 1024, 2048, 3072),
                          (0, 0, 0, 4096, 30, 2048, 3072, 4096)])
        converted, stats = convert_ifp(data, ANIM_DEFAULTS)
        self.assertEqual(converted[:4], b'DANM')
        self.assertEqual(stats['keys_out'], 2)


if __name__ == '__main__':
    unittest.main()
