import assert from 'node:assert/strict';
import {existsSync} from 'node:fs';
import {mkdir, mkdtemp, readFile, writeFile} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';
import test from 'node:test';
import {sync} from '../assets.mjs';
import {convertLooseAssets} from '../../web/loose_assets.mjs';
import {Parameters} from '../../web/params.mjs';

const root = resolve(import.meta.dirname, '../..');
const wasm = existsSync(resolve(root, 'web/assets.mjs'));

function archive(entries) {
    const header = Buffer.alloc(2048);
    header.write('VER2');
    header.writeUInt32LE(entries.length, 4);
    const parts = [header];
    let sector = 1;
    for (const [i, [name, bytes]] of entries.entries()) {
        const count = Math.ceil(bytes.length / 2048);
        header.writeUInt32LE(sector, 8 + i * 32);
        header.writeUInt16LE(count, 12 + i * 32);
        header.writeUInt16LE(count, 14 + i * 32);
        header.write(name, 16 + i * 32);
        parts.push(Buffer.concat([bytes, Buffer.alloc(count * 2048 - bytes.length)]));
        sector += count;
    }
    return Buffer.concat(parts);
}

test('sync extracts selected entries from case-insensitive cutscene IMGs', {
    skip: !wasm
}, async () => {
    const work = await mkdtemp(join(tmpdir(), 'gtasads-cutscene-sync-test-'));
    const source = join(work, 'source'), output = join(work, 'game');
    await mkdir(join(source, 'ANIM'), {recursive: true});
    await mkdir(join(source, 'MODELS'), {recursive: true});
    await writeFile(join(source, 'ANIM/CUTS.IMG'), archive([
        ['PROLOG3.DAT', Buffer.from('camera')],
        ['OTHER.DAT', Buffer.from('other')]
    ]));
    await writeFile(join(source, 'MODELS/CUTSCENE.IMG'), archive([
        ['CSSTEW.DFF', Buffer.from('actor')]
    ]));
    await sync({source, output, sections: ['cutscene_files', 'cutscene_models']});
    assert.equal((await readFile(join(output, 'anim/cuts/prolog3.dat')))
        .subarray(0, 6).toString(), 'camera');
    assert.equal(existsSync(join(output, 'anim/cuts/other.dat')), false);
    assert.equal((await readFile(join(output, 'models/cutscene/csstew.dff')))
        .subarray(0, 5).toString(), 'actor');
});

test('selected cutscene data is packed without duplicate loose output', {
    skip: !wasm
}, async () => {
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const manifest = {global: {texture_scale: 1, model_ratio: 1,
        audio_rate_scale: 1}, defaults: {texture: {}}, sections: {
        cutscene_models: {kind: 'files',
            src: ['models/cutscene/*.dat'], keep_paths: false,
            pack: 'models/cutscene.dspk'}
    }};
    const original = new Map([['models/cutscene/scene.dat',
        new Blob(['original'])]]);
    const outputs = await convertLooseAssets(module,
        new Parameters(manifest, original), original, []);
    const packed = outputs.find(entry => entry.path === 'models/cutscene.dspk');
    assert.ok(packed);
    const bytes = Buffer.from(await packed.file.arrayBuffer());
    assert.equal(bytes.subarray(0, 4).toString(), 'DSPK');
    assert.ok(bytes.includes(Buffer.from('original')));
    assert.equal(outputs.some(entry => entry.path === 'models/cutscene/scene.dat'), false);
});
