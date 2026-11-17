import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import {closeSync, existsSync, openSync, readSync, statSync} from 'node:fs';
import {mkdir, mkdtemp, readFile, readdir, writeFile} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join, relative, resolve, sep} from 'node:path';
import test from 'node:test';
import {build} from '../assets.mjs';
import {convertWorld} from '../../web/world.mjs';

const root = resolve(import.meta.dirname, '../..');
const wasm = existsSync(join(root, 'web/assets.mjs'));
const checksum = bytes => createHash('sha256').update(bytes).digest('hex');

async function syntheticWorld(source, objects, instances) {
    await mkdir(join(source, 'data/maps'), {recursive: true});
    await mkdir(join(source, 'models'), {recursive: true});
    await writeFile(join(source, 'data/gta.dat'),
        'IDE data/maps/objects.ide\nIPL data/maps/out.ipl\n');
    await writeFile(join(source, 'data/maps/objects.ide'),
        `objs\n${objects.map(([id, name]) =>
            `${id}, ${name}, tex, 100, 0`).join('\n')}\nend\n`);
    await writeFile(join(source, 'data/maps/out.ipl'),
        `inst\n${instances.map(([id, name, x, y, z]) =>
            `${id}, ${name}, 0, ${x}, ${y}, ${z}, 0, 0, 0, 1, -1`)
            .join('\n')}\nend\n`);
    const names = [...objects.map(([, name]) => `${name}.dff`), 'tex.txd'];
    const archive = Buffer.alloc(2048 * (1 + names.length));
    archive.write('VER2');
    archive.writeUInt32LE(names.length, 4);
    for (const [i, name] of names.entries()) {
        const p = 8 + i * 32;
        archive.writeUInt32LE(i + 1, p);
        archive.writeUInt16LE(1, p + 4);
        archive.writeUInt16LE(1, p + 6);
        archive.write(name, p + 8);
    }
    await writeFile(join(source, 'models/gta3.img'), archive);
    await writeFile(join(source, 'models/gta_int.img'),
        Buffer.from([86, 69, 82, 50, 0, 0, 0, 0]));
}

test('native and browser world builders match Python on original-format files', {
    skip: !wasm
}, async t => {
    const built = spawnSync('make', ['-f', 'tools/portable/Makefile',
        'build/worldbuild_cli'], {cwd: root, encoding: 'utf8'});
    if (built.status !== 0) return t.skip('C compiler unavailable');
    const work = await mkdtemp(join(tmpdir(), 'gtasads-worldbuild-test-'));
    const source = join(work, 'game');
    await syntheticWorld(source, [[1, 'outside']],
        [[1, 'outside', 10, -20, 12]]);
    const native = join(work, 'native.dwld');
    const run = spawnSync(join(root, 'build/worldbuild_cli'),
        [source, native], {encoding: 'utf8'});
    assert.equal(run.status, 0, run.stderr);
    const result = await readFile(native);
    assert.equal(result.length, 164);
    assert.equal(checksum(result),
        '897be0bc5afcc540dde80d14079f35aac646dd685241efc2b331d318c5f0a17f');
    const output = join(work, 'assets');
    await build({source, output, sections: ['world']});
    assert.deepEqual(await readFile(join(output, 'data/world.dwld')), result);
});

test('two-model world output retains sector and instance ordering from Python', {
    skip: !wasm
}, async () => {
    const work = await mkdtemp(join(tmpdir(), 'gtasads-two-model-world-test-'));
    const source = join(work, 'game'), output = join(work, 'assets');
    await syntheticWorld(source, [[1, 'outside'], [2, 'thing']], [
        [1, 'outside', 10.5, -20.25, 12],
        [2, 'thing', 101.5, 75, 9]
    ]);
    await build({source, output, sections: ['world']});
    const blob = await readFile(join(output, 'data/world.dwld'));
    assert.equal(blob.length, 316);
    assert.equal(checksum(blob),
        'a855a18b16c76306a51d5df7fa1559e575afd16bf88a6526395f0f0ce7757010');
    assert.deepEqual([blob.subarray(0, 4).toString(),
        blob.readUInt16LE(4), blob.readUInt16LE(16)],
    ['DWLD', 6, 2]);
});

test('interior IPL instances retain their area and interior IMG model/TXD pack', {
    skip: !wasm
}, async () => {
    const work = await mkdtemp(join(tmpdir(), 'gtasads-interior-world-test-'));
    const source = join(work, 'game'), output = join(work, 'assets');
    await mkdir(join(source, 'data/maps/interior'), {recursive: true});
    await mkdir(join(source, 'models'), {recursive: true});
    await writeFile(join(source, 'data/gta.dat'),
        'IDE data/maps/objects.ide\nIPL data/maps/ex.ipl\n' +
        'IPL data/maps/interior/room.ipl\n');
    await writeFile(join(source, 'data/maps/objects.ide'),
        'objs\n1, outside, outtex, 100, 0\n' +
        '2, room, roomtex, 100, 0\nend\n');
    await writeFile(join(source, 'data/maps/ex.ipl'),
        'inst\n1, outside, 0, 10, 10, 10, 0, 0, 0, 1, -1\nend\n');
    await writeFile(join(source, 'data/maps/interior/room.ipl'),
        'inst\n2, room, 3, 20, 20, 1000, 0, 0, 0, 1, -1\nend\n');
    for (const [archive, names] of [
        ['gta3.img', ['outside.dff', 'outtex.txd']],
        ['gta_int.img', ['room.dff', 'roomtex.txd']]
    ]) {
        const bytes = Buffer.alloc(6144);
        bytes.write('VER2');
        bytes.writeUInt32LE(names.length, 4);
        names.forEach((name, i) => {
            bytes.writeUInt32LE(i + 1, 8 + i * 32);
            bytes.writeUInt16LE(1, 12 + i * 32);
            bytes.writeUInt16LE(1, 14 + i * 32);
            bytes.write(name, 16 + i * 32);
        });
        await writeFile(join(source, 'models', archive), bytes);
    }
    await build({source, output, sections: ['world']});
    const blob = await readFile(join(output, 'data/world.dwld'));
    assert.equal(blob.subarray(0, 4).toString(), 'DWLD');
    assert.equal(blob.readUInt16LE(4), 6);
    assert.equal(blob.readUInt32LE(20), 2);
    const modelOffset = blob.readUInt32LE(24);
    assert.deepEqual(Array.from({length: 2}, (_, i) =>
        blob.readUInt32LE(modelOffset + i * 36 + 32) >>> 31), [0, 1]);
    assert.deepEqual(Array.from({length: 2}, (_, i) =>
        blob.readUInt16LE(modelOffset + 72 + i * 28 + 26)), [0, 1]);
    const instances = blob.readUInt32LE(32);
    assert.deepEqual(new Set(Array.from({length: 2}, (_, i) =>
        blob.readUInt16LE(instances + i * 40 + 36))), new Set([0, 3]));
});

test('world sectors preserve Python order for empty, overlapping and edge cells', {
    skip: !wasm
}, async () => {
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    for (const points of [[], [[-100, 20], [-50, 20],
        [-100, 20], [49.99, -50]]]) {
        const records = module._malloc(Math.max(points.length * 112, 1));
        const pointers = module._malloc(16);
        let counts = 0, order = 0;
        try {
            module.HEAPU8.fill(0, records, records + points.length * 112);
            const view = new DataView(module.HEAPU8.buffer);
            for (const [i, [x, y]] of points.entries()) {
                view.setFloat64(records + i * 112 + 8, x, true);
                view.setFloat64(records + i * 112 + 16, y, true);
            }
            module.HEAPU8.fill(0, pointers, pointers + 16);
            assert.equal(module._worldplan_sectors(records, points.length, 50,
                pointers, pointers + 2, pointers + 4, pointers + 6,
                pointers + 8, pointers + 12), 1);
            const output = new DataView(module.HEAPU8.buffer);
            const dimension = Array.from({length: 4}, (_, i) =>
                i < 2 ? output.getInt16(pointers + i * 2, true) :
                    output.getUint16(pointers + i * 2, true));
            assert.deepEqual(dimension, points.length ?
                [-100, -50, 3, 2] : [0, 0, 1, 1]);
            counts = output.getUint32(pointers + 8, true);
            order = output.getUint32(pointers + 12, true);
            assert.deepEqual(Array.from({length: dimension[2] * dimension[3]},
                (_, i) => output.getUint16(counts + i * 2, true)),
            points.length ? [0, 0, 1, 2, 1, 0] : [0]);
            assert.deepEqual(Array.from({length: points.length}, (_, i) =>
                output.getUint32(order + i * 4, true)),
            points.length ? [3, 0, 2, 1] : []);
        } finally {
            if (counts) module._free(counts);
            if (order) module._free(order);
            module._free(records);
            module._free(pointers);
        }
    }
});

test('world asset source resolves main IMG before interior IMG', {
    skip: !wasm
}, async () => {
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const lists = [
        [['main.dff', 8192, 2048], ['shared.dff', 2048, 4096]],
        [['interior.dff', 4096, 6144], ['shared.dff', 12288, 2048]]
    ];
    const pointers = lists.map(list => module._malloc(list.length * 48));
    const found = module._malloc(52);
    try {
        const memory = new DataView(module.HEAPU8.buffer);
        for (const [pack, list] of lists.entries())
            for (const [i, [name, offset, size]] of list.entries()) {
                const address = pointers[pack] + i * 48;
                module.HEAPU8.fill(0, address, address + 48);
                module.HEAPU8.set(Buffer.from(name), address);
                memory.setBigUint64(address + 32, BigInt(offset), true);
                memory.setUint32(address + 40, size, true);
            }
        for (const [name, expected] of [
            ['shared.dff', [2048, 4096, 0]],
            ['main.dff', [8192, 2048, 0]],
            ['interior.dff', [4096, 6144, 1]],
            ['missing.dff', null]
        ]) {
            const text = Buffer.from(name + '\0');
            const address = module._malloc(text.length);
            try {
                module.HEAPU8.set(text, address);
                const ok = module._worldplan_resolve(address, pointers[0], 2,
                    pointers[1], 2, found, found + 48);
                assert.equal(ok, expected ? 1 : 0, name);
                if (expected) {
                    const view = new DataView(module.HEAPU8.buffer);
                    assert.deepEqual([Number(view.getBigUint64(found + 32, true)),
                        view.getUint32(found + 40, true),
                        view.getInt32(found + 48, true)], expected);
                }
            } finally {
                module._free(address);
            }
        }
    } finally {
        for (const address of pointers) module._free(address);
        module._free(found);
    }
});

test('world data-list parser agrees with Python IDE and IPL selection', {
    skip: !wasm
}, async () => {
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const cases = [Buffer.from(
        '# ignored\nIDE DATA\\MAPS\\city.IDE\nIPL DATA\\MAPS\\city.IPL\n' +
        'IPL data/maps/city.dat\n  ide data/maps/roads.ide # comment\n')];
    if (existsSync(join(root, 'game/data/gta.dat')))
        cases.push(await readFile(join(root, 'game/data/gta.dat')));
    for (const bytes of cases) {
        const input = module._malloc(bytes.length);
        const pointers = module._malloc(8);
        let entries = 0;
        try {
            module.HEAPU8.set(bytes, input);
            assert.equal(module._worldplan_parse_dat(
                input, bytes.length, pointers, pointers + 4), 1);
            const view = new DataView(module.HEAPU8.buffer);
            entries = view.getUint32(pointers, true);
            const count = view.getUint32(pointers + 4, true);
            const actual = [[], []];
            for (let i = 0; i < count; i++) {
                const p = entries + i * 257;
                const path = Buffer.from(module.HEAPU8.subarray(p, p + 256))
                    .toString('latin1').split('\0')[0];
                actual[module.HEAPU8[p + 256]].push(path);
            }
            const expected = [[], []];
            for (const raw of bytes.toString('utf8').split(/\r?\n/)) {
                const line = raw.split('#', 1)[0].trim();
                if (!line) continue;
                const gap = line.indexOf(' ');
                if (gap < 0) continue;
                const kind = line.slice(0, gap).toUpperCase();
                const path = line.slice(gap + 1).trim();
                if (kind === 'IDE') expected[0].push(path);
                else if (kind === 'IPL' && /\.ipl$/i.test(path))
                    expected[1].push(path);
            }
            assert.deepEqual(actual, expected);
        } finally {
            if (entries) module._free(entries);
            module._free(input);
            module._free(pointers);
        }
    }
});

test('world instance planner preserves Python LOD, bounds and missing-model rules', {
    skip: !wasm
}, async () => {
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const text = [
        [100, 0, 0, 0, 0, 2],
        [101, 1, 2, 3, -1, -1],
        [102, 10, 10, 0, 256, -1]
    ];
    const stream = [[103, 100, 100, 0, 257, 2],
        [404, 0, 0, 0, 0, -1]];
    const models = [['main', 1], ['lodghost', 0],
        ['lodmain', 1], ['streamed', 1]];
    const sources = [text, stream].map(rows => {
        const address = module._malloc(rows.length * 72);
        module.HEAPU8.fill(0, address, address + rows.length * 72);
        const view = new DataView(module.HEAPU8.buffer);
        for (const [i, [id, x, y, z, area, lod]] of rows.entries()) {
            const p = address + i * 72;
            view.setInt32(p, id, true);
            for (const [j, value] of [x, y, z, 0, 0, 0, 1].entries())
                view.setFloat64(p + 8 + j * 8, value, true);
            view.setInt32(p + 64, area, true);
            view.setInt32(p + 68, lod, true);
        }
        return address;
    });
    const records = module._malloc(4 * 136);
    const bounds = module._malloc(32);
    const pointers = module._malloc(20);
    let result = 0;
    try {
        module.HEAPU8.fill(0, records, records + 4 * 136);
        for (const [i, [name, available]] of models.entries()) {
            const p = records + i * 136;
            module.HEAPU32[p >>> 2] = 100 + i;
            module.HEAPU8.set(Buffer.from(name), p + 4);
            module.HEAPU8[p + 132] = available;
        }
        const memory = new DataView(module.HEAPU8.buffer);
        for (const [i, value] of [-10, -10, 20, 20].entries())
            memory.setFloat64(bounds + i * 8, value, true);
        for (const bounded of [false, true])
            for (const keep of [false, true]) {
                module.HEAPU8.fill(0, pointers, pointers + 20);
                assert.equal(module._worldplan_instances(sources[0], text.length,
                    sources[1], stream.length, records, models.length,
                    bounded ? bounds : 0, keep ? 1 : 0,
                    pointers, pointers + 4, pointers + 8), 1);
                const view = new DataView(module.HEAPU8.buffer);
                result = view.getUint32(pointers, true);
                const count = view.getUint32(pointers + 4, true);
                const expected = keep ? bounded ? [100, 102] :
                    [100, 102, 103] : bounded ? [100] : [100, 103];
                assert.equal(count, expected.length);
                assert.deepEqual(Array.from({length: count}, (_, i) => {
                    const p = result + i * 112;
                    return [view.getInt32(p, true),
                        view.getInt32(p + 4, true),
                        view.getFloat64(p + 8, true),
                        view.getUint8(p + 104), view.getUint8(p + 105)];
                }), expected.map(id => id === 100 ?
                    [100, 102, 0, 0, 0] : id === 102 ?
                    [102, 103, 10, 1, 0] : [103, 102, 100, 0, 1]));
                assert.deepEqual([0, 1, 2].map(i =>
                    view.getUint32(pointers + 8 + i * 4, true)),
                [2, keep ? 0 : 1, bounded ? 1 : 0]);
                module._free(result);
                result = 0;
            }
    } finally {
        if (result) module._free(result);
        for (const address of sources) module._free(address);
        module._free(records);
        module._free(bounds);
        module._free(pointers);
    }
});

test('world model and texture tables preserve sources, flags and parent links', {
    skip: !wasm
}, async () => {
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const ids = module._malloc(12);
    const objects = module._malloc(3 * 288);
    const parents = module._malloc(2 * 256);
    const main = module._malloc(3 * 48);
    const interior = module._malloc(4 * 48);
    const pointers = module._malloc(12);
    let records = 0, textures = 0;
    try {
        for (const [address, size] of [[ids, 12], [objects, 3 * 288],
            [parents, 2 * 256], [main, 3 * 48], [interior, 4 * 48],
            [pointers, 12]])
            module.HEAPU8.fill(0, address, address + size);
        const view = new DataView(module.HEAPU8.buffer);
        for (const [i, [name, txd, distance, flags, on, off]] of [
            ['one', 'a', 100, 0, 24, 24],
            ['two', 'b', 200, 3, 23, 5],
            ['three', 'missing', 50, 7, 24, 24]
        ].entries()) {
            const address = objects + i * 288;
            view.setInt32(ids + i * 4, i + 1, true);
            view.setInt32(address, i + 1, true);
            module.HEAPU8.set(Buffer.from(name), address + 4);
            module.HEAPU8.set(Buffer.from(txd), address + 132);
            view.setFloat64(address + 264, distance, true);
            view.setBigInt64(address + 272, BigInt(flags), true);
            view.setUint8(address + 280, on);
            view.setUint8(address + 281, off);
        }
        for (const [i, [name, parent]] of [['a', 'b'], ['b', 'a']].entries()) {
            module.HEAPU8.set(Buffer.from(name), parents + i * 256);
            module.HEAPU8.set(Buffer.from(parent), parents + i * 256 + 128);
        }
        for (const [pointer, list] of [[main, [
            ['a.txd', 8192, 2048], ['b.txd', 10240, 4096],
            ['one.dff', 2048, 4096]
        ]], [interior, [
            ['b.txd', 24576, 2048], ['one.dff', 16384, 2048],
            ['three.dff', 22528, 2048], ['two.dff', 20480, 2048]
        ]]]) {
            for (const [i, [name, offset, size]] of list.entries()) {
                const address = pointer + i * 48;
                module.HEAPU8.set(Buffer.from(name), address);
                view.setBigUint64(address + 32, BigInt(offset), true);
                view.setUint32(address + 40, size, true);
            }
        }
        assert.equal(module._worldplan_tables(ids, 3, objects, 3,
            parents, 2, main, 3, interior, 4,
            pointers, pointers + 4, pointers + 8), 1);
        const memory = new DataView(module.HEAPU8.buffer);
        records = memory.getUint32(pointers, true);
        textures = memory.getUint32(pointers + 4, true);
        assert.equal(memory.getUint32(pointers + 8, true), 2);
        const readName = (address, width) => Buffer.from(module.HEAPU8
            .subarray(address, address + width)).toString('latin1')
            .split('\0')[0];
        assert.deepEqual(Array.from({length: 3}, (_, i) => {
            const p = records + i * 168;
            return [readName(p, 128), memory.getUint16(p + 156, true),
                memory.getFloat64(p + 128, true),
                Number(memory.getBigInt64(p + 136, true)),
                memory.getUint8(p + 160), memory.getUint8(p + 161),
                Number(memory.getBigUint64(p + 144, true)),
                memory.getUint32(p + 152, true),
                memory.getUint16(p + 158, true)];
        }), [
            ['one', 0, 100, 0, 24, 24, 2048, 4096, 0],
            ['two', 1, 200, 3 | (1 << 31), 23, 5, 20480, 2048, 1],
            ['three', 0xffff, 50, 7 | (1 << 31), 24, 24, 22528, 2048, 1]
        ].map(([name, tex, dist, flags, on, off, offset, size, pack]) =>
            [name, tex, dist, flags >>> 0, on, off, offset, size, pack]));
        assert.deepEqual(Array.from({length: 2}, (_, i) => {
            const p = textures + i * 132;
            return [readName(p, 128), memory.getInt16(p + 128, true),
                memory.getUint16(p + 130, true)];
        }), [['a', 1, 0], ['b', 0, 0]]);
    } finally {
        if (records) module._free(records);
        if (textures) module._free(textures);
        for (const address of [ids, objects, parents, main, interior, pointers])
            module._free(address);
    }
});

test('bounded real world output matches the frozen Python conversion', {
    skip: !wasm || !existsSync(join(root, 'game/models/gta3.img'))
}, async () => {
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const game = join(root, 'game');
    const data = (await readdir(join(game, 'data'), {
        recursive: true, withFileTypes: true
    })).filter(entry => entry.isFile())
        .map(entry => join(entry.parentPath, entry.name));
    const files = new Map([...data, join(game, 'models/gta3.img'),
        join(game, 'models/gta_int.img')].map(path =>
        [relative(game, path).split(sep).join('/').toLowerCase(),
            {path, size: statSync(path).size}]));
    const handles = new Map();
    try {
        const readRange = (file, start, length) => {
            if (!handles.has(file.path))
                handles.set(file.path, openSync(file.path, 'r'));
            const bytes = Buffer.allocUnsafe(length);
            let used = 0;
            while (used < length) {
                const read = readSync(handles.get(file.path),
                    bytes, used, length - used, start + used);
                if (!read) throw new Error(`Truncated source: ${file.path}`);
                used += read;
            }
            return bytes;
        };
        const result = convertWorld(module, files, {
            sector: 50, lods: true, bounds: [2150, -1950, 2850, -1400]
        }, readRange);
        assert.deepEqual([result.instances, result.models, result.data.length],
            [1651, 527, 879684]);
        if (checksum(await readFile(join(game, 'data/gta.dat'))) ===
            '391881940700b84d9d4a263374a9543e04ec816f3f76f54fdbcabe533f094c01')
            assert.equal(checksum(result.data),
                '1c1d6aac92376256a3e69c82a1282c6014b81b4a80576273da6694a6cb9ff7b0');
    } finally {
        for (const handle of handles.values()) closeSync(handle);
    }
});
