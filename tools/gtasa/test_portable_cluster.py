import os
import random
import shutil
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from . import ds_mesh
from .test_portable_build import build_library


@unittest.skipUnless(shutil.which('cc'), 'C compiler unavailable')
class PortableClusterTest(unittest.TestCase):
    def test_decimation_matches_python(self):
        randomizer = random.Random(47)
        verts = [(randomizer.uniform(-30, 30), randomizer.uniform(-40, 40),
                  randomizer.uniform(-10, 10)) for _ in range(160)]
        tris = [(randomizer.randrange(len(verts)), randomizer.randrange(len(verts)),
                 randomizer.randrange(len(verts)), randomizer.randrange(4))
                for _ in range(500)]
        with tempfile.TemporaryDirectory() as tmp:
            library = Path(tmp) / 'libassets.so'
            build_library(library)
            for target in (0, 10, 60, 250, len(tris)):
                with self.subTest(target=target):
                    with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                        expected = ds_mesh.decimate(verts, tris, target)
                    with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                        self.assertEqual(ds_mesh.decimate(verts, tris, target), expected)


if __name__ == '__main__':
    unittest.main()
