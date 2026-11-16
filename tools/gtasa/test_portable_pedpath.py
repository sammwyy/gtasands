import os
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from . import peds
from . import world
from .test_portable_build import ROOT, build_library


@unittest.skipUnless(shutil.which('cc'), 'C compiler unavailable')
class PortablePedPathTest(unittest.TestCase):
    def test_real_path_graph_and_population_match_python(self):
        game = ROOT / 'game'
        paths = world.resolve(game, 'data/paths')
        if not paths:
            self.skipTest('local GTA installation unavailable')
        bounds = [2150, -1950, 2850, -1400]
        with tempfile.TemporaryDirectory() as tmp:
            library = Path(tmp) / 'libassets.so'
            build_library(library)
            for vehicles in (False, True):
                with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                    expected = peds.read_path_nodes(paths, bounds, vehicles)
                with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                    self.assertEqual(peds.read_path_nodes(paths, bounds, vehicles),
                                     expected)
            script = (game / 'data/script/main.scm').read_bytes()
            with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                expected_zones = peds.scm_zone_setup(script)
            with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                self.assertEqual(peds.scm_zone_setup(script), expected_zones)
            with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                expected_pop = peds.parse_popcycle(game / 'data/popcycle.dat')
            with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                self.assertEqual(peds.parse_popcycle(game / 'data/popcycle.dat'),
                                 expected_pop)
            for function, name in ((peds.parse_peds_ide, 'peds.ide'),
                                   (peds.parse_pedstats, 'pedstats.dat'),
                                   (peds.parse_animgrp, 'animgrp.dat'),
                                   (peds.parse_zones, 'info.zon')):
                with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                    expected_section = function(game / 'data' / name)
                with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                    self.assertEqual(function(game / 'data' / name),
                                     expected_section, name)
            by_name = {ped['model']: i for i, ped in enumerate(
                peds.parse_peds_ide(game / 'data/peds.ide'))}
            with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                expected_groups = peds.parse_pedgrp(game / 'data/pedgrp.dat', by_name)
            with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                self.assertEqual(peds.parse_pedgrp(game / 'data/pedgrp.dat',
                                                    by_name), expected_groups)
            with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                expected = peds.build(game, {'bounds': bounds})
            with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                self.assertEqual(peds.build(game, {'bounds': bounds}), expected)
            subprocess.run(['make', '-f', 'tools/portable/Makefile',
                            'build/pedbuild_cli'], cwd=ROOT, check=True,
                           stdout=subprocess.DEVNULL)
            output = Path(tmp) / 'peds.dped'
            subprocess.run([str(ROOT / 'build/pedbuild_cli'),
                            str(game), str(output), *(str(value) for value in bounds)],
                           check=True)
            self.assertEqual(output.read_bytes(), expected[0])

    def test_pedestrian_and_vehicle_path_nodes_match_python(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            paths = root / 'paths'
            paths.mkdir()
            raw = bytearray(struct.pack('<5I', 3, 1, 2, 0, 3))
            for x, y, base, width, flags in ((16, -8, 0, 4, 0x10001),
                                              (32, 16, 1, 3, 0x20002),
                                              (3200, -1000, 3, 2, 0)):
                raw += bytes(8)
                raw += struct.pack('<3hhhHHBBI', x, y, 0, 0, base, 0, 0,
                                   width, 0, flags)
            for area, node in ((0, 1), (0, 0), (0, 2)):
                raw += struct.pack('<HH', area, node)
            (paths / 'nodes0.dat').write_bytes(raw)
            library = root / 'libassets.so'
            build_library(library)
            for vehicles in (False, True):
                with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                    expected = peds.read_path_nodes(paths, [-10, -10, 10, 10],
                                                      vehicles)
                with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                    actual = peds.read_path_nodes(paths, [-10, -10, 10, 10],
                                                   vehicles)
                self.assertEqual(actual, expected)
                self.assertTrue(actual)


if __name__ == '__main__':
    unittest.main()
