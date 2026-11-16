import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from . import ds_misc, img
from .test_portable_build import build_library

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which('cc'), 'C compiler unavailable')
class PortableCollisionTest(unittest.TestCase):
    def test_collision_formats_match_python(self):
        with tempfile.TemporaryDirectory() as tmp:
            library = Path(tmp) / 'libassets.so'
            build_library(library)
            files = [ROOT / 'game/models/coll' / name
                     for name in ('peds.col', 'vehicles.col', 'weapons.col')]
            if not any(path.is_file() for path in files):
                self.skipTest('local GTA installation unavailable')
            for path in files:
                if not path.is_file():
                    continue
                for mode in ('full', 'bounds', 'simple'):
                    if path.name == 'peds.col' and mode == 'simple':
                        continue
                    with self.subTest(file=path.name, mode=mode):
                        params = {'mode': mode, 'face_ratio': 0.5, 'min_faces': 16}
                        with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                            expected = ds_misc.convert_col(path.read_bytes(), params)
                        with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                            actual = ds_misc.convert_col(path.read_bytes(), params)
                        self.assertEqual(actual, expected)
            archive = ROOT / 'game/models/gta3.img'
            if archive.is_file():
                names = {'levelmap_1.col', 'seabed10.col', 'seabed12.col'}
                with archive.open('rb') as source:
                    for name, offset, size in img.read_toc(archive):
                        if name.lower() not in names:
                            continue
                        source.seek(offset)
                        data = source.read(size)
                        params = {'mode': 'simple', 'face_ratio': 0.5, 'min_faces': 16}
                        with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                            expected = ds_misc.convert_col(data, params)
                        with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                            self.assertEqual(ds_misc.convert_col(data, params), expected, name)


if __name__ == '__main__':
    unittest.main()
