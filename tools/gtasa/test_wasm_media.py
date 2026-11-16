import json
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

from . import audio
from .world import resolve


ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which('emcc') and shutil.which('node') and
                     shutil.which('ffmpeg') and shutil.which('cc'),
                     'Emscripten, Node, ffmpeg or C compiler unavailable')
class WASMMediaTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(['make', '-f', 'tools/portable/Makefile',
                        'web/assets.mjs', 'build/mpeg_movie_cli',
                        'build/audio_stream_cli', 'build/worldbuild_cli',
                        'build/pedbuild_cli', 'build/vehbuild_cli',
                        'build/enexbuild_cli'],
                       cwd=ROOT, check=True,
                       stdout=subprocess.DEVNULL)

    def test_movie_and_encrypted_stream_match_native(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            movie = root / 'sample.mpg'
            subprocess.run(['ffmpeg', '-nostdin', '-loglevel', 'error',
                            '-f', 'lavfi', '-i',
                            'testsrc=size=64x48:rate=30:duration=1',
                            '-f', 'lavfi', '-i',
                            'sine=frequency=440:sample_rate=44100:duration=1',
                            '-c:v', 'mpeg1video', '-q:v', '5',
                            '-c:a', 'mp2', '-b:a', '128k', '-shortest',
                            str(movie)], check=True)
            ogg = root / 'sample.ogg'
            subprocess.run(['ffmpeg', '-nostdin', '-loglevel', 'error',
                            '-f', 'lavfi', '-i',
                            'sine=frequency=500:sample_rate=32000:duration=1',
                            '-ac', '2', '-c:a', 'libvorbis', str(ogg)],
                           check=True)
            offset = 29
            encrypted = root / 'stream.enc'
            encrypted.write_bytes(audio.decrypt(
                bytes(audio.TRACK_HEADER) + ogg.read_bytes(), offset))
            native_movie = root / 'native.dvid'
            native_sound = root / 'native.dsnd'
            wasm_movie = root / 'wasm.dvid'
            wasm_sound = root / 'wasm.dsnd'
            subprocess.run([str(ROOT / 'build/mpeg_movie_cli'),
                            str(movie), str(native_movie)], check=True)
            subprocess.run([str(ROOT / 'build/audio_stream_cli'),
                            str(encrypted), str(native_sound), str(offset)],
                           check=True)
            script = f"""
                import fs from 'node:fs';
                import createModule from
                    {json.dumps((ROOT / 'web/assets.mjs').as_uri())};
                import {{convertMovie}} from
                    {json.dumps((ROOT / 'web/movies.mjs').as_uri())};
                import {{convertStream}} from
                    {json.dumps((ROOT / 'web/streams.mjs').as_uri())};
                const module = await createModule();
                const movie = convertMovie(module,
                    new Uint8Array(fs.readFileSync(process.argv[1])));
                fs.writeFileSync(process.argv[2], movie.data);
                const sound = await convertStream(module,
                    new Uint8Array(fs.readFileSync(process.argv[3])),
                    Number(process.argv[4]), {{
                        sample_rate: 11025, format: 'adpcm',
                        volume: 1, max_seconds: 0
                    }});
                fs.writeFileSync(process.argv[5], sound);
            """
            subprocess.run(['node', '--input-type=module', '-e', script,
                            str(movie), str(wasm_movie), str(encrypted),
                            str(offset), str(wasm_sound)], cwd=ROOT, check=True)
            self.assertEqual(wasm_movie.read_bytes(), native_movie.read_bytes())
            self.assertEqual(wasm_sound.read_bytes(), native_sound.read_bytes())

    def test_world_file_callbacks_match_native_cli(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = Path(tmp) / 'game'
            maps = game / 'data/maps'
            models = game / 'models'
            maps.mkdir(parents=True)
            models.mkdir()
            (game / 'data/gta.dat').write_text(
                'IDE data/maps/objects.ide\nIPL data/maps/out.ipl\n')
            (maps / 'objects.ide').write_text(
                'objs\n1, outside, tex, 100, 0\nend\n')
            (maps / 'out.ipl').write_text(
                'inst\n1, outside, 0, 10, -20, 12, 0, 0, 0, 1, -1\nend\n')
            index = bytearray(2048)
            struct.pack_into('<4sI', index, 0, b'VER2', 2)
            struct.pack_into('<IHH24s', index, 8, 1, 1, 1, b'outside.dff')
            struct.pack_into('<IHH24s', index, 40, 2, 1, 1, b'tex.txd')
            (models / 'gta3.img').write_bytes(index + bytes(4096))
            (models / 'gta_int.img').write_bytes(b'VER2' + bytes(4))
            native = Path(tmp) / 'native.dwld'
            wasm = Path(tmp) / 'wasm.dwld'
            subprocess.run([str(ROOT / 'build/worldbuild_cli'), str(game),
                            str(native)], check=True)
            script = f"""
                import fs from 'node:fs';
                import path from 'node:path';
                import createModule from
                    {json.dumps((ROOT / 'web/assets.mjs').as_uri())};
                import {{convertWorld}} from
                    {json.dumps((ROOT / 'web/world.mjs').as_uri())};
                const names = ['data/gta.dat', 'data/maps/objects.ide',
                    'data/maps/out.ipl', 'models/gta3.img',
                    'models/gta_int.img'];
                const files = new Map(names.map(name => {{
                    const data = fs.readFileSync(path.join(process.argv[1], name));
                    return [name, {{size: data.length, data}}];
                }}));
                const module = await createModule();
                const result = convertWorld(module, files, {{sector: 50}},
                    (file, start, length) =>
                        file.data.subarray(start, start + length));
                fs.writeFileSync(process.argv[2], result.data);
            """
            subprocess.run(['node', '--input-type=module', '-e', script,
                            str(game), str(wasm)], check=True)
            self.assertEqual(wasm.read_bytes(), native.read_bytes())

    def test_real_peds_file_callbacks_match_native_cli(self):
        game = ROOT / 'game'
        if not resolve(game, 'data/peds.ide'):
            self.skipTest('local GTA installation unavailable')
        names = [
            'data/peds.ide', 'data/pedstats.dat', 'data/pedgrp.dat',
            'data/cargrp.dat', 'data/vehicles.ide', 'data/popcycle.dat',
            'data/animgrp.dat', 'data/info.zon', 'data/script/main.scm'
        ] + [f'data/paths/nodes{i}.dat' for i in range(64)]
        files = {name: str(file.resolve()) for name in names
                 if (file := resolve(game, name)) and file.is_file()}
        bounds = [2150, -1950, 2850, -1400]
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            listing = root / 'files.json'
            listing.write_text(json.dumps(files))
            native = root / 'native.dped'
            wasm = root / 'wasm.dped'
            subprocess.run([str(ROOT / 'build/pedbuild_cli'), str(game),
                            str(native), *(str(value) for value in bounds)],
                           check=True)
            script = f"""
                import fs from 'node:fs';
                import createModule from
                    {json.dumps((ROOT / 'web/assets.mjs').as_uri())};
                import {{convertPeds}} from
                    {json.dumps((ROOT / 'web/peds.mjs').as_uri())};
                const list = JSON.parse(fs.readFileSync(process.argv[1], 'utf8'));
                const files = new Map(Object.entries(list).map(([name, filename]) =>
                    [name, {{name: filename, size: fs.statSync(filename).size}}]));
                const module = await createModule();
                const result = convertPeds(module, files,
                    {{bounds: [2150, -1950, 2850, -1400]}},
                    (file, start, length) => {{
                        const data = Buffer.allocUnsafe(length);
                        const handle = fs.openSync(file.name, 'r');
                        try {{
                            const count = fs.readSync(handle, data, 0, length, start);
                            return data.subarray(0, count);
                        }} finally {{fs.closeSync(handle);}}
                    }});
                fs.writeFileSync(process.argv[2], result.data);
            """
            subprocess.run(['node', '--input-type=module', '-e', script,
                            str(listing), str(wasm)], check=True)
            self.assertEqual(wasm.read_bytes(), native.read_bytes())

    def test_real_vehicle_file_callbacks_match_native_cli(self):
        game = ROOT / 'game'
        if not resolve(game, 'data/vehicles.ide'):
            self.skipTest('local GTA installation unavailable')
        names = ['data/vehicles.ide', 'data/handling.cfg',
                 'data/carcols.dat', 'data/surface.dat', 'data/surfinfo.dat',
                 'data/water.dat'] + [
                     f'data/paths/tracks{suffix}.dat'
                     for suffix in ('', '2', '3', '4')]
        files = {name: str(file.resolve()) for name in names
                 if (file := resolve(game, name)) and file.is_file()}
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            listing = root / 'files.json'
            listing.write_text(json.dumps(files))
            native = root / 'native.dveh'
            wasm = root / 'wasm.dveh'
            subprocess.run([str(ROOT / 'build/vehbuild_cli'),
                            str(game), str(native)], check=True)
            script = f"""
                import fs from 'node:fs';
                import createModule from
                    {json.dumps((ROOT / 'web/assets.mjs').as_uri())};
                import {{convertVehicles}} from
                    {json.dumps((ROOT / 'web/vehicles.mjs').as_uri())};
                const list = JSON.parse(fs.readFileSync(process.argv[1], 'utf8'));
                const files = new Map(Object.entries(list).map(([name, filename]) =>
                    [name, {{name: filename, size: fs.statSync(filename).size}}]));
                const module = await createModule();
                const result = convertVehicles(module, files,
                    (file, start, length) => {{
                        const data = Buffer.allocUnsafe(length);
                        const handle = fs.openSync(file.name, 'r');
                        try {{
                            const count = fs.readSync(handle, data, 0, length, start);
                            return data.subarray(0, count);
                        }} finally {{fs.closeSync(handle);}}
                    }});
                fs.writeFileSync(process.argv[2], result.data);
            """
            subprocess.run(['node', '--input-type=module', '-e', script,
                            str(listing), str(wasm)], check=True)
            self.assertEqual(wasm.read_bytes(), native.read_bytes())

    def test_real_entrances_file_callbacks_match_native_cli(self):
        game = ROOT / 'game'
        dat = resolve(game, 'data/gta.dat')
        if not dat:
            self.skipTest('local GTA installation unavailable')
        names = ['data/gta.dat'] + [
            line.strip().split(maxsplit=1)[1]
            for line in dat.read_text(errors='replace').splitlines()
            if line.upper().lstrip().startswith('IPL ') and len(line.split()) > 1]
        files = {name.replace('\\', '/').lower(): str(file.resolve())
                 for name in names
                 if (file := resolve(game, name)) and file.is_file()}
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            listing = root / 'files.json'
            listing.write_text(json.dumps(files))
            native = root / 'native.denx'
            wasm = root / 'wasm.denx'
            points = root / 'wasm.dpoi'
            subprocess.run([str(ROOT / 'build/enexbuild_cli'),
                            str(game), str(native)], check=True)
            script = f"""
                import fs from 'node:fs';
                import createModule from
                    {json.dumps((ROOT / 'web/assets.mjs').as_uri())};
                import {{convertEntrances}} from
                    {json.dumps((ROOT / 'web/entrances.mjs').as_uri())};
                const list = JSON.parse(fs.readFileSync(process.argv[1], 'utf8'));
                const files = new Map(Object.entries(list).map(([name, filename]) =>
                    [name, {{name: filename, size: fs.statSync(filename).size}}]));
                const module = await createModule();
                const result = convertEntrances(module, files,
                    (file, start, length) => {{
                        const data = Buffer.allocUnsafe(length);
                        const handle = fs.openSync(file.name, 'r');
                        try {{
                            const count = fs.readSync(handle, data, 0, length, start);
                            return data.subarray(0, count);
                        }} finally {{fs.closeSync(handle);}}
                    }});
                fs.writeFileSync(process.argv[2], result.enex);
                fs.writeFileSync(process.argv[3], result.points);
            """
            subprocess.run(['node', '--input-type=module', '-e', script,
                            str(listing), str(wasm), str(points)], check=True)
            self.assertEqual(wasm.read_bytes(), native.read_bytes())
            from .map_points import build
            self.assertEqual(points.read_bytes(), build(game, {})[0])
