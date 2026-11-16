import os
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from . import ds_misc, img
from .test_ifp import chunk
from .test_portable_build import build_library

ROOT = Path(__file__).resolve().parents[2]


def sample():
    frame = struct.pack('<4f3ff', 0, 0, 0, 1, -100, 75, 3, 0)
    frames = frame + struct.pack('<4f3ff', 0, 0, 0, 1, -120, 80, 3, 1)
    seq = chunk(b'CPAN', chunk(b'ANIM', b'root'.ljust(28, b'\0')
                                        + struct.pack('<3Ii', 2, 0, 0, -1))
                + chunk(b'KRT0', frames))
    return chunk(b'ANPK', chunk(b'INFO', struct.pack('<I', 1) + b'PROLOG3\0')
                 + chunk(b'NAME', b'csgizmo\0')
                 + chunk(b'DGAN', chunk(b'INFO', struct.pack('<II', 1, 0)) + seq))


@unittest.skipUnless(shutil.which('cc'), 'C compiler unavailable')
class PortableIFPTest(unittest.TestCase):
    def test_native_cli_matches_python(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / 'sample.ifp'
            source.write_bytes(sample())
            exe = root / 'ifp_cli'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2',
                            str(ROOT / 'tools/portable/ifp.c'),
                            str(ROOT / 'tools/portable/ifp_cli.c'), '-lm', '-o', str(exe)], check=True)
            output = root / 'sample.danm'
            subprocess.run([str(exe), str(source), str(output), '2', '0', '0'], check=True)
            with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                expected, _ = ds_misc.convert_ifp(source.read_bytes(),
                                                  {**ds_misc.ANIM_DEFAULTS, 'keep_every': 2})
            self.assertEqual(output.read_bytes(), expected)

    def test_anim_output_matches_python(self):
        with tempfile.TemporaryDirectory() as tmp:
            library = Path(tmp) / 'libassets.so'
            build_library(library)
            samples = [('ANPK', sample())]
            path = ROOT / 'game/anim/ped.ifp'
            if path.is_file():
                samples.append((path.name, path.read_bytes()))
            archive = ROOT / 'game/anim/anim.img'
            if archive.is_file():
                with archive.open('rb') as source:
                    for name, offset, size in img.read_toc(archive)[:3]:
                        if name.lower().endswith('.ifp'):
                            source.seek(offset)
                            samples.append((name, source.read(size)))
            for name, data in samples:
                for params in (
                    {**ds_misc.ANIM_DEFAULTS, 'keep_every': 2},
                    {**ds_misc.ANIM_DEFAULTS, 'keep_every': 4, 'max_frames': 3,
                     'drop_translation': True, 'rigid_clips': ['csgizmo']},
                    {**ds_misc.ANIM_DEFAULTS, 'remove': ['csgizmo', 'idle*']},
                    {**ds_misc.ANIM_DEFAULTS, 'remove': ['[c]sgizmo', 'idle[0-9]*']},
                    {**ds_misc.ANIM_DEFAULTS, 'remove': ['[!c]sgizmo', 'csg[!z]mo']},
                    {**ds_misc.ANIM_DEFAULTS, 'rigid_clips': ['CSGIZMO']},
                ):
                    with self.subTest(file=name, options=params):
                        with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                            expected = ds_misc.convert_ifp(data, params)
                        with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                            actual = ds_misc.convert_ifp(data, params)
                        self.assertEqual(actual, expected)

    def test_real_cutscene_packages(self):
        root = ROOT / 'game/anim/cuts'
        if not root.is_dir():
            self.skipTest('local cutscene assets unavailable')
        with tempfile.TemporaryDirectory() as tmp:
            library = Path(tmp) / 'libassets.so'
            build_library(library)
            params = {**ds_misc.ANIM_DEFAULTS, 'keep_every': 4,
                      'rigid_clips': ['csgizmo', 'cshandcuff', 'csmoneystck']}
            for path in root.glob('*.ifp'):
                data = path.read_bytes()
                with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                    expected = ds_misc.convert_ifp(data, params)
                with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                    actual = ds_misc.convert_ifp(data, params)
                self.assertEqual(actual, expected, path.name)


if __name__ == '__main__':
    unittest.main()
