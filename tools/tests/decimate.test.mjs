import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {existsSync} from 'node:fs';
import {join, resolve} from 'node:path';
import test from 'node:test';

const root = resolve(import.meta.dirname, '../..');
const wasm = existsSync(join(root, 'web/assets.mjs'));
const checksum = bytes => createHash('sha256').update(bytes).digest('hex');

function packFaces(module, pointer, count) {
    const view = new DataView(module.HEAPU8.buffer);
    const bytes = Buffer.alloc(count * 16);
    for (let i = 0; i < count; i++) {
        for (let j = 0; j < 3; j++)
            bytes.writeUInt32LE(view.getUint32(pointer + i * 16 + j * 4, true),
                i * 16 + j * 4);
        bytes.writeUInt16LE(view.getUint16(pointer + i * 16 + 12, true),
            i * 16 + 12);
    }
    return bytes;
}

test('QEM preserves Python face order with and without bone constraints', {
    skip: !wasm
}, async () => {
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const vertices = module._malloc(64 * 24);
    const faces = module._malloc(98 * 16);
    const bones = module._malloc(64);
    const pointers = module._malloc(8);
    let result = 0;
    try {
        module.HEAPU8.fill(0, faces, faces + 98 * 16);
        const view = new DataView(module.HEAPU8.buffer);
        for (let y = 0; y < 8; y++)
            for (let x = 0; x < 8; x++) {
                const p = vertices + (y * 8 + x) * 24;
                view.setFloat64(p, x, true);
                view.setFloat64(p + 8, y, true);
                view.setFloat64(p + 16, ((x * y) % 5) * 0.2, true);
                module.HEAPU8[bones + y * 8 + x] = y % 3;
            }
        for (let y = 0; y < 7; y++)
            for (let x = 0; x < 7; x++) {
                const a = y * 8 + x;
                const p = faces + (y * 7 + x) * 32;
                for (const [i, triangle] of [
                    [a, a + 1, a + 8], [a + 1, a + 9, a + 8]
                ].entries()) {
                    for (const [j, index] of triangle.entries())
                        view.setUint32(p + i * 16 + j * 4, index, true);
                    view.setUint16(p + i * 16 + 12, x % 3, true);
                }
            }
        const golden = [
            [[26, '4731ca425140c7692808957c42210b0a1a45a69675a0c1f28ffa6968ffcee2d2'],
                [30, 'bfd076bb314f461da3f767f50315ffb65daf1221aec88711da44aef87844c787'],
                [60, '7833168ecc0da657e92aab68c2c154ea51f4aae2c4ca51a8557e8d1ba1b3e803'],
                [98, 'b672540d414ef17a368898818488eee92de4184297f3faca506f11096be8942b']],
            [[26, '13609ae092e51d4cd50c5f80b9101eb99119cc0b2777eeefa74dd8149ab572c6'],
                [30, 'a060eec993ee02bfb2c552f4e5e4e2d8dc7a9d30ab1386304f6e55aca46846ac'],
                [60, 'ea258b5ffdc78c8573fb1f43ec315ddacd8baf952abab8d119ea90fc98a891e6'],
                [98, 'b672540d414ef17a368898818488eee92de4184297f3faca506f11096be8942b']]
        ];
        for (const [boneIndex, constraints] of golden.entries())
            for (const [i, target] of [10, 30, 60, 98].entries()) {
                assert.equal(module._qem_decimate(vertices, 64, faces, 98,
                    target, boneIndex ? bones : 0, pointers, pointers + 4), 1);
                const memory = new DataView(module.HEAPU8.buffer);
                result = memory.getUint32(pointers, true);
                const count = memory.getUint32(pointers + 4, true);
                assert.equal(count, constraints[i][0]);
                assert.equal(checksum(packFaces(module, result, count)),
                    constraints[i][1], `${boneIndex}/${target}`);
                module._free(result);
                result = 0;
            }
    } finally {
        if (result) module._free(result);
        for (const pointer of [vertices, faces, bones, pointers])
            module._free(pointer);
    }
});

test('vertex clustering matches Python representatives and kept faces at five targets', {
    skip: !wasm
}, async () => {
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const vertices = module._malloc(160 * 24);
    const faces = module._malloc(500 * 16);
    const result = module._malloc(12);
    let seed = 47;
    const random = () => {
        seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
        return seed / 4294967296;
    };
    try {
        module.HEAPU8.fill(0, faces, faces + 500 * 16);
        const view = new DataView(module.HEAPU8.buffer);
        for (let i = 0; i < 160; i++) {
            const values = [-30 + 60 * random(), -40 + 80 * random(),
                -10 + 20 * random()];
            values.forEach((value, j) =>
                view.setFloat64(vertices + i * 24 + j * 8, value, true));
        }
        for (let i = 0; i < 500; i++) {
            for (let j = 0; j < 3; j++)
                view.setUint32(faces + i * 16 + j * 4,
                    Math.trunc(random() * 160), true);
            view.setUint16(faces + i * 16 + 12,
                Math.trunc(random() * 4), true);
        }
        const golden = [
            [0, 0, 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855',
                'd3923a68d39e1daeae6d0cd606f5d906a37a1b1c38eaf6508dbb700492a6387f'],
            [10, 0, 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855',
                'd3923a68d39e1daeae6d0cd606f5d906a37a1b1c38eaf6508dbb700492a6387f'],
            [60, 0, 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855',
                'd3923a68d39e1daeae6d0cd606f5d906a37a1b1c38eaf6508dbb700492a6387f'],
            [250, 77, '156093f8b82e457779f186bf2f8c9f72f07c1ca82c7bd1c856dcc1ee44672dd8',
                '878da44ab0b39fdfbd9606af654d8ae4d56e7483dd1f4c32deab165c5e4b8731'],
            [500, 500, '743b810637a08523152970686e7b58ddc96634e19c5230e8e94c1685fcb1e36e',
                null]
        ];
        for (const [target, count, facesHash, representativesHash] of golden) {
            assert.equal(module._cluster_decimate(
                vertices, 160, faces, 500, target, result), 1);
            const memory = new DataView(module.HEAPU8.buffer);
            const representatives = memory.getUint32(result, true);
            const kept = memory.getUint32(result + 4, true);
            const size = memory.getUint32(result + 8, true);
            try {
                assert.equal(size, count, `target ${target}`);
                const output = Buffer.alloc(count * 16);
                for (let i = 0; i < size; i++) {
                    const index = memory.getUint32(kept + i * 4, true);
                    const source = packFaces(module, faces + index * 16, 1);
                    source.copy(output, i * 16);
                }
                assert.equal(checksum(output), facesHash, `faces at ${target}`);
                assert.equal(representatives ?
                    checksum(module.HEAPU8.slice(representatives,
                        representatives + 160 * 24)) : null,
                representativesHash, `representatives at ${target}`);
            } finally {
                module._cluster_free(result);
            }
        }
    } finally {
        module._free(vertices);
        module._free(faces);
        module._free(result);
    }
});

test('maximum-shift limiter preserves Python corner displacement and cap', {
    skip: !wasm
}, async () => {
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const vertices = module._malloc(9 * 24);
    const faces = module._malloc(8 * 16);
    const result = module._malloc(12);
    try {
        module.HEAPU8.fill(0, faces, faces + 8 * 16);
        const view = new DataView(module.HEAPU8.buffer);
        for (let y = 0; y < 3; y++)
            for (let x = 0; x < 3; x++) {
                const p = vertices + (y * 3 + x) * 24;
                view.setFloat64(p, x, true);
                view.setFloat64(p + 8, y, true);
                view.setFloat64(p + 16, 0, true);
            }
        for (let y = 0; y < 2; y++)
            for (let x = 0; x < 2; x++) {
                const a = y * 3 + x;
                const p = faces + (y * 2 + x) * 32;
                for (const [i, corners] of [
                    [a, a + 1, a + 3], [a + 1, a + 4, a + 3]
                ].entries())
                    corners.forEach((index, j) =>
                        view.setUint32(p + i * 16 + j * 4, index, true));
            }
        for (const [maximum, count, facesHash, repHash] of [
            [0, 8, '164db1934ed9175a9897e1388612606e7a8c2fd5420d59eca88da6bea5211937', null],
            [3, 2, '905be9e54a911467e91327077928dc9236400c12e6cdcaa7222a14496aa270c7',
                '5cf565b83a94fc8f4802c28d5a2994bf8fc61f11f78554f242e1eb5a25ddaec9']
        ]) {
            assert.equal(module._cluster_decimate_limited(vertices, 9,
                faces, 8, 2, 0, 0.25, maximum, result), 1);
            const data = new DataView(module.HEAPU8.buffer);
            const representative = data.getUint32(result, true);
            const kept = data.getUint32(result + 4, true);
            const size = data.getUint32(result + 8, true);
            try {
                assert.equal(size, count);
                assert.ok(maximum || size > 2);
                assert.ok(!maximum || size <= maximum);
                const packed = Buffer.alloc(size * 16);
                for (let i = 0; i < size; i++) {
                    const index = data.getUint32(kept + i * 4, true);
                    packFaces(module, faces + index * 16, 1).copy(packed, i * 16);
                    if (representative && !maximum)
                        for (let j = 0; j < 3; j++) {
                            const point = data.getUint32(faces + index * 16 + j * 4, true);
                            const x = data.getFloat64(representative + point * 24, true);
                            const y = data.getFloat64(representative + point * 24 + 8, true);
                            const oldX = data.getFloat64(vertices + point * 24, true);
                            const oldY = data.getFloat64(vertices + point * 24 + 8, true);
                            assert.ok(Math.hypot(x - oldX, y - oldY) <= 0.25);
                        }
                }
                assert.equal(checksum(packed), facesHash);
                assert.equal(representative ?
                    checksum(module.HEAPU8.slice(representative,
                        representative + 9 * 24)) : null, repHash);
            } finally {
                module._cluster_free(result);
            }
        }
    } finally {
        module._free(vertices);
        module._free(faces);
        module._free(result);
    }
});
