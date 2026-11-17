import assert from 'node:assert/strict';
import {existsSync} from 'node:fs';
import {readFile} from 'node:fs/promises';
import {resolve} from 'node:path';
import test from 'node:test';
import {Parameters} from '../../web/params.mjs';

const root = resolve(import.meta.dirname, '../..');
const steps = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37,
    41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173,
    190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
    6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289,
    16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
];
const indexDelta = [-1, -1, -1, -1, 2, 4, 6, 8];

test('radio belongs only in the separate SD section, not the ROM or User Tracks', async () => {
    const manifest = JSON.parse(await readFile(resolve(root, 'web/resources.json')));
    const planner = new Parameters(manifest, new Map());
    assert.equal(manifest.sections.radio_streams.sd_only, true);
    for (const name of ['CH', 'CO', 'CR', 'DS', 'HC', 'MH', 'MR', 'NJ',
                        'RE', 'RG', 'TK']) {
        assert.equal((await planner.params('radio_streams', 'audio', name)).removed,
            false, name);
        assert.equal((await planner.params('audio_streams', 'audio', name)).removed,
            true, name);
    }
    for (const name of ['AA', 'ADVERTS', 'AMBIENCE', 'BEATS',
                        'CUTSCENE', 'USER_TRACKS'])
        assert.equal((await planner.params('radio_streams', 'audio', name)).removed,
            true, name);
});

test('radio seek checkpoints match sequential ADPCM state and DSST offsets', {
    skip: !existsSync(resolve(root, 'web/assets.mjs'))
}, async () => {
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const count = 25000;
    const pcm = module._malloc(count * 2), pointers = module._malloc(8);
    let data = 0, entries = 0, packPointers = 0, pack = 0;
    try {
        module.HEAP16.set(Int16Array.from({length: count}, (_, i) =>
            i * 173 % 50000 - 25000), pcm >>> 1);
        assert.equal(module._dsnd_encode_simple(
            pcm, count, 11025, 11025, 2, 1, 1, 0, -1, 0,
            pointers, pointers + 4), 1);
        const soundPtr = module.HEAPU32[pointers >>> 2];
        const length = module.HEAPU32[(pointers + 4) >>> 2];
        const sound = Buffer.from(module.HEAPU8.slice(soundPtr, soundPtr + length));
        module._free(soundPtr);
        assert.equal(sound.subarray(0, 4).toString(), 'DSND');
        const raw = sound.readUInt32LE(20);
        const at = 24 + (raw + 3 & ~3);
        const checkpoints = sound.subarray(at);
        assert.equal(checkpoints.length, 16);
        let prediction = sound.readInt16LE(24), index = sound[26];
        for (let i = 0; i < 8192; i++) {
            const code = sound[28 + (i >>> 1)] >> (i & 1 ? 4 : 0) & 15;
            const step = steps[index];
            const diff = (step >> 3) + (code & 4 ? step : 0) +
                (code & 2 ? step >> 1 : 0) + (code & 1 ? step >> 2 : 0);
            prediction = Math.max(-32767, Math.min(32767,
                prediction + (code & 8 ? -diff : diff)));
            index = Math.max(0, Math.min(88, index + indexDelta[code & 7]));
        }
        assert.deepEqual([prediction, index],
            [checkpoints.readInt16LE(4), checkpoints[6]]);
        data = module._malloc(sound.length);
        entries = module._malloc(12);
        packPointers = module._malloc(8);
        module.HEAPU8.set(sound, data);
        const view = new DataView(module.HEAPU8.buffer);
        view.setUint32(entries, 231, true);
        view.setUint32(entries + 4, data, true);
        view.setUint32(entries + 8, sound.length, true);
        assert.equal(module._dsst_pack(entries, 1, 1,
                                      packPointers, packPointers + 4), 1);
        pack = module.HEAPU32[packPointers >>> 2];
        const packedLength = module.HEAPU32[(packPointers + 4) >>> 2];
        const stream = Buffer.from(module.HEAPU8.slice(pack, pack + packedLength));
        assert.equal(stream.subarray(0, 4).toString(), 'DSST');
        assert.deepEqual([stream.readUInt16LE(4), stream.readUInt16LE(6),
            stream.readUInt32LE(8), stream.readUInt32LE(12),
            stream.readUInt32LE(16), stream.readUInt32LE(20),
            stream.readUInt32LE(24)], [2, 1, 231, 28, sound.length,
                                      28 + at, 4]);
        assert.deepEqual(stream.subarray(28 + at, 28 + at + 16), checkpoints);
    } finally {
        if (pack) module._free(pack);
        if (packPointers) module._free(packPointers);
        if (entries) module._free(entries);
        if (data) module._free(data);
        module._free(pointers);
        module._free(pcm);
    }
});
