import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {existsSync} from 'node:fs';
import {readFile} from 'node:fs/promises';
import {join, resolve} from 'node:path';
import test from 'node:test';
import {convertModel} from '../../web/native_convert.mjs';

const root = resolve(import.meta.dirname, '../..');
const wasm = existsSync(join(root, 'web/assets.mjs'));
const hash = bytes => createHash('sha256').update(bytes).digest('hex');

function chunk(type, data) {
    const header = Buffer.alloc(12);
    header.writeUInt32LE(type);
    header.writeUInt32LE(data.length, 4);
    header.writeUInt32LE(0x1803ffff, 8);
    return Buffer.concat([header, data]);
}

function model({frames, atomics, sphere, vertices = [], hanim = false}) {
    const identity = [1, 0, 0, 0, 1, 0, 0, 0, 1];
    const records = Buffer.alloc(4 + frames.length * 56);
    records.writeUInt32LE(frames.length);
    for (const [i, frame] of frames.entries()) {
        const start = 4 + i * 56;
        [...identity, ...frame.pos].forEach((value, j) =>
            records.writeFloatLE(value, start + j * 4));
        records.writeInt32LE(frame.parent, start + 48);
    }
    const extensions = frames.map((frame, i) => {
        let data = chunk(0x253f2fe, Buffer.from(`node${i}\0`));
        if (hanim && i) {
            const hierarchy = Buffer.alloc(i === 1 ? 44 : 20);
            hierarchy.writeInt32LE(i - 1, 4);
            if (i === 1) {
                hierarchy.writeInt32LE(2, 8);
                hierarchy.writeUInt32LE(1, 24);
                hierarchy.writeInt32LE(1, 32);
                hierarchy.writeUInt32LE(2, 36);
                hierarchy.writeUInt32LE(1, 40);
            }
            data = Buffer.concat([data, chunk(0x11e, hierarchy)]);
        }
        return chunk(3, data);
    });
    const frameList = chunk(0xe, Buffer.concat([chunk(1, records), ...extensions]));
    const triangles = vertices.length ? 1 : 0;
    const geometry = Buffer.alloc(16 + triangles * 8 + 24 +
        vertices.length * 12);
    geometry.writeUInt32LE(triangles, 4);
    geometry.writeUInt32LE(vertices.length, 8);
    geometry.writeUInt32LE(1, 12);
    if (triangles) {
        for (const [i, index] of [0, 1, 0, 2].entries())
            geometry.writeUInt16LE(index, 16 + i * 2);
    }
    const start = 16 + triangles * 8;
    sphere.forEach((value, i) => geometry.writeFloatLE(value, start + i * 4));
    if (vertices.length) geometry.writeUInt32LE(1, start + 16);
    vertices.flat().forEach((value, i) =>
        geometry.writeFloatLE(value, start + 24 + i * 4));
    const material = chunk(7, chunk(1, Buffer.concat([
        Buffer.from([0, 255, 255, 255, 255]), Buffer.alloc(19)
    ])));
    const materialList = chunk(8, Buffer.concat([
        chunk(1, Buffer.from([1, 0, 0, 0, 255, 255, 255, 255])), material
    ]));
    const geoms = chunk(0x1a, Buffer.concat([
        chunk(1, Buffer.from([1, 0, 0, 0])),
        chunk(0x0f, Buffer.concat([chunk(1, geometry), materialList]))
    ]));
    return chunk(0x10, Buffer.concat([frameList, geoms,
        ...atomics.map(([frame, geom]) => {
            const record = Buffer.alloc(8);
            record.writeUInt32LE(frame);
            record.writeUInt32LE(geom, 4);
            return chunk(0x14, chunk(1, record));
        })]));
}

function radius(module, bytes) {
    const input = module._malloc(bytes.length);
    const output = module._malloc(2);
    try {
        module.HEAPU8.set(bytes, input);
        assert.equal(module._dff_radius(input, bytes.length, output), 1);
        return new DataView(module.HEAPU8.buffer).getUint16(output, true);
    } finally {
        module._free(input);
        module._free(output);
    }
}

test('nested DFF frame transform and reused geometry radii match Python', {
    skip: !wasm
}, async () => {
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const nested = model({
        frames: [
            {parent: -1, pos: [10, 0, 0]},
            {parent: 0, pos: [0, 5, 0]}
        ],
        sphere: [1, 0, 0, 2],
        atomics: [[1, 0]]
    });
    const repeated = model({
        frames: [
            {parent: -1, pos: [0, 0, 0]},
            {parent: -1, pos: [20, 0, 0]}
        ],
        sphere: [0, 0, 0, 2],
        atomics: [[0, 0], [1, 0]]
    });
    assert.equal(radius(module, nested), 15);
    assert.equal(radius(module, repeated), 22);
});

test('rigid cutscene DFF follows HAnim hierarchy and matches Python DMDL', {
    skip: !wasm
}, async () => {
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const bytes = model({
        frames: [
            {parent: -1, pos: [0, 0, 0]},
            {parent: 0, pos: [10, 0, 0]},
            {parent: 1, pos: [2, 0, 0]}
        ],
        sphere: [0, 0, 0, 1],
        atomics: [[2, 0]],
        vertices: [[0, 0, 0], [1, 0, 0], [0, 1, 0]],
        hanim: true
    });
    assert.equal(bytes.length, 634);
    assert.equal(hash(bytes),
        '9c2f6d9e3bc72df18a9d4de4dfd3f92364c6e9140a3751f0809cf11fd6833030');
    const manifest = JSON.parse(await readFile(join(root, 'web/resources.json')));
    const result = Buffer.from(convertModel(module, bytes, {
        ...manifest.defaults.model, ratio: 1, drop_damage: false, rigid_skin: true
    }));
    assert.equal(result.length, 544);
    assert.equal(result.readUInt16LE(6), 1);
    assert.equal(result.readUInt16LE(14), 2);
    assert.equal(hash(result),
        '104fd0ba22e31006a35ec76304fb358987e0a2bd2fb564b48aed30c07f603945');
});
