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

    def test_cutscene_root_translation_does_not_clamp_at_32_metres(self):
        frames = (struct.pack('<4f3ff', 0, 0, 0, 1, -100, 75, 3, 0)
                  + struct.pack('<4f3ff', 0, 0, 0, 1, -120, 80, 3, 1))
        seq = chunk(b'CPAN', chunk(b'ANIM', b'root'.ljust(28, b'\0')
                                            + struct.pack('<4I', 2, 0, 0, 0))
                    + chunk(b'KRT0', frames))
        data = chunk(b'ANPK', chunk(b'INFO', struct.pack('<I', 1) + b'PROLOG1\0')
                     + chunk(b'NAME', b'csplay\0')
                     + chunk(b'DGAN', chunk(b'INFO', struct.pack('<II', 1, 0)) + seq))
        self.assertEqual(ifp.parse(data)[1][0].seqs[0].frames[0][5], -102400)
        converted, _ = convert_ifp(data, ANIM_DEFAULTS)
        self.assertEqual(struct.unpack_from('<H', converted, 4)[0], 3)
        record = converted[32 + 32:]
        self.assertEqual(record[28 + 27], 2)
        self.assertEqual(struct.unpack_from('<h', record, 28 + 32 + 10)[0], -25600)

    def test_rigid_cutscene_seqs_use_hanim_order(self):
        frames = (struct.pack('<4f3ff', 0, 0, 0, 1, 1, 2, 3, 0)
                  + struct.pack('<4f3ff', 0, 0, 0, 1, 1, 2, 3, 1))
        seqs = b''.join(
            chunk(b'CPAN', chunk(b'ANIM', n.ljust(28, b'\0') + struct.pack('<3Ii', 2, 0, 0, -1))
                  + chunk(b'KRT0', frames))
            for n in (b'DummyRoot', b'cuff01'))
        data = chunk(b'ANPK', chunk(b'INFO', struct.pack('<I', 1) + b'PROLOG3\0')
                     + chunk(b'NAME', b'cshandcuff\0')
                     + chunk(b'DGAN', chunk(b'INFO', struct.pack('<II', 2, 0)) + seqs))
        converted, _ = convert_ifp(data, {**ANIM_DEFAULTS, 'rigid_clips': ['cshandcuff']})
        record = converted[64:]
        self.assertEqual(struct.unpack_from('<h', record, 28 + 24)[0], 0)
        self.assertEqual(struct.unpack_from('<h', record, 28 + 32 + 32 + 24)[0], 1)


if __name__ == '__main__':
    unittest.main()
