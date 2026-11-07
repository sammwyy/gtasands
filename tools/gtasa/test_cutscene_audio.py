import tempfile
import unittest
from pathlib import Path

from .audio import KEY, TRACK_HEADER, aligned_track_offset


class CutsceneAudioTest(unittest.TestCase):
    def test_recovers_shifted_cutscene_track_start(self):
        with tempfile.TemporaryDirectory() as temp:
            expected, actual = 6400, 6600
            raw = bytearray(actual + TRACK_HEADER + 4)
            plain = b'\xff\xff\xff\xff\x00\x00\x00\x00' * 2
            for i, c in enumerate(plain):
                raw[actual + i] = c ^ KEY[(actual + i) & 15]
            for i, c in enumerate(b'OggS'):
                at = actual + TRACK_HEADER + i
                raw[at] = c ^ KEY[at & 15]
            path = Path(temp) / 'CUTSCENE'
            path.write_bytes(raw)
            self.assertEqual(aligned_track_offset(path, expected, 256), actual)


if __name__ == '__main__':
    unittest.main()
