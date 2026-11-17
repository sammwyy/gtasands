import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import {closeSync, existsSync, openSync, readSync, statSync} from 'node:fs';
import {mkdtemp, readFile, readdir} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join, relative, resolve, sep} from 'node:path';
import test from 'node:test';
import {build} from '../assets.mjs';
import {convertVehicles} from '../../web/vehicles.mjs';

const root = resolve(import.meta.dirname, '../..');
const game = join(root, 'game');
const wasm = existsSync(join(root, 'web/assets.mjs'));
const checksum = bytes => createHash('sha256').update(bytes).digest('hex');
const sources = [
    'data/vehicles.ide', 'data/handling.cfg', 'data/carcols.dat',
    'data/surface.dat', 'data/surfinfo.dat', 'data/water.dat',
    'data/paths/tracks.dat', 'data/paths/tracks2.dat',
    'data/paths/tracks3.dat', 'data/paths/tracks4.dat'
];

test('real vehicle sections reproduce Python DVEH via both native CLIs and WASM', {
    skip: !wasm || !existsSync(join(game, 'data/vehicles.ide'))
}, async t => {
    const compiled = spawnSync('make', ['-f', 'tools/portable/Makefile',
        'build/vehbuild_cli', 'build/veh_cli'],
    {cwd: root, encoding: 'utf8'});
    if (compiled.status !== 0) return t.skip('C compiler unavailable');
    const index = new Map((await readdir(join(game, 'data'), {
        recursive: true, withFileTypes: true
    })).filter(item => item.isFile()).map(item => {
        const path = join(item.parentPath, item.name);
        return [relative(game, path).split(sep).join('/').toLowerCase(),
            {path, size: statSync(path).size}];
    }));
    const original = createHash('sha256');
    for (const name of [...sources].sort()) {
        const file = index.get(name);
        if (file) original.update(name).update(Buffer.from([0]))
            .update(createHash('sha256').update(
                await readFile(file.path)).digest());
    }
    const handles = new Map();
    try {
        const readRange = (file, offset, size) => {
            if (!handles.has(file.path))
                handles.set(file.path, openSync(file.path, 'r'));
            const data = Buffer.allocUnsafe(size);
            let current = 0;
            while (current < size) {
                const count = readSync(handles.get(file.path), data,
                    current, size - current, offset + current);
                if (!count) throw new Error(`Truncated ${file.path}`);
                current += count;
            }
            return data;
        };
        const {default: createModule} = await import('../../web/assets.mjs');
        const module = await createModule();
        const {data, models} = convertVehicles(module, index, readRange);
        const output = Buffer.from(data);
        assert.deepEqual([models, output.subarray(0, 4).toString('latin1'),
            output.readUInt16LE(4), output.readUInt16LE(6),
            output.readUInt16LE(8), output.readUInt16LE(10),
            output.readUInt16LE(172), output.readUInt32LE(180)],
        [212, 'DVEH', 2, 212, 127, 179, 307, 1949]);
        assert.equal(output.length, 106188);
        const sourceDigest = original.digest('hex');
        if (sourceDigest ===
            '7f6920d11c0e81888283a07649539587f40215cc91f1c2776411220b0961fccf')
            assert.equal(checksum(output),
                '3a84786c81e245cc0a2700095a5a8f19aad6465bc78d3d9f9b2c0da7a73d9b82');
        const work = await mkdtemp(join(tmpdir(), 'gtasads-veh-parity-'));
        const nativePath = join(work, 'native.dveh');
        const autonomous = spawnSync(join(root, 'build/vehbuild_cli'),
            [game, nativePath], {encoding: 'utf8'});
        assert.equal(autonomous.status, 0, autonomous.stderr);
        assert.deepEqual(await readFile(nativePath), output);
        const sections = join(work, 'sections.dveh');
        const single = spawnSync(join(root, 'build/veh_cli'), [
            ...sources.map(name => index.get(name)?.path || '-'), sections
        ], {encoding: 'utf8'});
        assert.equal(single.status, 0, single.stderr);
        assert.deepEqual(await readFile(sections), output);
        const generated = join(work, 'build');
        await build({source: game, output: generated, sections: ['vehicles']});
        assert.deepEqual(await readFile(join(generated,
            'data/vehicles.dveh')), output);
    } finally {
        for (const handle of handles.values()) closeSync(handle);
    }
});
