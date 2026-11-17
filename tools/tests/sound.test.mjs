import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {createReadStream, existsSync} from 'node:fs';
import {open, readFile, stat} from 'node:fs/promises';
import {resolve} from 'node:path';
import test from 'node:test';
import {Parameters} from '../../web/params.mjs';
import {convertSFX, convertStreams} from '../../web/sound_assets.mjs';

const root = resolve(import.meta.dirname, '../..');
const game = resolve(root, 'game');
const wasm = existsSync(resolve(root, 'web/assets.mjs'));

function hash(bytes) {
    return createHash('sha256').update(bytes).digest('hex');
}

async function hashFile(path) {
    const checksum = createHash('sha256');
    for await (const chunk of createReadStream(path)) checksum.update(chunk);
    return checksum.digest('hex');
}

async function sources(names) {
    return new Map(await Promise.all(names.map(async ([name, file]) =>
        [name, new Blob([await readFile(resolve(game, file))])])));
}

test('real SFX and Vorbis packs agree with the native Python reference', {
    skip: !wasm || !existsSync(resolve(game, 'audio/sfx/FEET'))
}, async () => {
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const manifest = JSON.parse(await readFile(resolve(root, 'web/resources.json')));
    const paths = [
        ['audio/config/pakfiles.dat', 'audio/CONFIG/PakFiles.dat'],
        ['audio/config/banklkup.dat', 'audio/CONFIG/BankLkup.dat'],
        ['audio/sfx/feet', 'audio/sfx/FEET'],
        ['audio/config/strmpaks.dat', 'audio/CONFIG/StrmPaks.dat'],
        ['audio/config/traklkup.dat', 'audio/CONFIG/TrakLkup.dat'],
        ['audio/streams/aa', 'audio/streams/AA']
    ];
    const original = await sources(paths);
    const planner = new Parameters(manifest, original);
    const sfx = await convertSFX(module, planner, original, new Set(), () => {}, 2);
    const streams = await convertStreams(module, planner, original,
        new Set(), () => {}, 1);
    assert.equal(sfx.banks, 2);
    assert.equal(streams.tracks, 1);
    const outputs = new Map(await Promise.all([...sfx.files, ...streams.files]
        .map(async entry => [entry.path.split('/').at(-1),
            Buffer.from(await entry.file.arrayBuffer())])));
    assert.deepEqual(new Set(outputs.keys()),
        new Set(['feet.dsfx', 'banks.dsbl', 'aa.dsst']));
    const known = {
        'audio/sfx/feet': '57eeebdf4205f6db6ff388f8b2099a60ce507ee07a5d8b3defad0383d55e31d0',
        'audio/streams/aa': 'e8e854b3155d20ab8e71920618f53ba9f6d62251bf48766ead123b3d876dbfd4',
        'audio/config/pakfiles.dat': '36bc8f4814d4ef4dc3b7c43ae5c26f0de1feeda1bc15cdf2250456d2927bc57c',
        'audio/config/banklkup.dat': 'b89a66ddef4e5da74c4c13abb38a9aa7985397bee45b00e080da89f07a1eee50',
        'audio/config/strmpaks.dat': '03e32ee0efb3505d0cea13952370d24a417799b085946087c851060d4120dea0',
        'audio/config/traklkup.dat': 'adf22fcdfe798c7ad59f1b8fbf6a71574fbf06f1ca39a074ad2b3163c341c0d9'
    };
    if ((await Promise.all(Object.entries(known)
        .map(async ([path, expected]) =>
            hash(Buffer.from(await original.get(path).arrayBuffer())) === expected)))
        .every(Boolean)) {
        assert.deepEqual(Object.fromEntries([...outputs]
            .map(([name, bytes]) => [name, hash(bytes)])), {
            'aa.dsst': 'ed9237d23dee34f2bc733ace5806b4c063e23afac76fcc8a3312fd646e4d0792',
            'banks.dsbl': '49c6f516cb682f44296d100ce6da821be20cb4d0117b93601ba382ab957beb8f',
            'feet.dsfx': 'f9bb140249088240577eb47b4ebc5a956ab58dff59784932130908d63a340c5e'
        });
    }
});

test('real CUTSCENE stream realigns the encrypted Vorbis track', {
    skip: !wasm || !existsSync(resolve(game, 'audio/streams/CUTSCENE'))
}, async () => {
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const manifest = JSON.parse(await readFile(resolve(root, 'web/resources.json')));
    const names = [
        ['audio/config/strmpaks.dat', 'audio/CONFIG/StrmPaks.dat'],
        ['audio/config/traklkup.dat', 'audio/CONFIG/TrakLkup.dat']
    ];
    const original = await sources(names);
    const path = resolve(game, 'audio/streams/CUTSCENE');
    const size = (await stat(path)).size;
    const handle = await open(path);
    try {
        original.set('audio/streams/cutscene', {
            size, slice(start, end) {
                return {arrayBuffer: async () => {
                    const length = Math.max(0, Math.min(size, end) - start);
                    const bytes = Buffer.allocUnsafe(length);
                    let offset = 0;
                    while (offset < length) {
                        const {bytesRead} = await handle.read(bytes, offset,
                            length - offset, start + offset);
                        if (!bytesRead) throw new Error('Truncated CUTSCENE stream.');
                        offset += bytesRead;
                    }
                    return bytes;
                }};
            }
        });
        const result = await convertStreams(module,
            new Parameters(manifest, original), original, new Set(), () => {}, 707);
        assert.equal(result.tracks, 1);
        assert.equal(result.files[0].path, 'audio/streams/cutscene.dsst');
        const converted = Buffer.from(await result.files[0].file.arrayBuffer());
        assert.equal(converted.subarray(0, 4).toString(), 'DSST');
        assert.equal(converted.readUInt32LE(8), 706);
        if (hash(Buffer.from(await original.get('audio/config/traklkup.dat')
            .arrayBuffer())) ===
                'adf22fcdfe798c7ad59f1b8fbf6a71574fbf06f1ca39a074ad2b3163c341c0d9' &&
            await hashFile(path) ===
                'dd44b17b81d9fe28164d9a0ce6c8b0e7a715e1a90cc4787e5b2645bb5b16d35c')
            assert.equal(hash(converted),
                '3962e1e27402db689cd8ac480859d2c3ec65f9818290217f487a00ebb6377e9a');
    } finally {
        await handle.close();
    }
});
