import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import {mkdtemp, readFile, readdir} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';
import test from 'node:test';
import {exportManifest, parseResources,
    serializeManifest} from '../export_web_manifest.mjs';
import {directoryFiles, indexFiles, missingFiles, selectedFiles} from '../../web/folder.mjs';
import {match, Parameters} from '../../web/params.mjs';
import {latestRelease, downloadROM} from '../../web/releases.mjs';

const root = resolve(import.meta.dirname, '../..');

test('installer page uses English copy', async () => {
    const html = await readFile(resolve(root, 'web/index.html'), 'utf8');
    assert.ok(html.includes('<html lang="en">'));
    for (const text of ['Select GTA San Andreas folder', 'Download ZIP',
                        'Manual options', 'Version and changelog',
                        'Select missing data/handling.cfg',
                        'Current task', 'Overall progress'])
        assert.ok(html.includes(text), text);
    for (const path of ['index.html', ...(await readdir(resolve(root, 'web')))
        .filter(name => name.startsWith('local_') && name.endsWith('.mjs'))])
        assert.ok(!(await readFile(resolve(root, 'web', path), 'utf8'))
            .includes('in your browser'), path);
});

test('browser resource manifest omits local installation paths', async () => {
    const manifest = JSON.parse(await readFile(resolve(root, 'web/resources.json')));
    assert.deepEqual(Object.keys(manifest.global).sort(),
        ['audio_rate_scale', 'model_ratio', 'texture_scale']);
    assert.ok(Object.keys(manifest.defaults).includes('texture'));
    assert.ok(Object.keys(manifest.sections).includes('audio_streams'));
    assert.ok(Object.keys(manifest.sections).includes('radio_streams'));
    assert.equal(manifest.sections.radio_streams.sd_only, true);
    assert.ok(!JSON.stringify(manifest).includes('/mnt/c/'));
    assert.ok(!JSON.stringify(manifest).includes('Program Files'));
});

test('Node exports byte-for-byte browser rules from the same TOML source', async () => {
    const source = await readFile(resolve(root, 'resources.toml'), 'utf8');
    const expected = await readFile(resolve(root, 'web/resources.json'), 'utf8');
    assert.equal(serializeManifest(source), expected);
    const output = join(await mkdtemp(join(tmpdir(), 'gtasads-manifest-test-')),
        'resources.json');
    const run = spawnSync(process.execPath, [
        resolve(root, 'tools/export_web_manifest.mjs'), '--output', output
    ], {encoding: 'utf8'});
    assert.equal(run.status, 0, run.stderr);
    assert.equal(await readFile(output, 'utf8'), expected);
    assert.deepEqual(Object.keys(exportManifest(source).global).sort(),
        ['audio_rate_scale', 'model_ratio', 'texture_scale']);
    assert.ok(!expected.includes('/mnt/c/'));
});

test('manifest parser handles inline rules and rejects malformed input', () => {
    const parsed = parseResources(`
[global]
source = "/local/game" # not exported
texture_scale = 1.0
[defaults.model]
ratio = 0.5
[gta3_img]
kind = "img"
[[gta3_img.rule]]
match = ["foo.dff", "bar.dff"]
model = { ratio = 0.25, drop_frames = ["broken", "unused"] }
`);
    assert.deepEqual(parsed.gta3_img.rule[0].model,
        {ratio: 0.25, drop_frames: ['broken', 'unused']});
    assert.equal(exportManifest('[global]\nsource = "/local/game"\n')
        .global.model_ratio, 1);
    assert.throws(() => parseResources('[global]\ntexture_scale = strange\n'),
        /resources.toml:2/);
    assert.throws(() => parseResources('[global]\nsource = "ok"\nsource = "duplicate"\n'),
        /Invalid TOML assignment/);
});

test('browser rules preserve the legacy config reference for key selections', async () => {
    const manifest = JSON.parse(await readFile(resolve(root, 'web/resources.json')));
    const files = new Map(Object.entries({
        'data/vehicles.ide': '400, Infernus, Infernus, car, LANDSTAL\n',
        'data/peds.ide': 'peds\n1, family1, family1, CIVMALE, STAT_MAN, man, 0, 0\n',
        'data/default.ide': '10, m4, m4, weapon\n',
        'data/maps/LA/example.ide': '202, garden, garden, 100, 0\n'
    }).map(([path, value]) => [path, {text: async () => value}]));
    const planner = new Parameters(manifest, files);
    assert.equal(match('RADAR93.TXD', 'radar[0-9]*.txd'), true);
    assert.equal(match('RADARX.TXD', 'radar[0-9]*.txd'), false);
    const baseModel = {ratio: 0.5, min_tris: 64, max_tris: 0, decimate: 'cluster',
        min_area: 0, max_shift: 0, ground_qem: false, uvs: true,
        normals: false, vertex_colors: true, skin: true, drop_damage: true,
        drop_vlo: true, drop_frames: [], rigid_skin: false};
    const baseTexture = {scale: 0.5, max_size: 128, min_size: 8,
        format: 'auto', auto_colors: 16, alpha_threshold: 24,
        opaque: false, additive: false};
    const textureOverrides = [['*shad*', {max_size: 32}],
        ['*corona*', {max_size: 32}]];
    const cases = [
        ['gta3_img', 'model', 'infernus.dff',
            {params: {...baseModel, ratio: 0.25, min_tris: 24, max_tris: 320,
                normals: true, drop_damage: false},
            overrides: textureOverrides, removed: false}],
        ['gta3_img', 'texture', 'radar93.txd',
            {params: baseTexture, overrides: textureOverrides, removed: true}],
        ['gta3_img', 'model', 'family1.dff',
            {params: {...baseModel, ratio: 0.25, max_tris: 300,
                decimate: 'wedge'}, overrides: textureOverrides, removed: false}],
        ['gta3_img', 'model', 'garden.dff',
            {params: {...baseModel, ratio: 0.25, min_tris: 12,
                max_tris: 192, min_area: 0.85, max_shift: 0.5,
                ground_qem: true},
             overrides: ['*road*', '*pave*', '*sidew*', '*grass*', '*dirt*',
                 '*conc*', '*tar*', '*kerb*', '*curb*']
                 .map(pattern => [pattern, {max_size: 64}])
                 .concat(textureOverrides), removed: false}],
        ['models_loose', 'texture', 'models/particle.txd',
            {params: {...baseTexture, max_size: 32},
             overrides: ['coronastar', 'coronamoon', 'cloud1']
                 .map(pattern => [pattern, {max_size: 32, format: 'a5i3',
                                            additive: true}]),
             removed: false}],
        ['models_loose', 'texture', 'models/fronten1.txd',
            {params: {...baseTexture, max_size: 256, auto_colors: 256},
             overrides: [], removed: false}],
        ['audio_sfx', 'audio', 'SCRIPT',
            {params: {sample_rate: 8000, format: 'adpcm',
                      max_seconds: 0, volume: 1}, overrides: [], removed: false}],
        ['audio_sfx', 'audio', 'SPC_EA',
            {params: {sample_rate: 11025, format: 'adpcm',
                      max_seconds: 0, volume: 1}, overrides: [], removed: true}],
        ['audio_streams', 'audio', 'CUTSCENE',
            {params: {sample_rate: 11025, format: 'adpcm',
                      max_seconds: 0, volume: 1, track_ids: [706, 708]},
             overrides: [], removed: false}]
    ];
    for (const [section, handler, entry, expected] of cases)
        assert.deepEqual(await planner.params(section, handler, entry), expected,
            `${section}:${entry}`);
});

test('browser release and folder checks reject malformed files and ROMs', async () => {
    const files = [
        {path: 'MODELS\\GTA3.IMG', file: {name: 'gta3.img'}},
        {path: 'models/gta_int.img', file: {}},
        {path: 'anim/anim.img', file: {}},
        {path: 'data/gta.dat', file: {}},
        {path: 'text/american.gxt', file: {}}
    ];
    assert.deepEqual(missingFiles(indexFiles(files)), []);
    assert.equal(selectedFiles([{webkitRelativePath:
        'Grand Theft Auto San Andreas/models/gta3.img'}])[0].path,
    'models/gta3.img');
    assert.throws(() => indexFiles([...files,
        {path: 'models/gta3.img', file: {}}]));
    assert.throws(() => indexFiles([{path: '../escape', file: {}}]));
    const nested = {async *entries() {
        yield ['data', {kind: 'directory', async *entries() {
            yield ['gta.dat', {kind: 'file',
                getFile: async () => ({name: 'gta.dat'})}];
        }}];
    }};
    const selected = [];
    for await (const file of directoryFiles(nested)) selected.push(file);
    assert.deepEqual(selected, [
        {path: 'data/gta.dat', file: {name: 'gta.dat'}}
    ]);
    const bytes = Uint8Array.of(1, 2, 3);
    const digest = createHash('sha256').update(bytes).digest('hex');
    const release = await latestRelease(async () => new Response(JSON.stringify({
        tag_name: 'v1', body: 'Cambios', assets: [
            {name: 'gtasads.nds', size: 3, id: 123, digest: `sha256:${digest}`,
             browser_download_url:
                'https://github.com/sammwyy/gtasands/releases/download/v1/gtasads.nds'},
            {name: 'fat-gtasads.nds', size: 4,
             browser_download_url:
                'https://github.com/sammwyy/gtasands/releases/download/v1/fat-gtasads.nds'},
            {name: 'gtasads.dsi', size: 16000001,
             browser_download_url: 'https://example.invalid/gtasads.dsi'}
        ]
    }), {status: 200}));
    assert.equal(release.tag, 'v1');
    assert.equal(release.notes, 'Cambios');
    assert.deepEqual(Object.keys(release.assets), ['nds']);
    const rom = await downloadROM(release.assets.nds, async url => {
        assert.ok(url.endsWith('/roms/123/gtasads.nds'));
        return new Response(bytes);
    });
    assert.deepEqual([...rom], [1, 2, 3]);
    await assert.rejects(downloadROM(release.assets.nds,
        async () => new Response(Uint8Array.of(1, 2))));
    const empty = await latestRelease(async () => new Response(null, {status: 404}));
    assert.equal(empty.tag, null);
});
