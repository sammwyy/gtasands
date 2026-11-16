import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from . import ds_mesh, ds_misc, ds_texture, img
from .dsfmt import write_pack
from .pipeline import Config
from .test_portable_build import ROOT


@unittest.skipUnless(shutil.which('node'), 'Node unavailable')
class WebIMGTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which('emcc') and shutil.which('cc'),
                         'Emscripten or C compiler unavailable')
    def test_real_ground_models_match_python_in_wasm(self):
        archive = ROOT / 'game/models/gta3.img'
        if not archive.is_file():
            self.skipTest('local GTA installation unavailable')
        from .test_portable_build import build_library
        import os
        from unittest.mock import patch
        config = Config(ROOT / 'resources.toml')
        config.game = ROOT / 'game'
        wanted = {'cw_barnie.dff', 'gaz23_law.dff',
                  'groundbit84_sfs.dff', 'newcomprd_las2.dff',
                  'road_hill04b.dff', 'vegaseroad022.dff',
                  'vgssairportland12.dff'}
        entries = [(name, off, size) for name, off, size in img.read_toc(archive)
                   if name in wanted]
        self.assertEqual({name for name, _, _ in entries}, wanted)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            library = root / 'libassets.so'
            build_library(library)
            payload = []
            with archive.open('rb') as source:
                for name, off, size in entries:
                    source.seek(off)
                    data = source.read(size)
                    params, _, _ = config.params(
                        config.sections['gta3_img'], 'model', name)
                    with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                        expected = ds_mesh.convert_dff(data, params)[0]
                    with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                        self.assertEqual(ds_mesh.convert_dff(data, params)[0],
                                         expected)
                    infile, outfile = root / name, root / (name + '.dmdl')
                    infile.write_bytes(data)
                    outfile.write_bytes(expected)
                    payload.append({'source': str(infile), 'expected': str(outfile),
                                    'params': params})
            cases = root / 'cases.json'
            cases.write_text(json.dumps(payload))
            subprocess.run(['make', '-f', 'tools/portable/Makefile',
                            'web/assets.mjs'], cwd=ROOT, check=True,
                           stdout=subprocess.DEVNULL)
            script = f"""
                import fs from 'node:fs';
                import createModule from
                    {json.dumps((ROOT / 'web/assets.mjs').as_uri())};
                import {{convertModel}} from
                    {json.dumps((ROOT / 'web/native_convert.mjs').as_uri())};
                const cases = JSON.parse(fs.readFileSync(process.argv[1], 'utf8'));
                const module = await createModule();
                for (const item of cases) {{
                    const source = new Uint8Array(fs.readFileSync(item.source));
                    const expected = fs.readFileSync(item.expected);
                    const actual = convertModel(module, source, item.params);
                    if (!Buffer.from(actual).equals(expected))
                        throw new Error(`WASM model differs: ${{item.source}}`);
                }}
            """
            subprocess.run(['node', '--input-type=module', '-e', script,
                            str(cases)], cwd=ROOT, check=True)

    def test_missing_img_archives_are_built_locally(self):
        script = f"""
            import {{convertOriginalIMGs}} from
                {json.dumps((ROOT / 'web/local_imgs.mjs').as_uri())};
            import {{strict as assert}} from 'node:assert';
            const manifest = {{sections: {{
                gta3_img: {{src: 'models/gta3.img', out: 'models/gta3.dspk'}},
                gta_int_img: {{src: 'models/gta_int.img', out: 'models/gta_int.dspk'}},
                player_img: {{src: 'models/player.img', out: 'models/player.dspk'}},
                anim_img: {{src: 'anim/anim.img', out: 'anim/anim.dspk'}}
            }}}};
            const original = new Map(Object.values(manifest.sections).map(section =>
                [section.src, new Blob([])]));
            let terminated = false;
            const workerFactory = () => ({{
                postMessage({{files, section}}) {{
                    assert.equal(files, original);
                    queueMicrotask(() => this.onmessage({{
                        data: {{path: manifest.sections[section].out,
                                file: new Blob(['DSPK']), entries: 1, skipped: 0}}
                    }}));
                }},
                terminate() {{terminated = true;}}
            }});
            const built = await convertOriginalIMGs(original, [], manifest,
                                                    () => {{}}, workerFactory);
            assert.equal(built.length, 4);
            assert.equal(terminated, true);
            assert.deepEqual(await convertOriginalIMGs(original, built, manifest,
                () => assert.fail('already converted'),
                () => assert.fail('worker not needed')), []);
        """
        subprocess.run(['node', '--input-type=module', '-e', script], check=True)

    @unittest.skipUnless(shutil.which('cc'), 'C compiler unavailable')
    def test_wasm_img_package_matches_python(self):
        if not (ROOT / 'game/models/gta_int.img').is_file():
            self.skipTest('local GTA installation unavailable')
        from .test_portable_build import build_library
        config = Config(ROOT / 'resources.toml')
        section = config.sections['gta_int_img']
        archive = ROOT / 'game' / section['src']
        toc = {name: (off, size) for name, off, size in img.read_toc(archive)}
        names = ['8stad.txd', '8bar5.dff', 'gen_int2_3.col', 'gen_int5.ifp']
        with tempfile.TemporaryDirectory() as tmp:
            import os
            import struct
            from unittest.mock import patch
            root = Path(tmp)
            library = root / 'libassets.so'
            build_library(library)
            sources = {name: img.read_entry(archive, *toc[name]) for name in names}
            sources['sample.dat'] = b'local copy' + bytes(2048 - 10)
            index = bytearray(2048)
            struct.pack_into('<4sI', index, 0, b'VER2', len(sources))
            parts = [index]
            sector = 1
            for i, (name, data) in enumerate(sources.items()):
                count = (len(data) + 2047) // 2048
                struct.pack_into('<IHH24s', index, 8 + i * 32,
                                 sector, count, count, name.encode('ascii'))
                parts.append(data.ljust(count * 2048, b'\0'))
                sector += count
            original = root / 'sample.img'
            original.write_bytes(b''.join(parts))
            with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                results = {}
                for name, data in sources.items():
                    handler = {'.txd': 'texture', '.dff': 'model',
                               '.col': 'collision', '.ifp': 'anim'}.get(
                                   Path(name).suffix, 'copy')
                    params, overrides, removed = config.params(
                        section, handler if handler != 'copy' else 'texture',
                        name)
                    if removed:
                        continue
                    convert = {
                        'texture': lambda: ds_texture.convert_txd(data, params, overrides),
                        'model': lambda: ds_mesh.convert_dff(data, params)[0],
                        'collision': lambda: ds_misc.convert_col(data, params)[0],
                        'anim': lambda: ds_misc.convert_ifp(data, params)[0],
                        'copy': lambda: data
                    }
                    results[name] = convert[handler]()
                expected = root / 'expected.dspk'
                write_pack(expected, results)
            script = f"""
                import fs from 'node:fs';
                import createModule from
                    {json.dumps((ROOT / 'web/assets.mjs').as_uri())};
                import {{Parameters}} from
                    {json.dumps((ROOT / 'web/params.mjs').as_uri())};
                import {{convertIMG}} from
                    {json.dumps((ROOT / 'web/img_assets.mjs').as_uri())};
                const module = await createModule();
                const manifest = JSON.parse(fs.readFileSync(process.argv[1], 'utf8'));
                const planner = new Parameters(manifest, new Map());
                const source = new Blob([fs.readFileSync(process.argv[2])]);
                const result = await convertIMG(module, planner,
                                                'gta_int_img', source);
                fs.writeFileSync(process.argv[3],
                    new Uint8Array(await result.blob.arrayBuffer()));
            """
            subprocess.run(['node', '--input-type=module', '-e', script,
                            str(ROOT / 'web/resources.json'), str(original),
                            str(root / 'actual.dspk')], check=True, cwd=ROOT)
            self.assertEqual((root / 'actual.dspk').read_bytes(),
                             expected.read_bytes())
