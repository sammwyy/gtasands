import os
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from . import vehicles
from .test_portable_build import ROOT, build_library


@unittest.skipUnless(shutil.which('cc'), 'C compiler unavailable')
class PortableVehicleTest(unittest.TestCase):
    def test_real_vehicle_tables_match_python(self):
        game = ROOT / 'game'
        if not (game / 'data/vehicles.ide').is_file():
            self.skipTest('local GTA installation unavailable')
        with tempfile.TemporaryDirectory() as tmp:
            library = Path(tmp) / 'libassets.so'
            build_library(library)
            with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                reference_ide = vehicles.parse_ide(game / 'data/vehicles.ide')
            with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                self.assertEqual(vehicles.parse_ide(game / 'data/vehicles.ide'),
                                 reference_ide)
            with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                reference_handling = vehicles.parse_handling(game / 'data/handling.cfg')
            with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                self.assertEqual(vehicles.parse_handling(game / 'data/handling.cfg'),
                                 reference_handling)
            tracks = [vehicles.resolve(game, f'data/paths/tracks{n}.dat')
                      for n in ('', '2', '3', '4')]
            for func, args in (
                    (vehicles.parse_special, (game / 'data/handling.cfg',)),
                    (vehicles.parse_carcols, (game / 'data/carcols.dat',)),
                    (vehicles.parse_surfaces, (game / 'data/surface.dat',
                                               game / 'data/surfinfo.dat')),
                    (vehicles.parse_water, (vehicles.resolve(game, 'data/water.dat'),)),
                    (vehicles.parse_tracks, (tracks,))):
                with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                    expected_section = func(*args)
                with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                    self.assertEqual(func(*args), expected_section, func.__name__)
            with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                expected = vehicles.build(game, {})
            with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                actual = vehicles.build(game, {})
            self.assertEqual(actual, expected)
            subprocess.run(['make', '-f', 'tools/portable/Makefile',
                            'build/vehbuild_cli'], cwd=ROOT, check=True,
                           stdout=subprocess.DEVNULL)
            autonomous = Path(tmp) / 'autonomous.dveh'
            subprocess.run([str(ROOT / 'build/vehbuild_cli'), str(game),
                            str(autonomous)], check=True)
            self.assertEqual(autonomous.read_bytes(), expected[0])
            self.assertEqual(struct.unpack_from('<H', actual[0], 6)[0],
                             actual[1]['vehicles'])
            self.assertEqual(actual[1]['rec'], 380)
            exe = Path(tmp) / 'veh_cli'
            subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra',
                            '-Werror', str(ROOT / 'tools/portable/vehpack.c'),
                            str(ROOT / 'tools/portable/vehparse.c'),
                            str(ROOT / 'tools/portable/veh_cli.c'),
                            '-lm', '-o', str(exe)], check=True)
            inputs = [vehicles.resolve(game, path) for path in (
                'data/vehicles.ide', 'data/handling.cfg', 'data/carcols.dat',
                'data/surface.dat', 'data/surfinfo.dat', 'data/water.dat',
                *(f'data/paths/tracks{n}.dat' for n in ('', '2', '3', '4')))]
            output = Path(tmp) / 'vehicles.dveh'
            subprocess.run([str(exe), *(str(path) if path else '-'
                                        for path in inputs), str(output)], check=True)
            self.assertEqual(output.read_bytes(), expected[0])


if __name__ == '__main__':
    unittest.main()
