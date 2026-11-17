import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {existsSync} from 'node:fs';
import {open, readFile} from 'node:fs/promises';
import {join, resolve} from 'node:path';
import test from 'node:test';
import {imgEntries} from '../../web/img_assets.mjs';
import {convertModel} from '../../web/native_convert.mjs';

const root = resolve(import.meta.dirname, '../..');
const wasm = existsSync(join(root, 'web/assets.mjs'));
const checksum = bytes => createHash('sha256').update(bytes).digest('hex');

test('welded wedge decimation agrees with Python at three triangle limits', {
    skip: !wasm
}, async () => {
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const vertices = module._malloc(144 * 24);
    const triangles = module._malloc(242 * 16);
    const pointers = module._malloc(8);
    let result = 0;
    try {
        module.HEAPU8.fill(0, vertices, vertices + 144 * 24);
        module.HEAPU8.fill(0, triangles, triangles + 242 * 16);
        const view = new DataView(module.HEAPU8.buffer);
        for (let y = 0; y < 12; y++)
            for (let x = 0; x < 12; x++) {
                const p = vertices + (y * 12 + x) * 24;
                view.setFloat64(p, x * 0.5, true);
                view.setFloat64(p + 8, y * 0.5, true);
                view.setFloat64(p + 16, 0.1 * ((x + y) % 3), true);
            }
        for (let y = 0; y < 11; y++)
            for (let x = 0; x < 11; x++) {
                const a = y * 12 + x;
                const p = triangles + (y * 11 + x) * 32;
                for (const [i, values] of [
                    [a, a + 1, a + 12], [a + 1, a + 13, a + 12]
                ].entries())
                    for (const [j, value] of values.entries())
                        view.setUint32(p + i * 16 + j * 4, value, true);
            }
        for (const [target, count, golden] of [
            [30, 42, 'dc89b91544a80c8dfc17f65efc57f93b53714914807b96112aa472a0c67798e4'],
            [100, 100, '6f86153c09e42f38a02592e706e8cfeb9002ae77820e4b24b9c2dd946f6561b9'],
            [242, 242, '2324185fab615c5833bd6f144b5ab9d8e99e29289d4af3c4a27045a79cfa958c']
        ]) {
            assert.equal(module._wedge_decimate(vertices, 144,
                triangles, 242, target, pointers, pointers + 4), 1);
            const memory = new DataView(module.HEAPU8.buffer);
            result = memory.getUint32(pointers, true);
            const outputCount = memory.getUint32(pointers + 4, true);
            assert.equal(outputCount, count);
            const packed = Buffer.alloc(count * 16);
            for (let i = 0; i < count; i++) {
                const p = result + i * 16;
                for (let j = 0; j < 3; j++)
                    packed.writeUInt32LE(memory.getUint32(p + j * 4, true),
                        i * 16 + j * 4);
                packed.writeUInt16LE(memory.getUint16(p + 12, true),
                    i * 16 + 12);
            }
            assert.equal(checksum(packed), golden, `target ${target}`);
            module._free(result);
            result = 0;
        }
    } finally {
        if (result) module._free(result);
        module._free(vertices);
        module._free(triangles);
        module._free(pointers);
    }
});

test('real seabed ground mesh matches Python wedge output', {
    skip: !wasm || !existsSync(join(root, 'game/models/gta3.img'))
}, async () => {
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const path = join(root, 'game/models/gta3.img');
    const file = await open(path, 'r');
    try {
        const read = async (start, length) => {
            const bytes = Buffer.alloc(length);
            let offset = 0;
            while (offset < length) {
                const {bytesRead} = await file.read(bytes,
                    offset, length - offset, start + offset);
                if (!bytesRead) throw new Error('Truncated GTA IMG');
                offset += bytesRead;
            }
            return bytes;
        };
        const wrapper = {
            size: (await file.stat()).size,
            slice(start, end) {
                return {async arrayBuffer() {
                    const bytes = await read(start, end - start);
                    return bytes.buffer.slice(bytes.byteOffset,
                        bytes.byteOffset + bytes.byteLength);
                }};
            }
        };
        const entry = (await imgEntries(module, wrapper))
            .find(({name}) => name === 'sbseabed_08_sfse.dff');
        assert.ok(entry);
        const source = await read(entry.offset, entry.size);
        const defaults = JSON.parse(await readFile(join(root,
            'web/resources.json'))).defaults.model;
        const output = Buffer.from(convertModel(module, source,
            {...defaults, ratio: 0.37, max_tris: 120, ground_qem: true,
                drop_damage: false}));
        assert.equal(output.subarray(0, 4).toString(), 'DMDL');
        if (checksum(source) ===
            '5c4fcfb905411a4e23ed65a1740646292367058983a829f2c5291c809ac33624') {
            assert.equal(output.length, 3284);
            assert.equal(checksum(output),
                '606179bb67533489eef0d9764f38171092e22315e9c6a52853c04baf6cde6b38');
        }
    } finally {
        await file.close();
    }
});
