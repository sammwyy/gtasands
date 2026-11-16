import json
import os
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from . import ds_radar, ds_texture, img
from .pipeline import Config
from .test_portable_build import ROOT, build_library


@unittest.skipUnless(shutil.which('node') and shutil.which('emcc') and
                     shutil.which('cc'), 'Node, Emscripten or C compiler unavailable')
class WebLooseTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(['make', '-f', 'tools/portable/Makefile',
                        'web/assets.mjs'], cwd=ROOT, check=True,
                       stdout=subprocess.DEVNULL)

    def test_real_loose_txd_matches_native_and_copies_data(self):
        source = ROOT / 'game/models/hud.txd'
        if not source.is_file():
            self.skipTest('local GTA installation unavailable')
        config = Config(ROOT / 'resources.toml')
        params, overrides, removed = config.params(
            config.sections['models_loose'], 'texture', 'models/hud.txd')
        self.assertFalse(removed)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            library = root / 'libassets.so'
            build_library(library)
            with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                expected = ds_texture.convert_txd(source.read_bytes(),
                                                  params, overrides)
            original = {'models/hud.txd': str(source),
                        'data/synthetic.dat': str(source)}
            listing = root / 'files.json'
            listing.write_text(json.dumps(original))
            output = root / 'wasm.dtxd'
            script = f"""
                import fs from 'node:fs';
                import createModule from
                    {json.dumps((ROOT / 'web/assets.mjs').as_uri())};
                import {{Parameters}} from
                    {json.dumps((ROOT / 'web/params.mjs').as_uri())};
                import {{convertLooseAssets}} from
                    {json.dumps((ROOT / 'web/loose_assets.mjs').as_uri())};
                const listed = JSON.parse(fs.readFileSync(process.argv[1], 'utf8'));
                const original = new Map(Object.entries(listed).map(([path, filename]) =>
                    [path, new Blob([fs.readFileSync(filename)])]));
                const manifest = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
                const module = await createModule();
                const output = await convertLooseAssets(module,
                    new Parameters(manifest, original), original, []);
                const converted = output.find(entry => entry.path === 'models/hud.txd');
                const raw = output.find(entry => entry.path === 'data/synthetic.dat');
                if (!converted || raw.file !== original.get('data/synthetic.dat'))
                    throw new Error('Loose files or copy missing.');
                fs.writeFileSync(process.argv[3],
                    new Uint8Array(await converted.file.arrayBuffer()));
            """
            subprocess.run(['node', '--input-type=module', '-e', script,
                            str(listing), str(ROOT / 'web/resources.json'),
                            str(output)], cwd=ROOT, check=True)
            self.assertEqual(output.read_bytes(), expected)

    def test_real_radar_tiles_match_native(self):
        archive = ROOT / 'game/models/gta3.img'
        if not archive.is_file():
            self.skipTest('local GTA installation unavailable')
        entries = {name.lower(): (off, size)
                   for name, off, size in img.read_toc(archive)}
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            library = root / 'libassets.so'
            build_library(library)
            tiles = []
            with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                for i in range(4):
                    off, size = entries[f'radar{i:02}.txd']
                    tile, _ = ds_radar.convert_radar(
                        img.read_entry(archive, off, size), {})
                    tiles.append(tile)
            full = 8192
            expected = struct.pack('<4sHHHHHHII', b'DSRT', 1, 4, 2,
                                   64, 32, 0, 28, 28 + 4 * full)
            expected += bytes([1] * 4)
            expected += b''.join(tile[:full] for tile in tiles)
            expected += b''.join(tile[full:] for tile in tiles)
            output = root / 'radar.dsrt'
            script = f"""
                import fs from 'node:fs';
                import createModule from
                    {json.dumps((ROOT / 'web/assets.mjs').as_uri())};
                import {{convertRadar}} from
                    {json.dumps((ROOT / 'web/radar_assets.mjs').as_uri())};
                const handle = fs.openSync(process.argv[1], 'r');
                const file = {{
                    size: fs.statSync(process.argv[1]).size,
                    slice(start, end) {{
                        const bytes = Buffer.allocUnsafe(end - start);
                        const count = fs.readSync(handle, bytes, 0, bytes.length, start);
                        return new Blob([bytes.subarray(0, count)]);
                    }}
                }};
                const module = await createModule();
                const output = await convertRadar(module, file, 2);
                if (output.tiles !== 4) throw new Error('Missing radar tiles.');
                fs.writeFileSync(process.argv[2], output.data);
                fs.closeSync(handle);
            """
            subprocess.run(['node', '--input-type=module', '-e', script,
                            str(archive), str(output)], cwd=ROOT, check=True)
            self.assertEqual(output.read_bytes(), expected)

    def test_cutscene_data_is_extracted_without_installing_it(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            archive = bytearray(2048)
            struct.pack_into('<4sI', archive, 0, b'VER2', 1)
            struct.pack_into('<IHH24s', archive, 8, 1, 1, 1, b'PROLOG3.DAT')
            source = root / 'cuts.img'
            source.write_bytes(archive + b'cutscene' + bytes(2048 - 8))
            output = root / 'result.dat'
            script = f"""
                import fs from 'node:fs';
                import createModule from
                    {json.dumps((ROOT / 'web/assets.mjs').as_uri())};
                import {{Parameters}} from
                    {json.dumps((ROOT / 'web/params.mjs').as_uri())};
                import {{convertLooseAssets}} from
                    {json.dumps((ROOT / 'web/loose_assets.mjs').as_uri())};
                const original = new Map([['anim/cuts.img',
                    new Blob([fs.readFileSync(process.argv[1])])]]);
                const manifest = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
                const module = await createModule();
                const outputs = await convertLooseAssets(module,
                    new Parameters(manifest, original), original, []);
                const file = outputs.find(entry =>
                    entry.path === 'anim/cuts/prolog3.dat');
                if (!file) throw new Error('Cutscene DAT missing.');
                fs.writeFileSync(process.argv[3],
                    new Uint8Array(await file.file.arrayBuffer()));
            """
            subprocess.run(['node', '--input-type=module', '-e', script,
                            str(source), str(ROOT / 'web/resources.json'),
                            str(output)], cwd=ROOT, check=True)
            self.assertEqual(output.read_bytes(), b'cutscene' + bytes(2048 - 8))
