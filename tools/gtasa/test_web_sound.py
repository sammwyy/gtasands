import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from . import audio
from .test_portable_build import ROOT, build_library


@unittest.skipUnless(shutil.which('node'), 'Node unavailable')
class WebSoundTest(unittest.TestCase):
    def test_audio_worker_merges_generated_outputs(self):
        script = f"""
            import {{convertOriginalSound}} from
                {json.dumps((ROOT / 'web/local_sound.mjs').as_uri())};
            import {{strict as assert}} from 'node:assert';
            const original = new Map([['audio/config/pakfiles.dat', new Blob([])]]);
            const converted = [{{path: 'audio/sfx/feet.dsfx', file: new Blob([])}}];
            const manifest = {{sections: {{audio_sfx: {{}}, audio_streams: {{}}}}}};
            let terminated = false;
            const factory = () => ({{
                postMessage(message) {{
                    assert.equal(message.files, original);
                    assert.equal(message.converted, converted);
                    assert.equal(message.manifest, manifest);
                    queueMicrotask(() => this.onmessage({{data: {{
                        generated: [{{path: 'audio/sfx/banks.dsbl',
                                     file: new Blob(['DSBL'])}}],
                        banks: 2, tracks: 1
                    }}}}));
                }},
                terminate() {{terminated = true;}}
            }});
            const result = await convertOriginalSound(original, converted,
                manifest, () => {{}}, factory);
            assert.equal(result[0].path, 'audio/sfx/banks.dsbl');
            assert.equal(terminated, true);
        """
        subprocess.run(['node', '--input-type=module', '-e', script], check=True)

    @unittest.skipUnless(shutil.which('emcc') and shutil.which('cc'),
                         'Emscripten or C compiler unavailable')
    def test_real_sfx_and_vorbis_match_native(self):
        game = ROOT / 'game'
        if not (game / 'audio/sfx/FEET').is_file():
            self.skipTest('local GTA installation unavailable')
        subprocess.run(['make', '-f', 'tools/portable/Makefile',
                        'web/assets.mjs'], cwd=ROOT, check=True,
                       stdout=subprocess.DEVNULL)
        config = game / 'audio/CONFIG'
        names, banks = audio.sfx_tables(config)
        _, tracks = audio.stream_tables(config)
        sources = {
            'audio/config/pakfiles.dat': config / 'PakFiles.dat',
            'audio/config/banklkup.dat': config / 'BankLkup.dat',
            'audio/sfx/feet': game / 'audio/sfx/FEET',
            'audio/config/strmpaks.dat': config / 'StrmPaks.dat',
            'audio/config/traklkup.dat': config / 'TrakLkup.dat',
            'audio/streams/aa': game / 'audio/streams/AA'
        }
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            library = root / 'libassets.so'
            build_library(library)
            with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                raw = sources['audio/sfx/feet'].read_bytes()
                items = [(i, audio.convert_bank(
                    raw[off:off + size + audio.BANK_HEADER],
                    dict(sample_rate=11025, format='adpcm',
                         volume=1.0, max_seconds=0))[0])
                    for i, (_, off, size) in enumerate(banks[:2])]
                expected_pack = audio.pack_sound_banks(items)
                lookup = [(255, 0)] * len(banks)
                for i in range(2):
                    lookup[i] = (0, i)
                expected_lookup = audio.pack_sound_lookup(names, lookup)
                pk, off, size = tracks[0]
                self.assertEqual(pk, 0)
                stream = sources['audio/streams/aa'].read_bytes()
                sound = audio.convert_track(
                    stream[off:off + size + audio.TRACK_HEADER], off,
                    dict(sample_rate=16000, format='adpcm',
                         volume=1.0, max_seconds=0))[0]
                expected_stream = audio.pack_streams([(0, sound)])
            listed = root / 'files.json'
            listed.write_text(json.dumps({key: str(value)
                                          for key, value in sources.items()}))
            script = f"""
                import fs from 'node:fs';
                import createModule from
                    {json.dumps((ROOT / 'web/assets.mjs').as_uri())};
                import {{Parameters}} from
                    {json.dumps((ROOT / 'web/params.mjs').as_uri())};
                import {{convertSFX, convertStreams}} from
                    {json.dumps((ROOT / 'web/sound_assets.mjs').as_uri())};
                const list = JSON.parse(fs.readFileSync(process.argv[1], 'utf8'));
                const files = new Map(Object.entries(list).map(([name, filename]) =>
                    [name, new Blob([fs.readFileSync(filename)])]));
                const manifest = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
                const module = await createModule();
                const planner = new Parameters(manifest, files);
                const sfx = await convertSFX(module, planner, files,
                    new Set(), () => {{}}, 2);
                const streams = await convertStreams(module, planner, files,
                    new Set(), () => {{}}, 1);
                for (const entry of [...sfx.files, ...streams.files])
                    fs.writeFileSync(process.argv[3] + entry.path.split('/').at(-1),
                        new Uint8Array(await entry.file.arrayBuffer()));
                if (sfx.banks !== 2 || streams.tracks !== 1)
                    throw new Error('Unexpected audio counts.');
            """
            subprocess.run(['node', '--input-type=module', '-e', script,
                            str(listed), str(ROOT / 'web/resources.json'),
                            str(root) + '/'], cwd=ROOT, check=True)
            self.assertEqual((root / 'feet.dsfx').read_bytes(), expected_pack)
            self.assertEqual((root / 'banks.dsbl').read_bytes(), expected_lookup)
            self.assertEqual((root / 'aa.dsst').read_bytes(), expected_stream)

    @unittest.skipUnless(shutil.which('emcc') and shutil.which('cc'),
                         'Emscripten or C compiler unavailable')
    def test_cutscene_track_offset_is_realigned(self):
        game = ROOT / 'game'
        pack = game / 'audio/streams/CUTSCENE'
        if not pack.is_file():
            self.skipTest('local GTA installation unavailable')
        subprocess.run(['make', '-f', 'tools/portable/Makefile',
                        'web/assets.mjs'], cwd=ROOT, check=True,
                       stdout=subprocess.DEVNULL)
        names, tracks = audio.stream_tables(game / 'audio/CONFIG')
        self.assertEqual(names[tracks[706][0]], 'CUTSCENE')
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            library = root / 'libassets.so'
            build_library(library)
            _, offset, size = tracks[706]
            with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                position = audio.aligned_track_offset(pack, offset)
                with pack.open('rb') as stream:
                    stream.seek(position)
                    raw = stream.read(size + audio.TRACK_HEADER)
                sound = audio.convert_track(raw, position,
                    dict(sample_rate=11025, format='adpcm',
                         volume=1.0, max_seconds=0))[0]
                expected = audio.pack_streams([(706, sound)])
            sources = {
                'audio/config/strmpaks.dat':
                    game / 'audio/CONFIG/StrmPaks.dat',
                'audio/config/traklkup.dat':
                    game / 'audio/CONFIG/TrakLkup.dat',
                'audio/streams/cutscene': pack
            }
            listing = root / 'files.json'
            listing.write_text(json.dumps({key: str(value)
                                           for key, value in sources.items()}))
            script = f"""
                import fs from 'node:fs';
                import createModule from
                    {json.dumps((ROOT / 'web/assets.mjs').as_uri())};
                import {{Parameters}} from
                    {json.dumps((ROOT / 'web/params.mjs').as_uri())};
                import {{convertStreams}} from
                    {json.dumps((ROOT / 'web/sound_assets.mjs').as_uri())};
                const list = JSON.parse(fs.readFileSync(process.argv[1], 'utf8'));
                const files = new Map(Object.entries(list).map(([name, filename]) =>
                    [name, name.endsWith('cutscene') ? {{
                        size: fs.statSync(filename).size,
                        slice(start, end) {{
                            const size = Math.max(0, Math.min(this.size, end) - start);
                            const bytes = Buffer.allocUnsafe(size);
                            const fd = fs.openSync(filename, 'r');
                            try {{
                                const n = fs.readSync(fd, bytes, 0, size, start);
                                return new Blob([bytes.subarray(0, n)]);
                            }} finally {{fs.closeSync(fd);}}
                        }}
                    }} : new Blob([fs.readFileSync(filename)])]));
                const module = await createModule();
                const manifest = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
                const result = await convertStreams(module,
                    new Parameters(manifest, files), files, new Set(),
                    () => {{}}, 707);
                if (result.tracks !== 1) throw new Error('Cutscene track missing.');
                fs.writeFileSync(process.argv[3],
                    new Uint8Array(await result.files[0].file.arrayBuffer()));
            """
            output = root / 'cutscene.dsst'
            subprocess.run(['node', '--input-type=module', '-e', script,
                            str(listing), str(ROOT / 'web/resources.json'),
                            str(output)], cwd=ROOT, check=True)
            self.assertEqual(output.read_bytes(), expected)
