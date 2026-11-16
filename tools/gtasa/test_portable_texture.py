import os
import random
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from . import ds_texture, txd
from .dsfmt import pad4
from .test_portable_build import build_library

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which('cc'), 'C compiler unavailable')
class PortableTextureTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.work = Path(cls.temp.name)
        cls.exe = cls.work / 'dtex_cli'
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2',
                        str(ROOT / 'tools/portable/dtex.c'),
                        str(ROOT / 'tools/portable/dtex_cli.c'), '-o', str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def check_pixels(self, rgba, w, h, **params):
        p = {**ds_texture.DEFAULTS, **params}
        f, flags, tex, palette = ds_texture.encode(rgba, w, h, p)
        expected = (struct.pack('<4sHHBBHI', b'DTEX', w, h, f, flags, len(palette), len(tex))
                    + pad4(tex) + struct.pack(f'<{len(palette)}H', *palette))
        inp = self.work / 'pixels.rgba'
        out = self.work / 'pixels.dtex'
        inp.write_bytes(rgba)
        subprocess.run([str(self.exe), str(inp), str(w), str(h), p['format'], str(out),
                        str(p['alpha_threshold']), str(int(p['opaque'])), str(p['auto_colors']),
                        str(int(p['additive']))], check=True)
        actual = out.read_bytes()
        self.assertEqual(actual, expected, f'mismatch for {p}: first difference '
                         f'{next((i for i, (a, b) in enumerate(zip(actual, expected)) if a != b), None)}')

    def test_formats_and_alpha(self):
        rng = random.Random(17)
        for alpha in ('opaque', 'binary', 'gradient', 'clear'):
            rgba = bytearray()
            for i in range(16 * 16):
                a = (255 if alpha == 'opaque' else
                     [0, 255][i % 2] if alpha == 'binary' else
                     rng.randrange(256) if alpha == 'gradient' else 0)
                rgba.extend((rng.randrange(256), rng.randrange(256), rng.randrange(256), a))
            for fmt in ('auto', 'direct', 'pal4', 'pal16', 'pal256', 'a3i5', 'a5i3'):
                with self.subTest(alpha=alpha, fmt=fmt):
                    self.check_pixels(rgba, 16, 16, format=fmt)
            self.check_pixels(rgba, 16, 16, additive=True)
            self.check_pixels(rgba, 16, 16, opaque=True, auto_colors=256)

    def test_real_texture_samples(self):
        path = ROOT / 'game/models/hud.txd'
        if not path.exists():
            self.skipTest('local GTA installation unavailable')
        textures = txd.parse(path.read_bytes())
        self.assertGreater(len(textures), 0)
        for texture in textures[:8]:
            with self.subTest(texture=texture.name):
                w, h, rgba = txd.decode(texture)
                pixels = ds_texture.resize(rgba, w, h, 32, 32)
                self.check_pixels(pixels, 32, 32, format='auto')

    def test_native_texture_pipeline_matches_python(self):
        path = ROOT / 'game/models/hud.txd'
        if not path.exists():
            self.skipTest('local GTA installation unavailable')
        library = self.work / 'libdtex.so'
        build_library(library)
        texture = txd.parse(path.read_bytes())[0]
        params = {**ds_texture.DEFAULTS, 'scale': 0.25, 'max_size': 32}
        with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
            expected = ds_texture.convert_texture(texture, params)
        with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
            self.assertEqual(ds_texture.convert_texture(texture, params), expected)

    def test_native_dictionary_matches_python_with_overrides(self):
        path = ROOT / 'game/models/hud.txd'
        if not path.exists():
            self.skipTest('local GTA installation unavailable')
        library = self.work / 'libdtex.so'
        if not library.exists():
            build_library(library)
        data = path.read_bytes()
        options = {**ds_texture.DEFAULTS, 'scale': 0.5, 'max_size': 64}
        overrides = [('*.png', {'format': 'direct'}), ('*', {'max_size': 32})]
        with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
            expected = ds_texture.convert_txd(data, options, overrides)
        with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
            self.assertEqual(ds_texture.convert_txd(data, options, overrides), expected)

    def test_txd_decode_matches_python_for_real_archives(self):
        exe = self.work / 'txd_cli'
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2',
                        str(ROOT / 'tools/portable/txd.c'),
                        str(ROOT / 'tools/portable/txd_cli.c'), '-o', str(exe)], check=True)
        checked = 0
        for rel in ('models/hud.txd', 'models/fonts.txd', 'models/fronten1.txd',
                    'models/generic/vehicle.txd'):
            path = ROOT / 'game' / rel
            if not path.exists():
                continue
            for index, texture in enumerate(txd.parse(path.read_bytes())[:12]):
                for level in (0, min(1, len(texture.levels) - 1)):
                    with self.subTest(file=rel, name=texture.name, level=level):
                        out = self.work / 'decoded.rgba'
                        subprocess.run([str(exe), str(path), str(index), str(level), str(out)],
                                       check=True)
                        w, h, rgba = txd.decode(texture, level)
                        self.assertEqual(out.read_bytes(), struct.pack('<HH', w, h) + rgba)
                        checked += 1
        if not checked:
            self.skipTest('local GTA installation unavailable')

    def test_native_cli_converts_complete_dictionary(self):
        path = ROOT / 'game/models/fronten1.txd'
        if not path.exists():
            self.skipTest('local GTA installation unavailable')
        exe = self.work / 'texture_cli'
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2',
                        *(str(ROOT / f'tools/portable/{name}.c')
                          for name in ('dtex', 'txd', 'texture', 'texture_cli')),
                        '-o', str(exe)], check=True)
        output = self.work / 'fronten1.dtxd'
        subprocess.run([str(exe), str(path), str(output), '0.5', '128', 'auto'], check=True)
        params = {**ds_texture.DEFAULTS, 'scale': 0.5, 'max_size': 128}
        with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
            self.assertEqual(output.read_bytes(), ds_texture.convert_txd(path.read_bytes(), params))


if __name__ == '__main__':
    unittest.main()
