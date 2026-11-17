import assert from 'node:assert/strict';
import {existsSync} from 'node:fs';
import {open, readFile} from 'node:fs/promises';
import {resolve} from 'node:path';
import test from 'node:test';
import {convertLooseAssets} from '../../web/loose_assets.mjs';
import {imgEntries} from '../../web/img_assets.mjs';
import {Parameters} from '../../web/params.mjs';
import {convertRadar} from '../../web/radar_assets.mjs';

const root = resolve(import.meta.dirname, '../..');
const game = resolve(root, 'game');
const wasm = existsSync(resolve(root, 'web/assets.mjs'));

async function moduleAndManifest() {
    const {default: createModule} = await import('../../web/assets.mjs');
    return [await createModule(), JSON.parse(await readFile(resolve(root, 'web/resources.json')))];
}

test('real loose TXD converts while unrelated files retain their source Blob', {
    skip: !wasm || !existsSync(resolve(game, 'models/hud.txd')) ||
          !existsSync(resolve(root, 'nitrofs/models/hud.txd'))
}, async () => {
    const [module, manifest] = await moduleAndManifest();
    const source = await readFile(resolve(game, 'models/hud.txd'));
    const original = new Map([
        ['models/hud.txd', new Blob([source])],
        ['data/synthetic.dat', new Blob([source])]
    ]);
    const output = await convertLooseAssets(module,
        new Parameters(manifest, original), original, []);
    const converted = output.find(entry => entry.path === 'models/hud.txd');
    const raw = output.find(entry => entry.path === 'data/synthetic.dat');
    assert.ok(converted);
    assert.equal(raw.file, original.get('data/synthetic.dat'));
    assert.deepEqual(Buffer.from(await converted.file.arrayBuffer()),
        await readFile(resolve(root, 'nitrofs/models/hud.txd')));
});

test('real radar tiles have native pixel planes in the DSRT container', {
    skip: !wasm || !existsSync(resolve(game, 'models/gta3.img'))
}, async () => {
    const [module] = await moduleAndManifest();
    const source = resolve(game, 'models/gta3.img');
    const handle = await open(source);
    try {
        const file = {
            size: (await handle.stat()).size,
            slice(start, end) {
                return {arrayBuffer: async () => {
                    const bytes = Buffer.allocUnsafe(end - start);
                    const {bytesRead} = await handle.read(bytes, 0, bytes.length, start);
                    return bytes.subarray(0, bytesRead);
                }};
            }
        };
        const toc = await imgEntries(module, file);
        const tiles = [];
        for (let i = 0; i < 4; i++) {
            const entry = toc.find(({name}) => name.toLowerCase() ===
                `radar${String(i).padStart(2, '0')}.txd`);
            assert.ok(entry);
            const bytes = new Uint8Array(await file.slice(
                entry.offset, entry.offset + entry.size).arrayBuffer());
            const pointer = module._malloc(bytes.length);
            const result = module._malloc(8);
            let converted = 0;
            try {
                module.HEAPU8.set(bytes, pointer);
                assert.equal(module._radar_convert(pointer, bytes.length,
                                                  result, result + 4), 1);
                converted = module.HEAPU32[result >>> 2];
                const length = module.HEAPU32[(result + 4) >>> 2];
                tiles.push(Buffer.from(module.HEAPU8.slice(
                    converted, converted + length)));
            } finally {
                if (converted) module._free(converted);
                module._free(result);
                module._free(pointer);
            }
        }
        const actual = await convertRadar(module, file, 2);
        const full = 8192;
        const expected = Buffer.alloc(28 + 4 * (full + 2048));
        expected.write('DSRT');
        for (const [i, value] of [1, 4, 2, 64, 32, 0].entries())
            expected.writeUInt16LE(value, 4 + i * 2);
        expected.writeUInt32LE(28, 16);
        expected.writeUInt32LE(28 + 4 * full, 20);
        expected.fill(1, 24, 28);
        for (let i = 0; i < 4; i++) {
            tiles[i].copy(expected, 28 + i * full, 0, full);
            tiles[i].copy(expected, 28 + 4 * full + i * 2048, full);
        }
        assert.equal(actual.tiles, 4);
        assert.deepEqual(Buffer.from(actual.data), expected);
    } finally {
        await handle.close();
    }
});

test('cutscene DAT is extracted from IMG without installing the IMG itself', {
    skip: !wasm
}, async () => {
    const [module, manifest] = await moduleAndManifest();
    const index = Buffer.alloc(2048);
    index.write('VER2');
    index.writeUInt32LE(1, 4);
    index.writeUInt32LE(1, 8);
    index.writeUInt16LE(1, 12);
    index.writeUInt16LE(1, 14);
    index.write('PROLOG3.DAT', 16);
    const original = new Map([['anim/cuts.img', new Blob([
        index, Buffer.from('cutscene'), Buffer.alloc(2048 - 8)
    ])]]);
    const output = await convertLooseAssets(module,
        new Parameters(manifest, original), original, []);
    const extracted = output.find(entry => entry.path === 'anim/cuts/prolog3.dat');
    assert.ok(extracted);
    assert.deepEqual(Buffer.from(await extracted.file.arrayBuffer()),
        Buffer.concat([Buffer.from('cutscene'), Buffer.alloc(2048 - 8)]));
});
