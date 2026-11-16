import os
import shutil
import struct
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from . import img, world
from .test_portable_build import build_library

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which('cc'), 'C compiler unavailable')
class PortableIPLTest(unittest.TestCase):
    def test_binary_ipl_matches_python(self):
        data = b'bnry' + struct.pack('<I', 2) + bytes(20) + struct.pack('<I', 32)
        data += struct.pack('<7fiii', 1, -2, 3, 0, 0, 0, 1, 42, 0, -1)
        data += struct.pack('<7fiii', 8, 9, 10, 0.5, -0.5, 0, 0, 77, 4, 2)
        samples = [data, b'not bnry']
        archive = ROOT / 'game/models/gta3.img'
        if archive.is_file():
            entry = next(((name, off, size) for name, off, size in img.read_toc(archive)
                          if '_stream' in name and name.endswith('.ipl')), None)
            if entry:
                with archive.open('rb') as source:
                    source.seek(entry[1])
                    samples.append(source.read(entry[2]))
        with tempfile.TemporaryDirectory() as tmp:
            library = Path(tmp) / 'libassets.so'
            build_library(library)
            for sample in samples:
                with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                    expected = world.parse_ipl_binary(sample)
                with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                    self.assertEqual(world.parse_ipl_binary(sample), expected)
            root = ROOT / 'game'
            dat = root / 'data/gta.dat'
            if dat.is_file():
                for line in dat.read_text(errors='replace').splitlines():
                    if not line.upper().startswith('IPL '):
                        continue
                    path = world.resolve(root, line[4:].strip())
                    if not path or path.suffix.lower() != '.ipl':
                        continue
                    with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                        expected = world.parse_ipl_text(path)
                    with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                        self.assertEqual(world.parse_ipl_text(path), expected, str(path))
            for quat in ((0, 0, 0, 1), (0.1, -0.3, 0.5, 0.7), (0, 0, 0, 0)):
                with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                    expected = world.rotation(*quat)
                with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                    self.assertEqual(world.rotation(*quat), expected)


if __name__ == '__main__':
    unittest.main()
