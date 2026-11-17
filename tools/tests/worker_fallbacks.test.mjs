import assert from 'node:assert/strict';
import test from 'node:test';
import {convertOriginalWorld} from '../../web/local_world.mjs';
import {convertOriginalPeds} from '../../web/local_peds.mjs';
import {convertOriginalVehicles} from '../../web/local_vehicles.mjs';
import {convertOriginalEntrances} from '../../web/local_entrances.mjs';
import {convertOriginalSound} from '../../web/local_sound.mjs';
import {pcm16FromAudioBuffer} from '../../web/audio_pcm.mjs';

test('missing world is built from the original folder', async () => {
    const source = new Map([
        ['data/gta.dat', new Blob([])],
        ['models/gta3.img', new Blob([])],
        ['models/gta_int.img', new Blob([])]
    ]);
    let terminated = false;
    const workerFactory = () => ({
        postMessage({files, options}) {
            assert.equal(files, source);
            assert.equal(options.sector, 50);
            queueMicrotask(() => this.onmessage({
                data: {models: 11, instances: 20,
                       bytes: Uint8Array.of(68, 87, 76, 68).buffer}
            }));
        },
        terminate() {terminated = true;}
    });
    const messages = [];
    const generated = await convertOriginalWorld(source, [],
        {sector: 50}, message => messages.push(message), workerFactory);
    assert.equal(terminated, true);
    assert.equal(generated[0].path, 'data/world.dwld');
    assert.equal(generated[0].file.size, 4);
    assert.equal(messages.length, 2);
    assert.deepEqual(await convertOriginalWorld(source, generated,
        {}, () => assert.fail('already converted'),
        () => assert.fail('worker not needed')), []);
    await assert.rejects(convertOriginalWorld(new Map(), []));
});

test('missing peds are built locally with the requested bounds', async () => {
    const source = new Map([['data/peds.ide', new Blob([])]]);
    let terminated = false;
    const workerFactory = () => ({
        postMessage({files, options}) {
            assert.equal(files, source);
            assert.deepEqual(options.bounds, [-100, -100, 100, 100]);
            queueMicrotask(() => this.onmessage({
                data: {models: 2, nodes: 3,
                       bytes: Uint8Array.of(68, 80, 69, 68).buffer}
            }));
        },
        terminate() {terminated = true;}
    });
    const messages = [];
    const generated = await convertOriginalPeds(source, [],
        {bounds: [-100, -100, 100, 100]},
        message => messages.push(message), workerFactory);
    assert.equal(terminated, true);
    assert.equal(generated[0].path, 'data/peds.dped');
    assert.equal(generated[0].file.size, 4);
    assert.equal(messages.length, 2);
    assert.deepEqual(await convertOriginalPeds(source, generated,
        {}, () => assert.fail('already converted'),
        () => assert.fail('worker not needed')), []);
});

test('missing vehicles are built locally', async () => {
    const source = new Map([['data/vehicles.ide', new Blob([])]]);
    let terminated = false;
    const workerFactory = () => ({
        postMessage({files}) {
            assert.equal(files, source);
            queueMicrotask(() => this.onmessage({
                data: {models: 2, bytes: Uint8Array.of(68, 86, 69, 72).buffer}
            }));
        },
        terminate() {terminated = true;}
    });
    const built = await convertOriginalVehicles(source, [], () => {}, workerFactory);
    assert.equal(terminated, true);
    assert.equal(built[0].path, 'data/vehicles.dveh');
    assert.equal(built[0].file.size, 4);
    assert.deepEqual(await convertOriginalVehicles(source, built,
        () => assert.fail('already converted'),
        () => assert.fail('worker not needed')), []);
});

test('missing entrances and points are built together locally', async () => {
    const source = new Map([['data/gta.dat', new Blob([])]]);
    let terminated = false;
    const workerFactory = () => ({
        postMessage({files}) {
            assert.equal(files, source);
            queueMicrotask(() => this.onmessage({
                data: {count: 2,
                       denx: Uint8Array.of(68, 69, 78, 88).buffer,
                       dpoi: Uint8Array.of(68, 80, 79, 73).buffer}
            }));
        },
        terminate() {terminated = true;}
    });
    const built = await convertOriginalEntrances(source, [], () => {}, workerFactory);
    assert.equal(terminated, true);
    assert.deepEqual(built.map(entry => entry.path),
        ['data/entry_exits.denx', 'data/map_points.dpoi']);
    assert.deepEqual(await convertOriginalEntrances(source, built,
        () => assert.fail('already converted'),
        () => assert.fail('worker not needed')), []);
});

test('PCM mixing rounds ties to even and trims padded samples', () => {
    const mono = new Float32Array([0, 0.5 / 32768, 1.5 / 32768,
        -0.5 / 32768, -1.5 / 32768, 1, -1]);
    const buffer = channels => ({
        length: channels[0].length,
        numberOfChannels: channels.length,
        getChannelData: index => channels[index]
    });
    assert.deepEqual([...pcm16FromAudioBuffer(buffer([mono]), mono.length)],
        [0, 0, 2, 0, -2, 32767, -32768]);
    const left = new Float32Array([1, 2, -3, 4, 999].map(x => x / 32768));
    const right = new Float32Array([0, 1, 0, 3, 999].map(x => x / 32768));
    assert.deepEqual([...pcm16FromAudioBuffer(buffer([left, right]), 4)],
        [0, 2, -2, 4]);
    assert.throws(() => pcm16FromAudioBuffer(buffer([mono]), 8));
    assert.throws(() => pcm16FromAudioBuffer(buffer([mono, mono, mono]), 1));
});

test('audio worker merges its generated outputs with existing assets', async () => {
    const original = new Map([['audio/config/pakfiles.dat', new Blob([])]]);
    const converted = [{path: 'audio/sfx/feet.dsfx', file: new Blob([])}];
    const manifest = {sections: {audio_sfx: {}, audio_streams: {}}};
    let terminated = false;
    const factory = () => ({
        postMessage(message) {
            assert.equal(message.files, original);
            assert.equal(message.converted, converted);
            assert.equal(message.manifest, manifest);
            queueMicrotask(() => this.onmessage({data: {
                generated: [{path: 'audio/sfx/banks.dsbl',
                             file: new Blob(['DSBL'])}],
                banks: 2, tracks: 1
            }}));
        },
        terminate() {terminated = true;}
    });
    const result = await convertOriginalSound(original, converted,
        manifest, () => {}, factory);
    assert.equal(result[0].path, 'audio/sfx/banks.dsbl');
    assert.equal(terminated, true);
});
