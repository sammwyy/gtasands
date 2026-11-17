import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import {closeSync, existsSync, openSync, readSync, statSync} from 'node:fs';
import {mkdtemp, readFile, readdir} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join, relative, resolve, sep} from 'node:path';
import test from 'node:test';
import {convertPeds} from '../../web/peds.mjs';

const root = resolve(import.meta.dirname, '../..');
const wasm = existsSync(join(root, 'web/assets.mjs'));

test('pedestrian and vehicle paths retain Python bounds, flags and link order', {
    skip: !wasm
}, async () => {
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const data = Buffer.alloc(20 + 3 * 28 + 3 * 4);
    for (const [i, value] of [3, 1, 2, 0, 3].entries())
        data.writeUInt32LE(value, i * 4);
    for (const [i, [x, y, base, width, flags]] of [
        [16, -8, 0, 4, 0x10001],
        [32, 16, 1, 3, 0x20002],
        [3200, -1000, 3, 2, 0]
    ].entries()) {
        const p = 20 + i * 28 + 8;
        data.writeInt16LE(x, p);
        data.writeInt16LE(y, p + 2);
        data.writeInt16LE(base, p + 8);
        data[p + 14] = width;
        data.writeUInt32LE(flags, p + 16);
    }
    for (const [i, [area, node]] of [[0, 1], [0, 0], [0, 2]].entries()) {
        data.writeUInt16LE(area, 104 + i * 4);
        data.writeUInt16LE(node, 106 + i * 4);
    }
    const input = module._malloc(data.length);
    const bounds = module._malloc(32);
    const pointers = module._malloc(8);
    let output = 0;
    try {
        module.HEAPU8.set(data, input);
        const view = new DataView(module.HEAPU8.buffer);
        for (const [i, value] of [-70, -70, 70, 70].entries())
            view.setFloat64(bounds + i * 8, value, true);
        for (const [vehicles, expected] of [
            [0, [1, 32, 16, 0, 3, 131074, [[0, 0], [0, 2]]]],
            [1, [0, 16, -8, 0, 4, 65537, [[0, 1]]]]
        ]) {
            assert.equal(module._pedpath_parse(input, data.length,
                bounds, vehicles, pointers, pointers + 4), 1);
            const memory = new DataView(module.HEAPU8.buffer);
            output = memory.getUint32(pointers, true);
            assert.equal(memory.getUint32(pointers + 4, true), 1);
            const count = memory.getUint8(output + 11);
            assert.deepEqual([
                memory.getUint32(output, true),
                memory.getInt16(output + 4, true),
                memory.getInt16(output + 6, true),
                memory.getInt16(output + 8, true),
                memory.getUint8(output + 10),
                memory.getUint32(output + 12, true),
                Array.from({length: count}, (_, i) => [
                    memory.getUint16(output + 16 + i * 4, true),
                    memory.getUint16(output + 18 + i * 4, true)
                ])
            ], expected);
            module._free(output);
            output = 0;
        }
    } finally {
        if (output) module._free(output);
        module._free(input);
        module._free(bounds);
        module._free(pointers);
    }
});

test('bounded real ped paths and population reproduce Python DPED and native CLI', {
    skip: !wasm || !existsSync(join(root, 'game/data/peds.ide'))
}, async t => {
    const built = spawnSync('make', ['-f', 'tools/portable/Makefile',
        'build/pedbuild_cli'], {cwd: root, encoding: 'utf8'});
    if (built.status !== 0) return t.skip('C compiler unavailable');
    const game = join(root, 'game');
    const paths = (await readdir(join(game, 'data'), {
        recursive: true, withFileTypes: true
    })).filter(entry => entry.isFile())
        .map(entry => join(entry.parentPath, entry.name));
    const files = new Map(paths.map(path => [
        relative(game, path).split(sep).join('/').toLowerCase(),
        {path, size: statSync(path).size}
    ]));
    const input = createHash('sha256');
    const expected = new Set([
        'peds.ide', 'pedstats.dat', 'pedgrp.dat', 'cargrp.dat',
        'vehicles.ide', 'popcycle.dat', 'animgrp.dat', 'info.zon',
        'script/main.scm'
    ].map(name => `data/${name}`));
    const important = [...files].filter(([name]) =>
        expected.has(name) || /^data\/paths\/nodes.*\.dat$/.test(name))
        .sort(([a], [b]) => a < b ? -1 : a > b ? 1 : 0);
    for (const [name, file] of important)
        input.update(name).update(Buffer.from([0]))
            .update(createHash('sha256').update(
                await readFile(file.path)).digest());
    const handles = new Map();
    try {
        const readRange = (file, start, length) => {
            if (!handles.has(file.path))
                handles.set(file.path, openSync(file.path, 'r'));
            const bytes = Buffer.allocUnsafe(length);
            let read = 0;
            while (read < length) {
                const size = readSync(handles.get(file.path), bytes,
                    read, length - read, start + read);
                if (!size) throw new Error(`Truncated ${file.path}`);
                read += size;
            }
            return bytes;
        };
        const {default: createModule} = await import('../../web/assets.mjs');
        const module = await createModule();
        const result = convertPeds(module, files,
            {bounds: [2150, -1950, 2850, -1400]}, readRange);
        assert.equal(result.models, 275);
        assert.equal(result.nodes, 1602);
        assert.equal(result.data.length, 83574);
        const path = join(await mkdtemp(join(tmpdir(),
            'gtasads-bounded-peds-')), 'peds.dped');
        const native = spawnSync(join(root, 'build/pedbuild_cli'), [
            game, path, '2150', '-1950', '2850', '-1400'
        ], {encoding: 'utf8'});
        assert.equal(native.status, 0, native.stderr);
        assert.deepEqual(await readFile(path), Buffer.from(result.data));
        if (input.digest('hex') ===
            'cc3b78d651eceba46eaa4b1bd7ab12cc50b6a0f222169e8b20430e3d51e1a7c7')
            assert.equal(createHash('sha256').update(result.data).digest('hex'),
                '00cf253aa5caec1f4c48b6bf5be7807c24849633ed4612b5b67152aef7c9edf2');
    } finally {
        for (const handle of handles.values()) closeSync(handle);
    }
});
