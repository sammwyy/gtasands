import os
import shutil
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from . import ds_mesh
from .test_portable_build import build_library


@unittest.skipUnless(shutil.which('cc'), 'C compiler unavailable')
class PortableQEMTest(unittest.TestCase):
    def test_qem_matches_python(self):
        vertices = [(float(x), float(y), (x * y % 5) * 0.2)
                    for y in range(8) for x in range(8)]
        faces = []
        for y in range(7):
            for x in range(7):
                a = y * 8 + x
                faces.extend(((a, a + 1, a + 8, x % 3),
                              (a + 1, a + 9, a + 8, x % 3)))
        with tempfile.TemporaryDirectory() as tmp:
            library = Path(tmp) / 'libassets.so'
            build_library(library)
            for bones in (None, [i // 8 % 3 for i in range(len(vertices))]):
                for target in (10, 30, 60, len(faces)):
                    with self.subTest(bones=bones is not None, target=target):
                        with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                            expected = ds_mesh.decimate_qem(vertices, faces, target, bones)
                        with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                            self.assertEqual(ds_mesh.decimate_qem(vertices, faces, target, bones),
                                             expected)


if __name__ == '__main__':
    unittest.main()
