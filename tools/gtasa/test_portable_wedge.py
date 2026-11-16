import os
import shutil
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from . import ds_mesh, img, portable
from .test_portable_build import build_library

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which('cc'), 'C compiler unavailable')
class PortableWedgeTest(unittest.TestCase):
    def test_welded_decimation_matches_python(self):
        vertices = [(x * 0.5, y * 0.5, 0.1 * ((x + y) % 3))
                    for y in range(12) for x in range(12)]
        faces = []
        for y in range(11):
            for x in range(11):
                a = y * 12 + x
                faces.extend(((a, a + 1, a + 12, 0), (a + 1, a + 13, a + 12, 0)))
        with tempfile.TemporaryDirectory() as tmp:
            library = Path(tmp) / 'libassets.so'
            build_library(library)
            for target in (30, 100, len(faces)):
                with self.subTest(target=target):
                    with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                        expected = ds_mesh.decimate_wedge(vertices, faces, target)
                    with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                        self.assertEqual(ds_mesh.decimate_wedge(vertices, faces, target),
                                         expected)

    def test_ground_mesh_uses_native_wedge_and_matches_python(self):
        archive = ROOT / 'game/models/gta3.img'
        if not archive.is_file():
            self.skipTest('local GTA installation unavailable')
        entry = next((o, s) for n, o, s in img.read_toc(archive)
                     if n == 'sbseabed_08_sfse.dff')
        with archive.open('rb') as source:
            source.seek(entry[0])
            data = source.read(entry[1])
        params = {**ds_mesh.DEFAULTS, 'ratio': 0.37, 'max_tris': 120,
                  'ground_qem': True}
        with tempfile.TemporaryDirectory() as tmp:
            library = Path(tmp) / 'libassets.so'
            build_library(library)
            with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                expected = ds_mesh.convert_dff(data, params)
            with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                self.assertIsNotNone(portable.convert_model(data, params))
                self.assertEqual(ds_mesh.convert_dff(data, params), expected)


if __name__ == '__main__':
    unittest.main()
