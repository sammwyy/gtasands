import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import {existsSync} from 'node:fs';
import {mkdir, mkdtemp, readFile, readdir, writeFile} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join, relative, resolve, sep} from 'node:path';
import test from 'node:test';
import {build} from '../assets.mjs';

const root = resolve(import.meta.dirname, '../..');
const wasm = existsSync(join(root, 'web/assets.mjs'));
const checksum = bytes => createHash('sha256').update(bytes).digest('hex');

async function game(files) {
    const directory = await mkdtemp(join(tmpdir(), 'gtasads-entrances-'));
    const source = join(directory, 'source'), output = join(directory, 'nitrofs');
    for (const [name, contents] of Object.entries(files)) {
        const path = join(source, name);
        await mkdir(resolve(path, '..'), {recursive: true});
        await writeFile(path, contents);
    }
    return {directory, source, output};
}

function record(data, index) {
    const start = 8 + index * 64;
    return {
        pos: Array.from({length: 3}, (_, i) => data.readFloatLE(start + i * 4)),
        flags: data.readUInt16LE(start + 44),
        area: data.readInt16LE(start + 46),
        name: data.subarray(start + 48, start + 56).toString().split('\0')[0],
        source: data.readUInt16LE(start + 60),
        link: data.readUInt16LE(start + 62)
    };
}

test('entrance records follow gta.dat order and keep both linked ends', {
    skip: !wasm
}, async () => {
    const {source, output} = await game({
        'data/gta.dat':
            'IPL DATA\\MAPS\\outside.IPL\nIPL DATA\\MAPS\\inside.IPL\n',
        'data/maps/outside.ipl':
            'enex\n100, 200, 10, 0, 2, 3, 8, 1, 2, 1000, 90, 1, 4, "HOUSE", 0, 2, 0, 24\nend\n',
        'data/maps/inside.ipl':
            'enex\n1, 2, 1000, 90, 2, 3, 8, 100, 200, 10, 180, 0, 0, "HOUSE", 0, 2, 0, 24\nend\n'
    });
    await build({source, output, sections: ['entry_exits']});
    const data = await readFile(join(output, 'data/entry_exits.denx'));
    assert.equal(data.subarray(0, 4).toString(), 'DENX');
    assert.equal(data.readUInt16LE(4), 3);
    assert.equal(data.readUInt16LE(6), 2);
    const first = record(data, 0), second = record(data, 1);
    assert.deepEqual(first.pos, [100, 200, 10]);
    assert.deepEqual([first.flags, first.area], [4, 1]);
    assert.deepEqual([second.flags, second.area], [0, 0]);
    assert.equal(first.name, 'house');
    assert.equal(second.name, 'house');
    assert.deepEqual([first.source, first.link, second.source, second.link],
                     [0, 1, 1, 0]);
});

test('entrances distinguish a low-altitude interior from a direct warp', {
    skip: !wasm
}, async () => {
    const {source, output} = await game({
        'data/gta.dat': 'IPL DATA\\MAPS\\room.IPL\n',
        'data/maps/room.ipl':
            'inst\n10, room, 1, 681, -456, -24, 0, 0, 0, 1, -1\nend\n' +
            'enex\n681, -456, -24, 0, 2, 2, 8, 100, 200, 20, 90, 0, 0, "ROOM", 0, 2, 0, 24\n' +
            '100, 200, 20, 0, 2, 2, 8, 110, 210, 25, 90, 0, 0, "", 0, 2, 0, 24\nend\n'
    });
    await build({source, output, sections: ['entry_exits']});
    const data = await readFile(join(output, 'data/entry_exits.denx'));
    assert.equal(data.readUInt16LE(6), 2);
    assert.deepEqual([record(data, 0).source, record(data, 0).link], [1, 0xffff]);
    assert.deepEqual([record(data, 1).source, record(data, 1).link], [0, 0xffff]);
});

test('map points de-duplicate and keep only exterior points within map bounds', {
    skip: !wasm
}, async () => {
    const {source, output} = await game({
        'data/gta.dat':
            'IPL DATA\\MAPS\\city.IPL\nIPL DATA\\MAPS\\interior\\room.IPL\n',
        'data/maps/city.ipl':
            'inst\n0, building\nend\nenex\n' +
            '100.2, -200.8, 12, 0, 1, 1, 8, 0, 0, 0, 0, 0, 4, "AMMUN1", 0, 2, 0, 24\n' +
            '100.2, -200.8, 12, 0, 1, 1, 8, 0, 0, 0, 0, 0, 4, "AMMUN1", 0, 2, 0, 24\n' +
            '200, -300, 12, 0, 1, 1, 8, 0, 0, 0, 0, 0, 4, "BAR2", 0, 2, 0, 24\nend\n',
        'data/maps/interior/room.ipl':
            'enex\n10, 20, 1000, 0, 1, 1, 8, 0, 0, 0, 0, 5, 0, "GYM1", 0, 2, 0, 24\nend\n'
    });
    await build({source, output, sections: ['map_points']});
    const data = await readFile(join(output, 'data/map_points.dpoi'));
    assert.equal(data.subarray(0, 4).toString(), 'DPOI');
    assert.equal(data.readUInt16LE(4), 2);
    assert.equal(data.readUInt16LE(6), 2);
    assert.deepEqual([data.readInt16LE(8), data.readInt16LE(10),
        data[12], data[13]], [100, -201, 0, 0]);
    assert.deepEqual([data.readInt16LE(14), data.readInt16LE(16),
        data[18], data[19]], [200, -300, 12, 0]);
});

test('native entrances and points reject out-of-map coordinates and duplicates', {
    skip: !wasm
}, async t => {
    const built = spawnSync('make', ['-f', 'tools/portable/Makefile',
        'build/enexbuild_cli'], {cwd: root, encoding: 'utf8'});
    if (built.status !== 0) return t.skip('C compiler unavailable');
    const {source, output, directory} = await game({
        'data/gta.dat': 'IPL DATA\\MAPS\\city.IPL\n',
        'data/maps/city.ipl':
            'enex\n' +
            '100.5, -200.5, 12, 0, 1, 1, 8, 0, 0, 0, 0, 0, 4, "AMMUN1", 0, 2, 0, 24\n' +
            '100.5, -200.5, 12, 0, 1, 1, 8, 0, 0, 0, 0, 0, 4, "AMMUN1", 0, 2, 0, 24\n' +
            '2999.5, -3000.5, 12, 0, 1, 1, 8, 0, 0, 0, 0, 0, 4, "BAR2", 0, 2, 0, 24\n' +
            '200, -300, 12, 0, 1, 1, 8, 0, 0, 0, 0, 0, 4, "GYM1", 0, 2, 0, 24\nend\n'
    });
    await build({source, output, sections: ['entry_exits', 'map_points']});
    const enex = await readFile(join(output, 'data/entry_exits.denx'));
    const points = await readFile(join(output, 'data/map_points.dpoi'));
    assert.equal(enex.readUInt16LE(6), 4);
    assert.equal(points.readUInt16LE(6), 2);
    const native = join(directory, 'native.denx');
    const run = spawnSync(join(root, 'build/enexbuild_cli'), [source, native],
        {encoding: 'utf8'});
    assert.equal(run.status, 0, run.stderr);
    assert.deepEqual(await readFile(native), enex);
});

test('native CLI entrances and points match WASM without overwriting', {
    skip: !wasm
}, async t => {
    const built = spawnSync('make', ['-f', 'tools/portable/Makefile',
        'build/assets_data_cli'], {cwd: root, encoding: 'utf8'});
    if (built.status !== 0) return t.skip('C compiler unavailable');
    const {source, output, directory} = await game({
        'data/gta.dat': 'IPL DATA\\MAPS\\city.IPL\n',
        'data/maps/city.ipl':
            'enex\n' +
            '100.5, -200.5, 12, 0, 1, 1, 8, 0, 0, 0, 0, 0, 4, "AMMUN1", 0, 2, 0, 24\n' +
            '200, -300, 12, 0, 1, 1, 8, 0, 0, 0, 0, 0, 4, "GYM1", 0, 2, 0, 24\nend\n'
    });
    await build({source, output, sections: ['entry_exits', 'map_points']});
    const native = join(directory, 'native');
    await mkdir(native);
    const cli = join(root, 'build/assets_data_cli');
    const args = [source, native, 'entry_exits', 'map_points'];
    const run = spawnSync(cli, args, {encoding: 'utf8'});
    assert.equal(run.status, 0, run.stderr);
    for (const name of ['entry_exits.denx', 'map_points.dpoi'])
        assert.deepEqual(await readFile(join(native, 'data', name)),
            await readFile(join(output, 'data', name)));
    const repeat = spawnSync(cli, args, {encoding: 'utf8'});
    assert.notEqual(repeat.status, 0);
    assert.match(repeat.stderr, /already exists/);
    for (const name of ['entry_exits.denx', 'map_points.dpoi'])
        assert.deepEqual(await readFile(join(native, 'data', name)),
            await readFile(join(output, 'data', name)));
});

test('native CLI rejects unknown sections without creating outputs', async t => {
    const built = spawnSync('make', ['-f', 'tools/portable/Makefile',
        'build/assets_data_cli'], {cwd: root, encoding: 'utf8'});
    if (built.status !== 0) return t.skip('C compiler unavailable');
    const {source, directory} = await game({});
    await mkdir(source);
    const output = join(directory, 'output');
    await mkdir(output);
    const result = spawnSync(join(root, 'build/assets_data_cli'),
        [source, output, 'missing']);
    assert.equal(result.status, 2);
    assert.equal(existsSync(join(output, 'data')), false);
});

test('real entrance conversion matches native C and existing assets', {
    skip: !wasm || !existsSync(join(root, 'game/data/gta.dat'))
}, async t => {
    const built = spawnSync('make', ['-f', 'tools/portable/Makefile',
        'build/enexbuild_cli'], {cwd: root, encoding: 'utf8'});
    if (built.status !== 0) return t.skip('C compiler unavailable');
    const {directory, output} = await game({});
    const source = join(root, 'game');
    await build({source, output, sections: ['entry_exits', 'map_points']});
    const native = join(directory, 'native.denx');
    const run = spawnSync(join(root, 'build/enexbuild_cli'), [source, native],
        {encoding: 'utf8'});
    assert.equal(run.status, 0, run.stderr);
    const converted = await readFile(join(output, 'data/entry_exits.denx'));
    assert.deepEqual(await readFile(native), converted);
    assert.deepEqual(await readFile(join(root, 'nitrofs/data/entry_exits.denx')),
        converted);
    assert.deepEqual(await readFile(join(root, 'nitrofs/data/map_points.dpoi')),
        await readFile(join(output, 'data/map_points.dpoi')));
    if (checksum(await readFile(join(source, 'data/gta.dat'))) ===
        '391881940700b84d9d4a263374a9543e04ec816f3f76f54fdbcabe533f094c01') {
        assert.equal(checksum(converted),
            '4d51f6fc0196de026f1d8d73166792fd28f0998701ee79a2195498d89d4b6812');
        assert.equal(checksum(await readFile(join(output, 'data/map_points.dpoi'))),
            '30047dd397a89220597e556be211a4ca25e6e14ac42cb9cd7253c1addc057417');
        assert.equal(converted.readUInt16LE(6), 376);
    }
    const cli = spawnSync('make', ['-f', 'tools/portable/Makefile',
        'build/enex_cli'], {cwd: root, encoding: 'utf8'});
    if (cli.status !== 0) return t.skip('C compiler unavailable');
    const paths = new Map((await readdir(join(source, 'data'), {
        recursive: true, withFileTypes: true
    })).filter(entry => entry.isFile()).map(entry => {
        const path = join(entry.parentPath, entry.name);
        return [relative(source, path).split(sep).join('/').toLowerCase(), path];
    }));
    const files = (await readFile(join(source, 'data/gta.dat'), 'utf8'))
        .split(/\r?\n/).map(line => line.split('#', 1)[0].trim())
        .filter(line => /^IPL\s+/i.test(line) && /\.ipl$/i.test(line))
        .map(line => paths.get(line.replace(/^IPL\s+/i, '')
            .replaceAll('\\', '/').toLowerCase())).filter(Boolean);
    const direct = join(directory, 'direct.denx');
    const runCLI = spawnSync(join(root, 'build/enex_cli'),
        [direct, ...files], {encoding: 'utf8'});
    assert.equal(runCLI.status, 0, runCLI.stderr);
    assert.deepEqual(await readFile(direct), converted);
});
