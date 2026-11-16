import os
import shutil
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from . import img, world
from .test_portable_build import build_library

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which('cc'), 'C compiler unavailable')
class PortableDFFTest(unittest.TestCase):
    def test_model_radius_matches_python(self):
        archive = ROOT / 'game/models/gta3.img'
        if not archive.is_file():
            self.skipTest('local GTA installation unavailable')
        with tempfile.TemporaryDirectory() as tmp:
            library = Path(tmp) / 'libassets.so'
            build_library(library)
            entries = [(off, size) for name, off, size in img.read_toc(archive)
                       if name.lower().endswith('.dff')]
            for off, size in entries[:120]:
                with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                    expected = world._radius(archive, off, size)
                with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                    self.assertEqual(world._radius(archive, off, size), expected)


if __name__ == '__main__':
    unittest.main()
