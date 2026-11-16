import hashlib
import io
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from .pipeline import Config
from tools.mirror_rom_release import mirror


ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which('node'), 'Node.js unavailable')
class WebInstallerTest(unittest.TestCase):
    def test_same_origin_release_mirror_verifies_rom_hash(self):
        data = b'code-only-rom'
        asset = {
            'name': 'gtasads.nds', 'id': 123, 'size': len(data),
            'digest': 'sha256:' + hashlib.sha256(data).hexdigest(),
            'browser_download_url':
                'https://github.com/sammwyy/gtasands/releases/download/v1/gtasads.nds'
        }
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / 'roms'
            bad = {**asset, 'digest': 'sha256:' + '0' * 64}
            with self.assertRaises(ValueError):
                mirror({'assets': [bad]}, output,
                       opener=lambda url: io.BytesIO(data))
            target = output / '123/gtasads.nds'
            self.assertFalse(target.exists())
            self.assertEqual(mirror({'assets': [asset]}, output,
                                    opener=lambda url: io.BytesIO(data)),
                             [target])
            self.assertEqual(target.read_bytes(), data)
            self.assertEqual(mirror({'assets': [asset]}, output,
                                    opener=lambda url: self.fail('unexpected download')),
                             [target])

    def test_browser_resource_rules_match_python(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = Path(tmp)
            sources = {
                'data/vehicles.ide': '400, Infernus, Infernus, car, LANDSTAL\n',
                'data/peds.ide': 'peds\n1, family1, family1, CIVMALE, STAT_MAN, man, 0, 0\n',
                'data/default.ide': '10, m4, m4, weapon\n',
                'data/maps/LA/example.ide': '202, garden, garden, 100, 0\n'
            }
            for name, text in sources.items():
                path = game / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text)
            config = Config(ROOT / 'resources.toml')
            config.game = game
            cases = []
            for section, handler, entry in (
                    ('gta3_img', 'model', 'infernus.dff'),
                    ('gta3_img', 'texture', 'radar93.txd'),
                    ('gta3_img', 'model', 'family1.dff'),
                    ('gta3_img', 'model', 'garden.dff'),
                    ('models_loose', 'texture', 'models/particle.txd'),
                    ('models_loose', 'texture', 'models/fronten1.txd'),
                    ('audio_sfx', 'audio', 'SCRIPT'),
                    ('audio_sfx', 'audio', 'SPC_EA'),
                    ('audio_streams', 'audio', 'CUTSCENE')):
                params, overrides, removed = config.params(
                    config.sections[section], handler, entry)
                cases.append(dict(section=section, handler=handler, entry=entry,
                                  expected=dict(params=params, overrides=overrides,
                                                removed=removed)))
            payload = dict(files=sources, cases=cases)
            script = """
                import {Parameters, match} from MODULE_URI;
                import {readFileSync} from 'node:fs';
                import {strict as assert} from 'node:assert';
                const fixture = JSON.parse(readFileSync(0, 'utf8'));
                const manifest = JSON.parse(readFileSync(MANIFEST_PATH, 'utf8'));
                const files = new Map(Object.entries(fixture.files).map(([path, text]) =>
                    [path, {text: async () => text}]));
                const planner = new Parameters(manifest, files);
                assert.equal(match('RADAR93.TXD', 'radar[0-9]*.txd'), true);
                assert.equal(match('RADARX.TXD', 'radar[0-9]*.txd'), false);
                for (const item of fixture.cases)
                    assert.deepEqual(await planner.params(item.section, item.handler,
                                                          item.entry), item.expected,
                                     `${item.section}:${item.entry}`);
            """.replace('MODULE_URI', json.dumps((ROOT / 'web/params.mjs').as_uri())).replace(
                'MANIFEST_PATH', json.dumps(str(ROOT / 'web/resources.json')))
            subprocess.run(['node', '--input-type=module', '-e', script],
                           input=json.dumps(payload), text=True, check=True)

    def test_exported_browser_rules_match_python_without_local_paths(self):
        config = Config(ROOT / 'resources.toml')
        expected = json.loads((ROOT / 'web/resources.json').read_text())
        self.assertEqual(expected['defaults'], config.defaults)
        self.assertEqual(expected['sections'], config.sections)
        self.assertEqual(set(expected['global']),
                         {'texture_scale', 'model_ratio', 'audio_rate_scale'})
        with tempfile.TemporaryDirectory() as tmp:
            generated = Path(tmp) / 'resources.json'
            subprocess.run([sys.executable,
                            str(ROOT / 'tools/export_web_manifest.py'),
                            '--output', str(generated)], check=True)
            self.assertEqual(generated.read_bytes(),
                             (ROOT / 'web/resources.json').read_bytes())

    def test_release_and_folder_validation(self):
        digest = hashlib.sha256(bytes([1, 2, 3])).hexdigest()
        script = f"""
            import {{latestRelease, downloadROM}} from
                {json.dumps((ROOT / 'web/releases.mjs').as_uri())};
            import {{directoryFiles, indexFiles, missingFiles, selectedFiles}} from
                {json.dumps((ROOT / 'web/folder.mjs').as_uri())};
            import {{strict as assert}} from 'node:assert';
            const files = [
                {{path: 'MODELS\\\\GTA3.IMG', file: {{name: 'gta3.img'}}}},
                {{path: 'models/gta_int.img', file: {{}}}},
                {{path: 'anim/anim.img', file: {{}}}},
                {{path: 'data/gta.dat', file: {{}}}},
                {{path: 'text/american.gxt', file: {{}}}}
            ];
            assert.deepEqual(missingFiles(indexFiles(files)), []);
            assert.equal(selectedFiles([{{webkitRelativePath:
                'Grand Theft Auto San Andreas/models/gta3.img'}}])[0].path,
                'models/gta3.img');
            assert.throws(() => indexFiles([...files, {{
                path: 'models/gta3.img', file: {{}}}}]));
            assert.throws(() => indexFiles([{{path: '../escape', file: {{}}}}]));
            const nested = {{
                async *entries() {{
                    yield ['data', {{
                        kind: 'directory',
                        async *entries() {{
                            yield ['gta.dat', {{kind: 'file',
                                getFile: async () => ({{name: 'gta.dat'}})}}];
                        }}
                    }}];
                }}
            }};
            const selected = [];
            for await (const file of directoryFiles(nested)) selected.push(file);
            assert.deepEqual(selected, [{{path: 'data/gta.dat',
                                          file: {{name: 'gta.dat'}}}}]);
            const release = await latestRelease(async () => new Response(JSON.stringify({{
                tag_name: 'v1', body: 'Cambios',
                assets: [{{name: 'gtasads.nds', size: 3, id: 123,
                    digest: 'sha256:{digest}',
                    browser_download_url:
                    'https://github.com/sammwyy/gtasands/releases/download/v1/gtasads.nds'}},
                    {{name: 'fat-gtasads.nds', size: 4,
                    browser_download_url:
                    'https://github.com/sammwyy/gtasands/releases/download/v1/fat-gtasads.nds'}},
                    {{name: 'gtasads.dsi', size: 16000001,
                    browser_download_url: 'https://example.invalid/gtasads.dsi'}}]
            }}), {{status: 200}}));
            assert.equal(release.tag, 'v1');
            assert.equal(release.notes, 'Cambios');
            assert.deepEqual(Object.keys(release.assets), ['nds']);
            const rom = await downloadROM(release.assets.nds,
                async url => {{
                    assert.ok(url.endsWith('/roms/123/gtasads.nds'));
                    return new Response(Uint8Array.from([1, 2, 3]));
                }});
            assert.deepEqual([...rom], [1, 2, 3]);
            await assert.rejects(downloadROM(release.assets.nds,
                async () => new Response(Uint8Array.from([1, 2]))));
            const empty = await latestRelease(async () => new Response(null, {{status: 404}}));
            assert.equal(empty.tag, null);
        """
        subprocess.run(['node', '--input-type=module', '-e', script], check=True)
