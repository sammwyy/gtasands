import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {existsSync} from 'node:fs';
import {open, stat} from 'node:fs/promises';
import {join, resolve} from 'node:path';
import test from 'node:test';
import {imgEntries} from '../../web/img_assets.mjs';

const root = resolve(import.meta.dirname, '../..');
const wasm = existsSync(join(root, 'web/assets.mjs'));
const checksum = bytes => createHash('sha256').update(bytes).digest('hex');

test('world collision faces, spheres and boxes match Python packed record', {
    skip: !wasm
}, async () => {
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const vertices = module._malloc(11 * 24);
    const faces = module._malloc(9 * 16);
    const spheres = module._malloc(40);
    const boxes = module._malloc(56);
    const model = module._malloc(32);
    const pointers = module._malloc(16);
    let output = 0;
    try {
        module.HEAPU8.fill(0, vertices, vertices + 11 * 24);
        module.HEAPU8.fill(0, faces, faces + 9 * 16);
        module.HEAPU8.fill(0, spheres, spheres + 40);
        module.HEAPU8.fill(0, boxes, boxes + 56);
        const view = new DataView(module.HEAPU8.buffer);
        for (let i = 0; i < 11; i++) {
            const p = vertices + i * 24;
            for (const [j, value] of [
                i % 3 * 1.5, Math.trunc(i / 3) * 1.5,
                0.25 * (i % 2)
            ].entries()) view.setFloat64(p + j * 8, value, true);
        }
        for (let i = 0; i < 9; i++) {
            const p = faces + i * 16;
            for (let j = 0; j < 3; j++)
                view.setUint32(p + j * 4, i + j, true);
            view.setUint8(p + 12, i % 3);
        }
        for (const [i, value] of [2, 3, 0, 2].entries())
            view.setFloat64(spheres + i * 8, value, true);
        module.HEAPU8.set([4, 0, 0, 1], spheres + 32);
        for (const [i, value] of [-1, 0, -1, 1, 2, 1].entries())
            view.setFloat64(boxes + i * 8, value, true);
        module.HEAPU8.set([3, 0, 0, 2], boxes + 48);
        for (const [i, [pointer, count]] of [
            [vertices, 11], [faces, 9], [spheres, 1], [boxes, 1]
        ].entries()) {
            view.setUint32(model + i * 8, pointer, true);
            view.setUint32(model + i * 8 + 4, count, true);
        }
        assert.equal(module._worldcol_pack(model, pointers,
            pointers + 4, pointers + 8, pointers + 12), 1);
        const result = new DataView(module.HEAPU8.buffer);
        output = result.getUint32(pointers, true);
        const length = result.getUint32(pointers + 4, true);
        assert.deepEqual([length, result.getUint16(pointers + 8, true),
            result.getUint32(pointers + 12, true)], [312, 108, 9]);
        assert.equal(checksum(module.HEAPU8.slice(output, output + length)),
            'ab0d74e7b0651dc3a7a888b397612ff31e2e0839ceae03e039c12403f2fea53e');
    } finally {
        if (output) module._free(output);
        for (const address of [vertices, faces, spheres, boxes, model, pointers])
            module._free(address);
    }
});

test('real COL archives preserve every Python record, including empty models', {
    skip: !wasm || !existsSync(join(root, 'game/models/gta3.img'))
}, async () => {
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const path = join(root, 'game/models/gta3.img');
    const handle = await open(path, 'r');
    try {
        const readRange = async (start, length) => {
            const data = Buffer.alloc(length);
            let offset = 0;
            while (offset < length) {
                const {bytesRead} = await handle.read(data,
                    offset, length - offset, start + offset);
                if (!bytesRead) throw new Error('Truncated IMG archive');
                offset += bytesRead;
            }
            return data;
        };
        const file = {
            size: (await stat(path)).size,
            slice(start, end) {
                return {async arrayBuffer() {
                    const data = await readRange(start, end - start);
                    return data.buffer.slice(data.byteOffset,
                        data.byteOffset + data.byteLength);
                }};
            }
        };
        const expected = {
            'levelmap_1.col': [
                '78857e52b819031b249cc7d4b4f232a643d1c4eab887e45170fd98a0187844e5',
                84, '3ba25df92f29b3abea5b88b269e5ee7ae0a446515b79f5fef3bdf37e171749fb'],
            'seabed1.col': [
                '98470d8b374d6f2e76c3ff4bfcf716d4337be2d848648225689a1412233738cf',
                17, '7d220650723310f4acfaae886b8ec28e3744aec29a97714095b2fb4ee70c835f']
        };
        let compared = 0;
        for (const {name, offset, size} of await imgEntries(module, file)) {
            if (!(name in expected)) continue;
            const data = await readRange(offset, size);
            const input = module._malloc(data.length);
            const pointers = module._malloc(8);
            let records = 0, count = 0;
            try {
                module.HEAPU8.set(data, input);
                assert.equal(module._worldcol_parse_archive(input, data.length,
                    pointers, pointers + 4), 1, name);
                const view = new DataView(module.HEAPU8.buffer);
                records = view.getUint32(pointers, true);
                count = view.getUint32(pointers + 4, true);
                const hash = createHash('sha256');
                for (let i = 0; i < count; i++) {
                    const p = records + i * 40;
                    const nameBytes = Buffer.from(module.HEAPU8.slice(p, p + 24));
                    const end = nameBytes.indexOf(0);
                    hash.update(nameBytes.subarray(0, end));
                    hash.update(Buffer.from([0]));
                    const record = view.getUint32(p + 24, true);
                    const length = view.getUint32(p + 28, true);
                    const metadata = Buffer.alloc(6);
                    metadata.writeUInt16LE(view.getUint16(p + 32, true));
                    metadata.writeUInt32LE(view.getUint32(p + 36, true), 2);
                    hash.update(metadata);
                    hash.update(Buffer.from([record && length ? 1 : 0]));
                    if (record && length)
                        hash.update(module.HEAPU8.subarray(record, record + length));
                }
                if (checksum(data) === expected[name][0]) {
                    assert.equal(count, expected[name][1], name);
                    assert.equal(hash.digest('hex'), expected[name][2], name);
                } else assert.ok(count > 0, name);
                compared++;
            } finally {
                if (records) module._worldcol_free_archive(records, count);
                module._free(input);
                module._free(pointers);
            }
        }
        assert.equal(compared, 2);
    } finally {
        await handle.close();
    }
});
