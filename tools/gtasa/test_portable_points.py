import os
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from . import entry_exits, map_points
from .test_portable_build import build_library

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which('cc'), 'C compiler unavailable')
class PortablePointsTest(unittest.TestCase):
    def test_points_match_python(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            library = root / 'libassets.so'
            build_library(library)
            maps = root / 'data/maps'
            maps.mkdir(parents=True)
            (root / 'data/gta.dat').write_text("IPL DATA\\MAPS\\city.IPL\n")
            (maps / 'city.ipl').write_text(
                'enex\n'
                '100.5, -200.5, 12, 0, 1, 1, 8, 0, 0, 0, 0, 0, 4, "AMMUN1", 0, 2, 0, 24\n'
                '100.5, -200.5, 12, 0, 1, 1, 8, 0, 0, 0, 0, 0, 4, "AMMUN1", 0, 2, 0, 24\n'
                '2999.5, -3000.5, 12, 0, 1, 1, 8, 0, 0, 0, 0, 0, 4, "BAR2", 0, 2, 0, 24\n'
                '200, -300, 12, 0, 1, 1, 8, 0, 0, 0, 0, 0, 4, "GYM1", 0, 2, 0, 24\nend\n')
            with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                expected, stats = map_points.build(root, {})
                reference_denx = entry_exits.build(root, {})
            with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                actual, native_stats = map_points.build(root, {})
                converted_denx = entry_exits.build(root, {})
            self.assertEqual(actual, expected)
            self.assertEqual(native_stats, stats)
            self.assertEqual(converted_denx, reference_denx)
            self.assertEqual(struct.unpack_from('<H', reference_denx[0], 6)[0], 4)
            subprocess.run(['make', '-f', 'tools/portable/Makefile',
                            'build/enexbuild_cli'], cwd=ROOT, check=True,
                           stdout=subprocess.DEVNULL)
            output = root / 'entrances.denx'
            subprocess.run([str(ROOT / 'build/enexbuild_cli'), str(root),
                            str(output)], check=True)
            self.assertEqual(output.read_bytes(), reference_denx[0])

    def test_real_entrances_match_python(self):
        game = ROOT / 'game'
        if not (game / 'data/gta.dat').is_file():
            self.skipTest('local GTA installation unavailable')
        with tempfile.TemporaryDirectory() as tmp:
            library = Path(tmp) / 'libassets.so'
            build_library(library)
            with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                expected = entry_exits.build(game, {})
            with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                self.assertEqual(entry_exits.build(game, {}), expected)
            cli = Path(tmp) / 'enex_cli'
            subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra',
                            '-Werror', str(ROOT / 'tools/portable/enex.c'),
                            str(ROOT / 'tools/portable/ipl.c'),
                            str(ROOT / 'tools/portable/enex_cli.c'),
                            '-lm', '-o', str(cli)], check=True)
            files = []
            for line in entry_exits.world._lines(game / 'data/gta.dat'):
                kind, _, name = line.partition(' ')
                if kind.upper() == 'IPL' and name.lower().endswith('.ipl'):
                    path = entry_exits.world.resolve(game, name.strip())
                    if path:
                        files.append(str(path))
            output = Path(tmp) / 'entrances.denx'
            subprocess.run([str(cli), str(output), *files], check=True)
            self.assertEqual(output.read_bytes(), expected[0])


if __name__ == '__main__':
    unittest.main()
