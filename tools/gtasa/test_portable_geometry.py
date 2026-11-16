import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from . import ds_mesh, img, portable
from .test_portable_build import build_library
from .pipeline import Config

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which('cc'), 'C compiler unavailable')
class PortableGeometryTest(unittest.TestCase):
    def test_real_ground_models_match_python(self):
        archive = ROOT / 'game/models/gta3.img'
        if not archive.is_file():
            self.skipTest('local GTA installation unavailable')
        config = Config(ROOT / 'resources.toml')
        config.game = ROOT / 'game'
        wanted = {
            'cw_barnie.dff', 'gaz23_law.dff', 'groundbit84_sfs.dff',
            'newcomprd_las2.dff', 'road_hill04b.dff',
            'vegaseroad022.dff', 'vgssairportland12.dff'
        }
        entries = [(name, off, size)
                   for name, off, size in img.read_toc(archive)
                   if name in wanted]
        self.assertEqual({name for name, _, _ in entries}, wanted)
        with tempfile.TemporaryDirectory() as tmp:
            library = Path(tmp) / 'libassets.so'
            build_library(library)
            with archive.open('rb') as source:
                for name, off, size in entries:
                    with self.subTest(model=name):
                        source.seek(off)
                        data = source.read(size)
                        params, _, removed = config.params(
                            config.sections['gta3_img'], 'model', name)
                        self.assertFalse(removed)
                        with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                            expected = ds_mesh.convert_dff(data, params)
                        with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                            self.assertEqual(ds_mesh.convert_dff(data, params),
                                             expected)

    def test_native_cli_matches_python(self):
        archive = ROOT / 'game/models/gta3.img'
        if not archive.is_file():
            self.skipTest('local GTA installation unavailable')
        name, offset, size = next((n, o, s) for n, o, s in img.read_toc(archive)
                                  if n.lower().endswith('.dff'))
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / name
            with archive.open('rb') as data:
                data.seek(offset)
                source.write_bytes(data.read(size))
            exe = root / 'dmdl_cli'
            paths = ('dmdl', 'dmdl_cli', 'dff_mesh', 'geometry', 'cluster', 'qem')
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2',
                            *(str(ROOT / f'tools/portable/{part}.c') for part in paths),
                            '-lm', '-o', str(exe)], check=True)
            output = root / 'model.dmdl'
            subprocess.run([str(exe), str(source), str(output), '0.5', '32'],
                           check=True)
            with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                expected, _ = ds_mesh.convert_dff(source.read_bytes(),
                                                  {**ds_mesh.DEFAULTS, 'ratio': 0.5, 'min_tris': 32})
            self.assertEqual(output.read_bytes(), expected)

    def test_complete_model_matches_python(self):
        archive = ROOT / 'game/models/gta3.img'
        if not archive.is_file():
            self.skipTest('local GTA installation unavailable')
        with tempfile.TemporaryDirectory() as tmp:
            library = Path(tmp) / 'libassets.so'
            build_library(library)
            wanted = {'ws_floodbeams.dff'}
            entries = [(name, off, size) for name, off, size in img.read_toc(archive)
                       if name.lower().endswith('.dff')]
            entries = entries[:10] + entries[500:510] + [
                (name, off, size) for name, off, size in entries if name in wanted]
            params = {**ds_mesh.DEFAULTS, 'ratio': 0.37, 'min_tris': 32,
                      'max_tris': 280, 'normals': True,
                      'drop_damage': True, 'drop_vlo': True}
            with archive.open('rb') as source:
                for name, off, size in entries:
                    with self.subTest(model=name):
                        source.seek(off)
                        data = source.read(size)
                        with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                            expected = ds_mesh.convert_dff(data, params)
                        with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                            self.assertIsNotNone(portable.convert_model(data, params))
                            self.assertEqual(ds_mesh.convert_dff(data, params), expected)

    def test_dmdl_geometry_matches_python(self):
        archive = ROOT / 'game/models/gta3.img'
        if not archive.is_file():
            self.skipTest('local GTA installation unavailable')
        with tempfile.TemporaryDirectory() as tmp:
            library = Path(tmp) / 'libassets.so'
            build_library(library)
            entries = [(name, off, size) for name, off, size in img.read_toc(archive)
                       if name.lower().endswith('.dff')]
            params = {**ds_mesh.DEFAULTS, 'ratio': 0.25, 'min_tris': 12,
                      'max_tris': 192, 'min_area': 0.85,
                      'max_shift': 0.5, 'ground_qem': True}
            for name, off, size in entries[:5] + entries[500:510] + entries[2000:2005]:
                with self.subTest(model=name), archive.open('rb') as source:
                    source.seek(off)
                    data = source.read(size)
                    with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                        expected = ds_mesh.convert_dff(data, params)
                    with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                        self.assertIsNotNone(portable.convert_model(data, params))
                        self.assertEqual(ds_mesh.convert_dff(data, params), expected)

    def test_skinned_and_rigid_models_match_python(self):
        archive = ROOT / 'game/models/player.img'
        rigid = ROOT / 'game/models/cutscene/cshandcuff.dff'
        if not archive.is_file() or not rigid.is_file():
            self.skipTest('local character assets unavailable')
        with tempfile.TemporaryDirectory() as tmp:
            library = Path(tmp) / 'libassets.so'
            build_library(library)
            name, off, size = next((n, o, s) for n, o, s in img.read_toc(archive)
                                   if n.lower().endswith('.dff'))
            with archive.open('rb') as source:
                source.seek(off)
                data = source.read(size)
            cases = [
                (name, data, {**ds_mesh.DEFAULTS, 'ratio': 0.5, 'normals': True, 'decimate': 'qem'}),
                (name, data, {**ds_mesh.DEFAULTS, 'ratio': 0.5, 'normals': True}),
                (rigid.name, rigid.read_bytes(),
                 {**ds_mesh.DEFAULTS, 'ratio': 0.28, 'rigid_skin': True}),
            ]
            for label, data, params in cases:
                with self.subTest(model=label):
                    with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                        expected = ds_mesh.convert_dff(data, params)
                    with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                        self.assertIsNotNone(portable.convert_model(data, params))
                        self.assertEqual(ds_mesh.convert_dff(data, params), expected)


if __name__ == '__main__':
    unittest.main()
