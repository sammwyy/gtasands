import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {existsSync} from 'node:fs';
import {open, readFile} from 'node:fs/promises';
import {join, resolve} from 'node:path';
import test from 'node:test';
import {imgEntries} from '../../web/img_assets.mjs';
import {convertCollision} from '../../web/native_convert.mjs';

const root = resolve(import.meta.dirname, '../..');
const game = join(root, 'game/models');
const wasm = existsSync(join(root, 'web/assets.mjs'));
const checksum = bytes => createHash('sha256').update(bytes).digest('hex');
const golden = {
    'peds.col': ['d6faa88b9227e6f6fe286f69cfd8d7348229bda267b21125b7daea4d3dd23d10', {
        full: [15160, '45d35836c235844f2e377137b71a97cc39afd0c9c3ecbdf8da216e758bc235df'],
        bounds: [11048, '0e06f5ecd4357a4af6d99c4b6e4122ba401d57f99b1fd61709fc204281a7ede1']
    }],
    'vehicles.col': ['4b6fa3aacf7d8be7aabf2e7048b5d7bd9bc89991b13f4f3d7334047a4f5359cf', {
        full: [820, 'a619d4ff10414ecfb56b1d795c7ef2bc03dcc04f965c95ee7f8f27e22915f7f9'],
        bounds: [548, '42d92b8080fe294629aa4f77d78648216ca3bf43f23c78f85465cb451c2d99a8'],
        simple: [820, 'a619d4ff10414ecfb56b1d795c7ef2bc03dcc04f965c95ee7f8f27e22915f7f9']
    }],
    'weapons.col': ['389a5656e0496486e1c546e1e8ffd5399c1563c717f8238dcfdee3b6756903e6', {
        full: [508, 'd2ac228f8d98135db570993fb0e4dbe00b62167286dc94bd99dd35db74179d0b'],
        bounds: [508, 'd2ac228f8d98135db570993fb0e4dbe00b62167286dc94bd99dd35db74179d0b'],
        simple: [508, 'd2ac228f8d98135db570993fb0e4dbe00b62167286dc94bd99dd35db74179d0b']
    }],
    'levelmap_1.col': ['78857e52b819031b249cc7d4b4f232a643d1c4eab887e45170fd98a0187844e5', {
        simple: [65276, 'cf5f3e8c2f1b25dd6d691ce2641f08f0ab0333487b9c37f33b68c2a62ed08cdb']
    }],
    'seabed10.col': ['7e1ebb898b2d1c8a3fcdd2c04d64b7de049cbdce444cacdad39aad9d940d74ae', {
        simple: [3172, '1fb03494c5d70a2700b4b52e9af0b85e328ca27f6ca968a2fcaabf4136c2e7ae']
    }],
    'seabed12.col': ['b023619561f03b537e3aa2023bad75c221fb472c4e31fdce418220c2f1bf60c9', {
        simple: [10672, '2789d8feb2dbf83916222b0c7af104e9882ce9b41f4e61165b6759d403faf344']
    }]
};

test('real COL1-4 conversion modes match Python for loose and archived files', {
    skip: !wasm
}, async t => {
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    let checked = 0;
    const compare = (name, data, modes) => {
        for (const mode of modes) {
            const output = Buffer.from(convertCollision(module, data,
                {mode, face_ratio: 0.5, min_faces: 16}));
            assert.equal(output.subarray(0, 4).toString(), 'DCOL', name);
            if (checksum(data) === golden[name][0])
                assert.deepEqual([output.length, checksum(output)],
                    golden[name][1][mode], `${name} ${mode}`);
            checked++;
        }
    };
    for (const name of ['peds.col', 'vehicles.col', 'weapons.col']) {
        const path = join(game, 'coll', name);
        if (existsSync(path))
            compare(name, await readFile(path), name === 'peds.col' ?
                ['full', 'bounds'] : ['full', 'bounds', 'simple']);
    }
    const path = join(game, 'gta3.img');
    if (existsSync(path)) {
        const handle = await open(path);
        try {
            const read = async (start, length) => {
                const bytes = Buffer.alloc(length);
                let used = 0;
                while (used < length) {
                    const {bytesRead} = await handle.read(bytes,
                        used, length - used, start + used);
                    if (!bytesRead) throw new Error('Truncated IMG archive');
                    used += bytesRead;
                }
                return bytes;
            };
            const file = {
                size: (await handle.stat()).size,
                slice(start, end) {
                    return {async arrayBuffer() {
                        const bytes = await read(start, end - start);
                        return bytes.buffer.slice(bytes.byteOffset,
                            bytes.byteOffset + bytes.byteLength);
                    }};
                }
            };
            for (const {name, offset, size} of await imgEntries(module, file))
                if (name in golden && name !== 'peds.col' &&
                    name !== 'vehicles.col' && name !== 'weapons.col')
                    compare(name, await read(offset, size), ['simple']);
        } finally {
            await handle.close();
        }
    }
    if (!checked) t.skip('local GTA installation unavailable');
});
