import os
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from . import world
from .test_portable_build import ROOT, build_library


@unittest.skipUnless(shutil.which('cc'), 'C compiler unavailable')
class PortableWorldPackTest(unittest.TestCase):
    def test_native_world_cli_reads_original_files_without_python(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            data = root / 'game/data/maps'
            models = root / 'game/models'
            data.mkdir(parents=True)
            models.mkdir(parents=True)
            (root / 'game/data/gta.dat').write_text(
                'IDE data/maps/objects.ide\nIPL data/maps/out.ipl\n')
            (data / 'objects.ide').write_text(
                'objs\n1, outside, tex, 100, 0\nend\n')
            (data / 'out.ipl').write_text(
                'inst\n1, outside, 0, 10, -20, 12, 0, 0, 0, 1, -1\nend\n')
            index = bytearray(2048)
            struct.pack_into('<4sI', index, 0, b'VER2', 2)
            struct.pack_into('<IHH24s', index, 8, 1, 1, 1, b'outside.dff')
            struct.pack_into('<IHH24s', index, 40, 2, 1, 1, b'tex.txd')
            (models / 'gta3.img').write_bytes(index + bytes(4096))
            (models / 'gta_int.img').write_bytes(b'VER2' + bytes(4))
            with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                baseline, stats = world.build(root / 'game',
                                              {'sector': 50, 'lods': True})
            exe = root / 'worldbuild_cli'
            sources = ('worldbuild.c', 'worldbuild_cli.c', 'gamefiles.c',
                       'dff.c', 'ide.c',
                       'img.c', 'ipl.c', 'worldplan.c', 'worldpack.c',
                       'worldcol.c', 'worldcol_archive.c')
            subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                            *(str(ROOT / 'tools/portable' / source)
                              for source in sources), '-lm', '-o', str(exe)],
                           check=True)
            output = root / 'world.dwld'
            subprocess.run([str(exe), str(root / 'game'), str(output)],
                           check=True)
            self.assertEqual(output.read_bytes(), baseline)
            self.assertEqual(stats['instances'], 1)

    def test_world_sector_order_and_empty_world_match_python(self):
        from .portable import plan_world_sectors
        cases = ([], [
            (10, -100, 20, 1, (1, 0, 0, 0, 1, 0, 0, 0, 1), False, None, 0),
            (11, -50, 20, 1, (1, 0, 0, 0, 1, 0, 0, 0, 1), True, 10, 13),
            (12, -100, 20, 2, (1, 0, 0, 0, 1, 0, 0, 0, 1), False, 11, 0),
            (13, 49.99, -50, 3, (1, 0, 0, 0, 1, 0, 0, 0, 1), False, None, 0)])
        with tempfile.TemporaryDirectory() as tmp:
            library = Path(tmp) / 'libassets.so'
            build_library(library)
            for entries in cases:
                xs = [item[1] for item in entries] or [0]
                ys = [item[2] for item in entries] or [0]
                import math
                ox = math.floor(min(xs) / 50) * 50
                oy = math.floor(min(ys) / 50) * 50
                width = int((max(xs) - ox) // 50) + 1
                height = int((max(ys) - oy) // 50) + 1
                cells = [[] for _ in range(width * height)]
                for item in entries:
                    cx = int((item[1] - ox) // 50)
                    cy = int((item[2] - oy) // 50)
                    cells[cy * width + cx].append(item)
                with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                    self.assertEqual(plan_world_sectors(entries, 50),
                                     (ox, oy, width, height, cells))

    def test_world_asset_source_prefers_main_archive(self):
        with tempfile.TemporaryDirectory() as tmp:
            library = Path(tmp) / 'libassets.so'
            build_library(library)
            with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                from .portable import WorldAssetIndex, parse_world_dat
                index = WorldAssetIndex({'shared.dff': (2048, 4096),
                                         'main.dff': (8192, 2048)},
                                        {'shared.dff': (12288, 2048),
                                         'interior.dff': (4096, 6144)})
                self.assertEqual(index.resolve('shared.dff'), ((2048, 4096), 0))
                self.assertEqual(index.resolve('main.dff'), ((8192, 2048), 0))
                self.assertEqual(index.resolve('interior.dff'), ((4096, 6144), 1))
                self.assertIsNone(index.resolve('missing.dff'))
                source = (ROOT / 'game/data/gta.dat')
                if source.is_file():
                    expected = [[], []]
                    for line in world._lines(source):
                        key, _, value = line.partition(' ')
                        value = value.strip()
                        if key.upper() == 'IDE':
                            expected[0].append(value)
                        elif key.upper() == 'IPL' and value.lower().endswith('.ipl'):
                            expected[1].append(value)
                    self.assertEqual(parse_world_dat(source.read_bytes()),
                                     tuple(expected))

    def test_world_model_and_texture_tables_match_python(self):
        with tempfile.TemporaryDirectory() as tmp:
            library = Path(tmp) / 'libassets.so'
            build_library(library)
            with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                from .portable import WorldAssetIndex, plan_world_tables
                index = WorldAssetIndex(
                    {'one.dff': (2048, 4096), 'a.txd': (8192, 2048),
                     'b.txd': (10240, 4096)},
                    {'one.dff': (16384, 2048), 'two.dff': (20480, 2048),
                     'three.dff': (22528, 2048), 'b.txd': (24576, 2048)})
                objects = {1: ('one', 'a', 100.0, 0, 24, 24),
                           2: ('two', 'b', 200.0, 3, 23, 5),
                           3: ('three', 'missing', 50.0, 7, 24, 24)}
                self.assertEqual(
                    plan_world_tables([1, 2, 3], objects, {'a': 'b', 'b': 'a'},
                                      index),
                    ([('one', 0, 100.0, 0, 24, 24, 2048, 4096, 0),
                      ('two', 1, 200.0, 3 | (1 << 31), 23, 5, 20480, 2048, 1),
                      ('three', 0xFFFF, 50.0, 7 | (1 << 31), 24, 24,
                       22528, 2048, 1)],
                     [('a', 1, 0), ('b', 0, 0)]))

    def test_world_instance_planning_matches_python(self):
        text = [(100, 0, 0, 0, 0, 0, 0, 1, 0, 2),
                (101, 1, 2, 3, 0, 0, 0, 1, -1, -1),
                (102, 10, 10, 0, 0, 0, 0, 1, 256, -1)]
        stream = [(103, 100, 100, 0, 0, 0, 0, 1, 257, 2),
                  (404, 0, 0, 0, 0, 0, 0, 1, 0, -1)]
        objects = {n: (name, 'txd', 100, 0, 24, 24)
                   for n, name in ((100, 'main'), (101, 'lodghost'),
                                   (102, 'lodmain'), (103, 'streamed'))}
        toc = {name + '.dff': (0, 0) for name in ('main', 'lodmain', 'streamed')}
        with tempfile.TemporaryDirectory() as tmp:
            library = Path(tmp) / 'libassets.so'
            build_library(library)
            from .portable import plan_world_instances
            for bounds in (None, [-10, -10, 20, 20]):
                for keep in (False, True):
                    entries = text + stream
                    lods = {entry[9] for entry in entries if 0 <= entry[9] < len(text)}
                    hd = {entry[9]: entry[0] for entry in entries if 0 <= entry[9] < len(text)}
                    expected, skipped = [], {'model': 0, 'lod': 0, 'bounds': 0}
                    for i, entry in enumerate(entries):
                        mid, x, y, z, qx, qy, qz, qw, area, lod = entry
                        obj = objects.get(mid)
                        if not obj or obj[0] + '.dff' not in toc:
                            skipped['model'] += 1
                            continue
                        is_lod = i < len(text) and i in lods or obj[0].startswith('lod')
                        if is_lod and not keep:
                            skipped['lod'] += 1
                            continue
                        if bounds and not bounds[0] <= x <= bounds[2] or (
                                bounds and not bounds[1] <= y <= bounds[3]):
                            skipped['bounds'] += 1
                            continue
                        linked = (hd.get(i) if is_lod and i < len(text) else
                                  text[lod][0] if not is_lod and 0 <= lod < len(text) else None)
                        with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                            rot = world.rotation(qx, qy, qz, qw)
                        expected.append((mid, x, y, z, rot, is_lod, linked, area & 0xFF))
                    with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                        self.assertEqual(plan_world_instances(
                            text, stream, objects, toc, {}, bounds, keep),
                            (expected, skipped))

    def test_bounded_real_world_matches_python(self):
        game = ROOT / 'game'
        if not (game / 'models/gta3.img').is_file():
            self.skipTest('local GTA installation unavailable')
        options = {'sector': 50, 'lods': True, 'bounds': [2150, -1950, 2850, -1400]}
        with tempfile.TemporaryDirectory() as tmp:
            library = Path(tmp) / 'libassets.so'
            build_library(library)
            with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                expected = world.build(game, options)
            with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                self.assertEqual(world.build(game, options), expected)

    def test_world_output_matches_python(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            data = root / 'game/data'
            maps = data / 'maps'
            maps.mkdir(parents=True)
            models = root / 'game/models'
            models.mkdir()
            (models / 'gta3.img').touch()
            (models / 'gta_int.img').touch()
            (data / 'gta.dat').write_text(
                'IDE data/maps/objects.ide\nIPL data/maps/out.ipl\n')
            (maps / 'objects.ide').write_text(
                'objs\n1, outside, tex, 100, 0\n2, thing, tex, 100, 0\nend\n')
            (maps / 'out.ipl').write_text(
                'inst\n1, outside, 0, 10.5, -20.25, 12, 0, 0, 0, 1, -1\n'
                '2, thing, 0, 101.5, 75, 9, 0, 0, 0, 1, -1\nend\n')
            library = root / 'libassets.so'
            build_library(library)
            toc = [('outside.dff', 0, 0), ('thing.dff', 0, 0), ('tex.txd', 0, 0)]
            with patch.object(world.img, 'read_toc', return_value=toc), \
                 patch.object(world, '_radius', return_value=16), \
                 patch.object(world, '_col_models', return_value=({}, 0)):
                with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                    expected = world.build(root / 'game', {'sector': 50})
                with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                    actual = world.build(root / 'game', {'sector': 50})
            self.assertEqual(actual, expected)


if __name__ == '__main__':
    unittest.main()
