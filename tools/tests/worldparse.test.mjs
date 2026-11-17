import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {existsSync} from 'node:fs';
import {open, readFile, readdir} from 'node:fs/promises';
import {join, relative, resolve, sep} from 'node:path';
import test from 'node:test';
import {imgEntries} from '../../web/img_assets.mjs';

const root = resolve(import.meta.dirname, '../..');
const game = join(root, 'game');
const wasm = existsSync(join(root, 'web/assets.mjs'));
const checksum = bytes => createHash('sha256').update(bytes).digest('hex');
const zero = Buffer.from([0]);

function sourceIndex(paths) {
    return new Map(paths.map(path =>
        [relative(game, path).split(sep).join('/').toLowerCase(), path]));
}

async function dataFiles() {
    return sourceIndex((await readdir(join(game, 'data'), {
        recursive: true, withFileTypes: true
    })).filter(item => item.isFile()).map(item =>
        join(item.parentPath, item.name)));
}

function named(bytes) {
    return Buffer.from(bytes).toString('latin1').split('\0')[0];
}

function parseIDE(module, bytes) {
    const input = module._malloc(bytes.length);
    const pointers = module._malloc(16);
    let objects = 0, parents = 0;
    try {
        module.HEAPU8.set(bytes, input);
        assert.equal(module._ide_parse(input, bytes.length, pointers,
            pointers + 4, pointers + 8, pointers + 12), 1);
        const view = new DataView(module.HEAPU8.buffer);
        objects = view.getUint32(pointers, true);
        parents = view.getUint32(pointers + 8, true);
        const models = new Map(), links = new Map();
        for (let i = 0; i < view.getUint32(pointers + 4, true); i++) {
            const address = objects + i * 288;
            models.set(view.getInt32(address, true),
                [named(module.HEAPU8.subarray(address + 4, address + 132)),
                    named(module.HEAPU8.subarray(address + 132, address + 260)),
                    view.getFloat64(address + 264, true),
                    Number(view.getBigInt64(address + 272, true)),
                    view.getInt32(address + 280, true),
                    view.getInt32(address + 284, true)]);
        }
        for (let i = 0; i < view.getUint32(pointers + 12, true); i++) {
            const address = parents + i * 256;
            links.set(named(module.HEAPU8.subarray(address, address + 128)),
                named(module.HEAPU8.subarray(address + 128, address + 256)));
        }
        const output = Buffer.alloc(models.size * 288 + links.size * 256);
        let offset = 0;
        for (const [id, [name, txd, distance, flags, on, off]] of
            [...models].sort(([a], [b]) => a - b)) {
            output.writeInt32LE(id, offset);
            output.write(name, offset + 8, 'latin1');
            output.write(txd, offset + 136, 'latin1');
            output.writeDoubleLE(distance, offset + 264);
            output.writeBigInt64LE(BigInt(flags), offset + 272);
            output.writeInt32LE(on, offset + 280);
            output.writeInt32LE(off, offset + 284);
            offset += 288;
        }
        for (const [name, parent] of
            [...links].sort(([a], [b]) => a.localeCompare(b))) {
            output.write(name, offset, 'latin1');
            output.write(parent, offset + 128, 'latin1');
            offset += 256;
        }
        return {models, links, output};
    } finally {
        if (objects) module._free(objects);
        if (parents) module._free(parents);
        module._free(input);
        module._free(pointers);
    }
}

function parseIPL(module, bytes, binary) {
    const input = module._malloc(bytes.length);
    const pointers = module._malloc(8);
    let entries = 0;
    try {
        module.HEAPU8.set(bytes, input);
        assert.equal((binary ? module._ipl_parse_binary : module._ipl_parse_text)(
            input, bytes.length, pointers, pointers + 4), 1);
        const view = new DataView(module.HEAPU8.buffer);
        entries = view.getUint32(pointers, true);
        const count = view.getUint32(pointers + 4, true);
        return {count, packed: Buffer.from(module.HEAPU8.slice(
            entries, entries + count * 72))};
    } finally {
        if (entries) module._free(entries);
        module._free(input);
        module._free(pointers);
    }
}

test('IDE parser matches Python on synthetic objects, timed entries and real files', {
    skip: !wasm
}, async () => {
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const fixture = Buffer.from('objs\n1,thing,shared,1,150,2\nend\n' +
        'tobj\n2,clock,shared,200,1,8,16\nend\n' +
        'anim\n3,move,shared,walk,99,4\nend\n' +
        'txdp\nshared,base\nend\n');
    const synthetic = parseIDE(module, fixture);
    assert.deepEqual([...synthetic.models], [
        [1, ['thing', 'shared', 150, 2, 24, 24]],
        [2, ['clock', 'shared', 200, 1, 8, 16]],
        [3, ['move', 'shared', 99, 4, 24, 24]]
    ]);
    assert.deepEqual([...synthetic.links], [['shared', 'base']]);
    assert.equal(checksum(synthetic.output),
        '32e15060ffa25a2ff33269136d27c950280d458f1e0202a75aeb9d34bc09ed2d');
    if (!existsSync(join(game, 'data/gta.dat'))) return;
    const index = await dataFiles();
    const dat = await readFile(join(game, 'data/gta.dat'), 'utf8');
    const names = ['data/maps/txd.ide', ...dat.split(/\r?\n/)
        .filter(line => /^IDE /i.test(line))
        .map(line => line.slice(4).trim().replaceAll('\\', '/').toLowerCase())];
    const sources = createHash('sha256'), outputs = createHash('sha256');
    let count = 0, models = 0, parents = 0;
    for (const name of names) {
        const path = index.get(name);
        if (!path) continue;
        const data = await readFile(path);
        const key = Buffer.from(relative(game, path)
            .split(sep).join('/').toLowerCase(), 'latin1');
        const parsed = parseIDE(module, data);
        sources.update(key).update(zero)
            .update(createHash('sha256').update(data).digest());
        outputs.update(key).update(zero).update(parsed.output);
        count++;
        models += parsed.models.size;
        parents += parsed.links.size;
    }
    assert.deepEqual([count, models, parents], [55, 14258, 37]);
    if (sources.digest('hex') ===
        'a8f0db6869d9270ca37300faef6947853249df4f5095022a14c62423e48e017c')
        assert.equal(outputs.digest('hex'),
            'ec72514a5452a4e6338234982c105da6f3de0ae95446a78420af1683176142ac');
});

test('IPL parser and quaternion rotations agree with Python for synthetic and real data', {
    skip: !wasm
}, async () => {
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const fixture = Buffer.alloc(112);
    fixture.write('bnry');
    fixture.writeUInt32LE(2, 4);
    fixture.writeUInt32LE(32, 28);
    for (const [i, row] of [
        [1, -2, 3, 0, 0, 0, 1, 42, 0, -1],
        [8, 9, 10, 0.5, -0.5, 0, 0, 77, 4, 2]
    ].entries()) {
        for (let j = 0; j < 7; j++)
            fixture.writeFloatLE(row[j], 32 + i * 40 + j * 4);
        for (let j = 0; j < 3; j++)
            fixture.writeInt32LE(row[7 + j], 32 + i * 40 + 28 + j * 4);
    }
    const synthetic = parseIPL(module, fixture, true);
    assert.equal(synthetic.count, 2);
    assert.deepEqual(parseIPL(module, Buffer.from('not bnry'), true),
        {count: 0, packed: Buffer.alloc(0)});
    const text = Buffer.from('inst\n42, model, 0, 1, -2, 3, 0, 0, 0, 1, -1\nend\n');
    assert.equal(parseIPL(module, text, false).count, 1);
    const expected = [
        [1, 0, 0, 0, 1, 0, 0, 0, 1],
        [0.19047619047619035, -0.9047619047619049, -0.38095238095238104,
            0.7619047619047621, 0.3809523809523808, -0.523809523809524,
            0.6190476190476192, -0.1904761904761905, 0.7619047619047619],
        [1, 0, 0, 0, 1, 0, 0, 0, 1]
    ];
    const output = module._malloc(9 * 8);
    try {
        for (const [index, quaternion] of [
            [0, 0, 0, 1], [0.1, -0.3, 0.5, 0.7], [0, 0, 0, 0]
        ].entries()) {
            module._ipl_rotation(...quaternion, output);
            const view = new DataView(module.HEAPU8.buffer);
            for (let i = 0; i < 9; i++)
                assert.ok(Math.abs(view.getFloat64(output + i * 8, true) -
                    expected[index][i]) < 1e-15);
        }
    } finally {
        module._free(output);
    }
    if (!existsSync(join(game, 'data/gta.dat'))) return;
    const index = await dataFiles();
    const source = createHash('sha256'), result = createHash('sha256');
    let count = 0, records = 0;
    for (const line of (await readFile(join(game, 'data/gta.dat'), 'utf8'))
        .split(/\r?\n/)) {
        if (!/^IPL /i.test(line)) continue;
        const path = index.get(line.slice(4).trim()
            .replaceAll('\\', '/').toLowerCase());
        if (!path || !/\.ipl$/i.test(path)) continue;
        const data = await readFile(path);
        const key = Buffer.from(relative(game, path)
            .split(sep).join('/').toLowerCase(), 'latin1');
        const parsed = parseIPL(module, data, false);
        source.update(key).update(zero)
            .update(createHash('sha256').update(data).digest());
        result.update(key).update(zero).update(parsed.packed);
        count++;
        records += parsed.count;
    }
    assert.deepEqual([count, records], [50, 9268]);
    if (source.digest('hex') ===
        'dfbcf1f864107cab271e9eb88ef9f66f2e25040b4646d5966550704864e89983')
        assert.equal(result.digest('hex'),
            '02941680ddf6f2ce4cb5f930db20c64357afca0bc131124aca1ef28202d83bda');
    const handle = await open(join(game, 'models/gta3.img'));
    try {
        const read = async (offset, length) => {
            const bytes = Buffer.alloc(length);
            const {bytesRead} = await handle.read(bytes, 0, length, offset);
            assert.equal(bytesRead, length);
            return bytes;
        };
        const file = {size: (await handle.stat()).size,
            slice(start, end) {return {arrayBuffer: () => read(start, end - start)};}};
        const entry = (await imgEntries(module, file)).find(item =>
            item.name.includes('_stream') && item.name.endsWith('.ipl'));
        assert.ok(entry);
        const data = await read(entry.offset, entry.size);
        const parsed = parseIPL(module, data, true);
        if (checksum(data) ===
            'c8cc39fba87054f1b7b2041028fdf58e34a097506125d2a108987166cc8f7aa5') {
            assert.equal(parsed.count, 183);
            assert.equal(checksum(parsed.packed),
                '9ac033f161ea840b7d08781b6597346f7615af603d42bd8e875f86992f5c9f26');
        }
    } finally {
        await handle.close();
    }
});
