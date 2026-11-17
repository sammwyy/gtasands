import assert from 'node:assert/strict';
import {existsSync} from 'node:fs';
import {mkdir, mkdtemp, readFile, stat, utimes, writeFile} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';
import test from 'node:test';
import {build, packageAssets, sync} from '../assets.mjs';

test('sync stages source files with their normalized paths', async () => {
    const root = await mkdtemp(join(tmpdir(), 'gtasads-sync-test-'));
    const source = join(root, 'source'), output = join(root, 'game');
    await mkdir(join(source, 'TEXT'), {recursive: true});
    await writeFile(join(source, 'TEXT', 'AMERICAN.GXT'), 'English');
    await writeFile(join(source, 'TEXT', 'SPANISH.GXT'), 'Spanish');
    await sync({source, output, sections: ['text']});
    assert.equal((await readFile(join(output, 'text/american.gxt'))).toString(), 'English');
    assert.equal((await readFile(join(output, 'text/spanish.gxt'))).toString(), 'Spanish');
});

test('radio sync stages lookup files separately without overwriting custom ones', async () => {
    const root = await mkdtemp(join(tmpdir(), 'gtasads-radio-sync-test-'));
    const source = join(root, 'source'), output = join(root, 'game');
    await mkdir(join(source, 'audio/CONFIG'), {recursive: true});
    await mkdir(join(source, 'audio/STREAMS'), {recursive: true});
    await writeFile(join(source, 'audio/CONFIG/TrakLkup.dat'), 'original');
    await writeFile(join(source, 'audio/CONFIG/StrmPaks.dat'), 'names');
    await writeFile(join(source, 'audio/STREAMS/CH'), 'station');
    await sync({source, output, sections: ['radio_streams']});
    const target = join(output, 'audio/radio_config/traklkup.dat');
    assert.equal((await readFile(target)).toString(), 'original');
    await writeFile(target, 'matching custom lookup');
    await sync({source, output, sections: ['radio_streams']});
    assert.equal((await readFile(target)).toString(), 'matching custom lookup');
});

test('world sync stages both exterior and interior source archives', async () => {
    const root = await mkdtemp(join(tmpdir(), 'gtasads-world-sync-test-'));
    const source = join(root, 'source'), output = join(root, 'game');
    await mkdir(join(source, 'models'), {recursive: true});
    await writeFile(join(source, 'models/gta3.img'), 'world');
    await writeFile(join(source, 'models/gta_int.img'), 'interiors');
    await sync({source, output, sections: ['world']});
    assert.equal((await readFile(join(output, 'models/gta3.img'))).toString(), 'world');
    assert.equal((await readFile(join(output, 'models/gta_int.img'))).toString(),
                 'interiors');
});

test('audio config staging normalizes names expected by the converters', async () => {
    const root = await mkdtemp(join(tmpdir(), 'gtasads-audio-sync-test-'));
    const source = join(root, 'source'), output = join(root, 'game');
    await mkdir(join(source, 'AUDIO/CONFIG'), {recursive: true});
    await writeFile(join(source, 'AUDIO/CONFIG/BANKLKUP.DAT'), Buffer.alloc(12));
    await writeFile(join(source, 'AUDIO/CONFIG/PAKFILES.DAT'), 'FEET');
    await sync({source, output, sections: ['audio_sfx']});
    assert.equal((await readFile(join(output, 'audio/config/pakfiles.dat')))
        .subarray(0, 4).toString(), 'FEET');
    assert.equal((await readFile(join(output, 'audio/config/banklkup.dat'))).length, 12);
});

test('CLI converts entrances through the same WASM module as the browser', {
    skip: !existsSync(resolve(import.meta.dirname, '../../web/assets.mjs'))
}, async () => {
    const root = await mkdtemp(join(tmpdir(), 'gtasads-entrances-test-'));
    const source = join(root, 'source'), output = join(root, 'nitrofs');
    await mkdir(join(source, 'data/maps'), {recursive: true});
    await writeFile(join(source, 'data/gta.dat'), 'IPL DATA\\MAPS\\city.IPL\n');
    await writeFile(join(source, 'data/maps/city.ipl'),
        'enex\n100, 200, 12, 0, 1, 1, 8, 0, 0, 0, 0, 0, 4, "AMMUN1", 0, 2, 0, 24\nend\n');
    await build({source, output, sections: ['entry_exits', 'map_points']});
    const enex = await readFile(join(output, 'data/entry_exits.denx'));
    const points = await readFile(join(output, 'data/map_points.dpoi'));
    assert.equal(enex.subarray(0, 4).toString(), 'DENX');
    assert.equal(enex.readUInt16LE(6), 1);
    assert.equal(points.subarray(0, 4).toString(), 'DPOI');
    assert.equal(points.readUInt16LE(6), 1);
    const target = join(output, 'data/entry_exits.denx');
    const old = new Date('2020-01-01T00:00:00Z');
    await utimes(target, old, old);
    await build({source, output, sections: ['entry_exits', 'map_points']});
    assert.equal((await stat(target)).mtimeMs, old.getTime());
    await writeFile(join(source, 'data/maps/city.ipl'),
        'enex\n100, 200, 12, 0, 1, 1, 8, 0, 0, 0, 0, 0, 4, "AMMUN1", 0, 2, 0, 24\nend\n# updated\n');
    await build({source, output, sections: ['entry_exits']});
    assert.ok((await stat(target)).mtimeMs > old.getTime());
});

test('partial conversion reads an original install without staging it under game/', {
    skip: !existsSync(resolve(import.meta.dirname, '../../web/assets.mjs'))
}, async () => {
    const work = await mkdtemp(join(tmpdir(), 'gtasads-direct-package-test-'));
    const source = join(work, 'SA'), output = join(work, 'nitrofs');
    await mkdir(join(source, 'TEXT'), {recursive: true});
    await writeFile(join(source, 'TEXT/AMERICAN.GXT'), 'english');
    await writeFile(join(source, 'TEXT/SPANISH.GXT'), 'spanish');
    await build({source, output, sections: ['text']});
    assert.equal((await readFile(join(output, 'text/american.gxt'))).toString(),
                 'english');
    assert.equal((await readFile(join(output, 'text/spanish.gxt'))).toString(),
                 'spanish');
    assert.equal(existsSync(join(work, 'game')), false);
    const required = ['models/gta3.dspk', 'models/gta_int.dspk',
        'data/world.dwld', 'data/vehicles.dveh', 'data/peds.dped'];
    for (const path of required) {
        await mkdir(resolve(output, path, '..'), {recursive: true});
        await writeFile(join(output, path), path);
    }
    const rom = join(work, 'code.nds'), zip = join(work, 'package.zip');
    await writeFile(rom, 'code');
    await packageAssets(rom, zip, output,
        [...required, 'text/american.gxt', 'text/spanish.gxt']);
    const content = await readFile(zip);
    assert.ok(content.includes(Buffer.from('gtasads/text/american.gxt')));
    await assert.rejects(packageAssets(rom, zip, output,
        [...required, 'text/american.gxt']), {code: 'EEXIST'});
});

test('a missing source IMG cannot leave a partial SD package', {
    skip: !existsSync(resolve(import.meta.dirname, '../../web/assets.mjs'))
}, async () => {
    const work = await mkdtemp(join(tmpdir(), 'gtasads-missing-img-test-'));
    const source = join(work, 'SA'), output = join(work, 'nitrofs');
    await mkdir(source);
    const rom = join(work, 'code.dsi'), zip = join(work, 'package.zip');
    await writeFile(rom, 'code');
    await assert.rejects(build({source, output, sections: ['anim_img'],
        rom, zip}), /Missing original IMG/);
    assert.equal(existsSync(zip), false);
});

test('CLI writes a ROM and assets into a ZIP', async () => {
    const root = await mkdtemp(join(tmpdir(), 'gtasads-zip-test-'));
    const output = join(root, 'nitrofs'), rom = join(root, 'gtasads.nds');
    const paths = ['models/gta3.dspk', 'models/gta_int.dspk',
        'data/world.dwld', 'data/vehicles.dveh', 'data/peds.dped'];
    for (const path of paths) {
        const target = join(output, path);
        await mkdir(resolve(target, '..'), {recursive: true});
        await writeFile(target, path);
    }
    await writeFile(rom, 'code');
    const zipPath = join(root, 'package.zip');
    await packageAssets(rom, zipPath, output, paths);
    const zip = await readFile(zipPath);
    assert.equal(zip.subarray(0, 4).toString(), 'PK\x03\x04');
    assert.ok(zip.includes(Buffer.from('gtasads.nds')));
    assert.ok(zip.includes(Buffer.from('gtasads/data/world.dwld')));
    await assert.rejects(packageAssets(rom, zipPath, output, paths), {
        code: 'EEXIST'
    });
});
