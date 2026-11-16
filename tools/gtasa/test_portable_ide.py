import os
import shutil
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from . import world
from .test_portable_build import ROOT, build_library


@unittest.skipUnless(shutil.which('cc'), 'C compiler unavailable')
class PortableIDETest(unittest.TestCase):
    def test_ide_records_match_python(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            library = root / 'libassets.so'
            build_library(library)
            fixture = root / 'models.ide'
            fixture.write_text('objs\n1,thing,shared,1,150,2\nend\n'
                               'tobj\n2,clock,shared,200,1,8,16\nend\n'
                               'anim\n3,move,shared,walk,99,4\nend\n'
                               'txdp\nshared,base\nend\n')
            game = ROOT / 'game'
            paths = [fixture]
            dat = game / 'data/gta.dat'
            if dat.is_file():
                paths.append(world.resolve(game, 'data/maps/txd.ide'))
                paths.extend(world.resolve(game, line[4:].strip())
                             for line in dat.read_text(errors='replace').splitlines()
                             if line.upper().startswith('IDE '))
            for path in filter(None, paths):
                with self.subTest(path=str(path)):
                    with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                        objects, parents = {}, {}
                        world.parse_ide(path, objects, parents)
                    with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                        native_objects, native_parents = {}, {}
                        world.parse_ide(path, native_objects, native_parents)
                    self.assertEqual((native_objects, native_parents), (objects, parents))


if __name__ == '__main__':
    unittest.main()
