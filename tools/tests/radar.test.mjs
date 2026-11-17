import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {existsSync} from 'node:fs';
import {open} from 'node:fs/promises';
import {join, resolve} from 'node:path';
import test from 'node:test';
import {imgEntries} from '../../web/img_assets.mjs';

const root = resolve(import.meta.dirname, '../..');
const wasm = existsSync(join(root, 'web/assets.mjs'));
const checksum = bytes => createHash('sha256').update(bytes).digest('hex');

function chunk(tag, data) {
    const header = Buffer.alloc(12);
    header.writeUInt32LE(tag);
    header.writeUInt32LE(data.length, 4);
    return Buffer.concat([header, data]);
}

function fixture() {
    const rgba = Buffer.alloc(128 * 128 * 4);
    for (let y = 0; y < 128; y++)
        for (let x = 0; x < 128; x++) {
            const p = (y * 128 + x) * 4;
            rgba[p] = (x + y) & 255;
            rgba[p + 1] = x;
            rgba[p + 2] = y;
            rgba[p + 3] = 255;
        }
    const native = Buffer.alloc(92);
    native.writeUInt32LE(9);
    native.write('radar0', 8);
    native.writeUInt32LE(0x500, 72);
    native.writeUInt16LE(128, 80);
    native.writeUInt16LE(128, 82);
    native[84] = 32;
    native[85] = 1;
    native[87] = 1;
    native.writeUInt32LE(rgba.length, 88);
    return chunk(0x16, chunk(0x15,
        chunk(1, Buffer.concat([native, rgba]))));
}

function tile(module, bytes) {
    const input = module._malloc(bytes.length);
    const pointers = module._malloc(8);
    let result = 0;
    try {
        module.HEAPU8.set(bytes, input);
        assert.equal(module._radar_convert(input, bytes.length,
            pointers, pointers + 4), 1);
        const view = new DataView(module.HEAPU8.buffer);
        result = view.getUint32(pointers, true);
        const length = view.getUint32(pointers + 4, true);
        return Buffer.from(module.HEAPU8.slice(result, result + length));
    } finally {
        if (result) module._free(result);
        module._free(input);
        module._free(pointers);
    }
}

test('synthetic and three real radar tiles match Python pixel/palette bytes', {
    skip: !wasm
}, async () => {
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const data = fixture();
    assert.equal(checksum(data),
        '6612bf1a5ad557a2ce97948115427c80fe44fd5505285c9dba866a9b3958c319');
    const converted = tile(module, data);
    assert.equal(converted.length, 10240);
    assert.equal(checksum(converted),
        'b69d2b7a9690cc83959d1d7459ebfb2592e292c9469e886bfdb7b154ff9fc76d');
    const archive = join(root, 'game/models/gta3.img');
    if (!existsSync(archive)) return;
    const file = await open(archive);
    try {
        const read = async (offset, size) => {
            const bytes = Buffer.alloc(size);
            let used = 0;
            while (used < size) {
                const {bytesRead} = await file.read(bytes,
                    used, size - used, offset + used);
                if (!bytesRead) throw new Error('Truncated radar archive.');
                used += bytesRead;
            }
            return bytes;
        };
        const entryFile = {
            size: (await file.stat()).size,
            slice(start, end) {
                return {arrayBuffer: () => read(start, end - start)};
            }
        };
        const entries = (await imgEntries(module, entryFile)).filter(
            entry => /^radar.*\.txd$/i.test(entry.name)).slice(0, 3);
        assert.equal(entries.length, 3);
        const known = [
            ['radar00.txd',
                '66098d9bc591a17507db932481679f72a47204c54cfb13c47b1cb6206222c4ae',
                '59c08451072e341a391a3c76c704d8d7dee0eb7ec061db7805987e670397ec0b'],
            ['radar01.txd',
                '774ac4f509f403934df89844689f4beff0730facb00077fa069310dde092ee3f',
                '13cb4b9bc406dd1fd885bf10fc4258fd5a0d1eea4989a3108df819bb605e4855'],
            ['radar02.txd',
                '979418626bb21524dda5043e5846c2c86f70db5c7f0dd4926831692ebeeff4d6',
                '69928326d6433149a0630509b0938235a459d69f9355a69cb6cdacffd36cc55e']
        ];
        for (const [i, entry] of entries.entries()) {
            const source = await read(entry.offset, entry.size);
            const result = tile(module, source);
            assert.equal(result.length, 10240);
            if (entry.name.toLowerCase() === known[i][0] &&
                checksum(source) === known[i][1])
                assert.equal(checksum(result), known[i][2], entry.name);
        }
    } finally {
        await file.close();
    }
});
