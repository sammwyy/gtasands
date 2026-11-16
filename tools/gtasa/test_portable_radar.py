import os
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from . import ds_radar, img
from .test_portable_build import build_library

ROOT = Path(__file__).resolve().parents[2]


def fixture():
    rgba = bytes(v for y in range(128) for x in range(128)
                 for v in ((x + y) % 256, x % 256, y % 256, 255))
    native = (struct.pack('<I', 9) + b'\0' * 4 + b'radar0\0'.ljust(32, b'\0')
              + b'\0' * 32 + struct.pack('<IIHHBBBB', 0x500, 0, 128, 128, 32, 1, 0, 1)
              + struct.pack('<I', len(rgba)) + rgba)

    def chunk(kind, data):
        return struct.pack('<III', kind, len(data), 0) + data

    return chunk(0x16, chunk(0x15, chunk(1, native)))


@unittest.skipUnless(shutil.which('cc'), 'C compiler unavailable')
class PortableRadarTest(unittest.TestCase):
    def test_radar_matches_python(self):
        with tempfile.TemporaryDirectory() as tmp:
            library = Path(tmp) / 'libassets.so'
            build_library(library)
            samples = [fixture()]
            archive = ROOT / 'game/models/gta3.img'
            if archive.exists():
                entries = [(off, size) for name, off, size in img.read_toc(archive)
                           if name.lower().startswith('radar') and name.lower().endswith('.txd')]
                with archive.open('rb') as source:
                    for off, size in entries[:3]:
                        source.seek(off)
                        samples.append(source.read(size))
            for data in samples:
                with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                    expected, _ = ds_radar.convert_radar(data, {})
                with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                    actual, _ = ds_radar.convert_radar(data, {})
                self.assertEqual(actual, expected)


if __name__ == '__main__':
    unittest.main()
