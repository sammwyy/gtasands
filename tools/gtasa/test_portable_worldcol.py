import os
import shutil
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from . import col, img, world
from .test_portable_build import build_library

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which('cc'), 'C compiler unavailable')
class PortableWorldCollisionTest(unittest.TestCase):
    def test_world_collision_matches_python(self):
        faces = [(i, i + 1, i + 2, i % 3, 0) for i in range(9)]
        vertices = [(i % 3 * 1.5, (i // 3) * 1.5, 0.25 * (i % 2))
                    for i in range(11)]
        sample = SimpleNamespace(verts=vertices, faces=faces,
                                 spheres=[((2, 3, 0), 2, (4, 0, 0, 1))],
                                 boxes=[((-1, 0, -1), (1, 2, 1), (3, 0, 0, 2))])
        cases = [sample]
        archive = ROOT / 'game/models/gta3.img'
        if archive.is_file():
            with archive.open('rb') as source:
                for name, offset, size in img.read_toc(archive):
                    if name not in ('levelmap_1.col', 'seabed1.col'):
                        continue
                    source.seek(offset)
                    cases.extend(col.parse(source.read(size)))
        with tempfile.TemporaryDirectory() as tmp:
            library = Path(tmp) / 'libassets.so'
            build_library(library)
            for c in cases:
                with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                    expected = world._col_record(c)
                with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                    self.assertEqual(world._col_record(c), expected)

    def test_world_col_archive_matches_python(self):
        archive = ROOT / 'game/models/gta3.img'
        if not archive.is_file():
            self.skipTest('local GTA installation unavailable')
        with tempfile.TemporaryDirectory() as tmp:
            library = Path(tmp) / 'libassets.so'
            build_library(library)
            with archive.open('rb') as source:
                for name, offset, size in img.read_toc(archive):
                    if name not in ('levelmap_1.col', 'seabed1.col'):
                        continue
                    source.seek(offset)
                    data = source.read(size)
                    with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                        expected = [(c.name, *world._col_record(c))
                                    for c in col.parse(data)]
                    with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                        from .portable import parse_world_collisions
                        self.assertEqual(parse_world_collisions(data), expected)


if __name__ == '__main__':
    unittest.main()
