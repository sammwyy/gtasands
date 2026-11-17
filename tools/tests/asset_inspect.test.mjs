import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import {existsSync} from 'node:fs';
import {mkdtemp, readFile} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';
import test from 'node:test';
import {inflateSync} from 'node:zlib';
import {decodeDTEX, inspectTextures} from '../asset_inspect.mjs';

const root = resolve(import.meta.dirname, '../..');
const wasm = existsSync(join(root, 'web/assets.mjs'));
const sha = bytes => createHash('sha256').update(bytes).digest('hex');

function invoke(args) {
    const result = spawnSync('node', ['tools/assets.mjs', ...args],
        {cwd: root, encoding: 'utf8', maxBuffer: 2 ** 22});
    assert.equal(result.status, 0, result.stderr);
    return result.stdout;
}

function pngPixels(data) {
    assert.equal(data.subarray(0, 8).toString('hex'),
        '89504e470d0a1a0a');
    const width = data.readUInt32BE(16), height = data.readUInt32BE(20);
    const blocks = [];
    for (let p = 8; p < data.length;) {
        const length = data.readUInt32BE(p);
        if (data.toString('latin1', p + 4, p + 8) === 'IDAT')
            blocks.push(data.subarray(p + 8, p + 8 + length));
        p += length + 12;
    }
    const raw = inflateSync(Buffer.concat(blocks));
    const pixels = Buffer.alloc(width * height * 4);
    for (let y = 0; y < height; y++) {
        assert.equal(raw[y * (1 + width * 4)], 0);
        raw.copy(pixels, y * width * 4,
            y * (1 + width * 4) + 1, (y + 1) * (1 + width * 4));
    }
    return {width, height, pixels};
}

test('native CLI lists IMG rules and inspects archive, model, TXD, COL and IFP', {
    skip: !wasm || !existsSync(join(root, 'game/models/gta3.img'))
}, () => {
    const listing = invoke(['list', 'gta3_img', '-n', '2']);
    assert.equal(listing.trim().split('\n').length, 2);
    assert.match(listing, /vgwsthiway1\.txd.*texture.*scale=/);
    assert.match(invoke(['info', 'models/gta3.img']),
        /gta3\.img: 16316 entries/);
    assert.match(invoke(['info', 'models/gta3.img:bbb_lr_slv1.dff']),
        /1 frames, 1 geometries, 1 atomics/);
    assert.match(invoke(['info', 'models/hud.txd']),
        /radardisc 64x64/);
    assert.match(invoke(['info', 'anim/ped.ifp']),
        /block ped, 294 animations/);
    assert.match(invoke(['info', 'models/coll/peds.col']),
        /COL cop faces=8/);
    const missing = spawnSync('node', ['tools/assets.mjs',
        'info', 'models/gta3.img:not_here.txd'],
    {cwd: root, encoding: 'utf8'});
    assert.notEqual(missing.status, 0);
    assert.match(missing.stderr, /not found/);
});

test('native TXD PNG previews retain Python RGBA for original and DS pixels', {
    skip: !wasm || !existsSync(join(root, 'game/models/hud.txd'))
}, async () => {
    const output = join(await mkdtemp(join(tmpdir(), 'gtasads-asset-preview-')),
        'images');
    const text = invoke(['preview', 'models/hud.txd', 'radardisc',
        '--section', 'models_loose', '--output', output]);
    assert.match(text, /radardisc: 64x64 -> 32x32/);
    const input = await readFile(join(root, 'game/models/hud.txd'));
    const original = pngPixels(await readFile(join(output, 'radardisc_orig.png')));
    const converted = pngPixels(await readFile(join(output, 'radardisc_ds.png')));
    assert.deepEqual([original.width, original.height,
        converted.width, converted.height], [64, 64, 32, 32]);
    if (sha(input) ===
        'de3d9f867ccd3a7cc7d35f1cb6c43659d8abec7cbd68991f09dd30b872aab4f2') {
        assert.equal(sha(original.pixels),
            'b6968a89fe94050b1ca0dc58742fcc93a7367761d6540847cc0434b6cdff1585');
        assert.equal(sha(converted.pixels),
            'b37e1bb4fdfb291e6d5f3dc4767d29dcd8453594945974fcfea7d628ea0e06bc');
    }
});

test('DTEX decoder rejects malformed dimensions, palette indices and truncation', {
    skip: !wasm || !existsSync(join(root, 'game/models/hud.txd'))
}, async () => {
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const hud = await readFile(join(root, 'game/models/hud.txd'));
    const textures = inspectTextures(module, hud);
    assert.equal(textures[0].name, 'radardisc');
    assert.equal(textures[0].width, 64);
    const header = Buffer.alloc(22);
    header.write('DTEX');
    header.writeUInt16LE(1, 4);
    header.writeUInt16LE(1, 6);
    header[8] = 4;
    header.writeUInt16LE(1, 10);
    header.writeUInt32LE(1, 12);
    header[16] = 2;
    assert.throws(() => decodeDTEX(header), /palette/);
    header[16] = 0;
    assert.deepEqual([...decodeDTEX(header).pixels], [0, 0, 0, 255]);
    header.writeUInt32LE(0, 12);
    assert.throws(() => decodeDTEX(header), /Truncated/);
});
