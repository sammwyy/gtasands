import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import {existsSync} from 'node:fs';
import {mkdtemp, readFile, writeFile} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';
import test from 'node:test';
import {audioLookup, audioNames} from '../../web/sound_assets.mjs';
import {convertStreamNative} from '../../web/streams.mjs';

const root = resolve(import.meta.dirname, '../..');
const wasm = existsSync(join(root, 'web/assets.mjs'));
const checksum = bytes => createHash('sha256').update(bytes).digest('hex');

function encodeSound(module, samples, {
    rate = 11025, targetRate = 11025, format = 'adpcm', seekable = false,
    volume = 1, maxSeconds = 0, loop = -1, headroom = 0
} = {}) {
    const pcm = module._malloc(Math.max(samples.byteLength, 1));
    const pointers = module._malloc(8);
    let output = 0;
    try {
        assert.ok(pcm && pointers);
        module.HEAP16.set(samples, pcm >>> 1);
        assert.equal(module._dsnd_encode_simple(pcm, samples.length,
            rate, targetRate, {pcm8: 0, pcm16: 1, adpcm: 2}[format],
            seekable ? 1 : 0, volume, maxSeconds, loop, headroom,
            pointers, pointers + 4), 1);
        output = module.HEAPU32[pointers >>> 2];
        return Buffer.from(module.HEAPU8.slice(output,
            output + module.HEAPU32[(pointers + 4) >>> 2]));
    } finally {
        if (output) module._free(output);
        if (pcm) module._free(pcm);
        if (pointers) module._free(pointers);
    }
}

function convertBank(module, raw) {
    const input = module._malloc(raw.length);
    const options = module._malloc(40);
    const pointers = module._malloc(12);
    let output = 0;
    try {
        assert.ok(input && options && pointers);
        module.HEAPU8.set(raw, input);
        module.HEAPU8.fill(0, options, options + 40);
        const view = new DataView(module.HEAPU8.buffer);
        view.setUint32(options + 4, 11025, true);
        view.setUint8(options + 8, 2);
        view.setFloat64(options + 16, 1, true);
        view.setInt32(options + 32, -1, true);
        if (!module._dsbk_convert(input, raw.length, options,
            pointers, pointers + 4, pointers + 8)) return null;
        output = module.HEAPU32[pointers >>> 2];
        return Buffer.from(module.HEAPU8.slice(output,
            output + module.HEAPU32[(pointers + 4) >>> 2]));
    } finally {
        if (output) module._free(output);
        if (input) module._free(input);
        if (options) module._free(options);
        if (pointers) module._free(pointers);
    }
}

test('flat WASM sound entry matches frozen Python PCM/ADPCM output', {
    skip: !wasm
}, async () => {
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const samples = Int16Array.from({length: 32000}, (_, i) =>
        i * 109 % 30000 - 15000);
    const pcm = module._malloc(samples.byteLength);
    const pointers = module._malloc(8);
    let output = 0;
    try {
        module.HEAP16.set(samples, pcm >>> 1);
        assert.equal(module._dsnd_encode_simple(pcm, samples.length,
            32000, 11025, 2, 1, 0.8, 0, -1, 3,
            pointers, pointers + 4), 1);
        output = module.HEAPU32[pointers >>> 2];
        const length = module.HEAPU32[(pointers + 4) >>> 2];
        const bytes = module.HEAPU8.slice(output, output + length);
        assert.equal(length, 5552);
        assert.equal(checksum(bytes),
            '871f5afb397b86a9bbdd6827292dc95c01244aeebcd925bc0ad4abf7774072b8');
    } finally {
        if (output) module._free(output);
        module._free(pcm);
        module._free(pointers);
    }
});

test('C/WASM SFX banks and lookup match Python pack layouts', {
    skip: !wasm
}, async t => {
    const build = spawnSync('make', ['-f', 'tools/portable/Makefile',
        'build/audio_pack_cli'], {cwd: root, encoding: 'utf8'});
    if (build.status !== 0) return t.skip('C compiler unavailable');
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const items = [[19, Buffer.from('abc')], [1, Buffer.alloc(4099, 120)],
        [7, Buffer.alloc(0)]];
    const sorted = [...items].sort(([a], [b]) => a - b);
    const names = ['GENRL', 'SCRIPT', 'ñame'];
    const lookup = [[255, 0], [0, 1], [2, 65535]];
    const allocated = [];
    const allocate = bytes => {
        const pointer = module._malloc(Math.max(bytes, 1));
        assert.ok(pointer);
        allocated.push(pointer);
        return pointer;
    };
    const records = allocate(items.length * 12);
    const result = allocate(8);
    let output = 0;
    try {
        for (const [i, [bank, bytes]] of sorted.entries()) {
            const pointer = allocate(bytes.length);
            module.HEAPU8.set(bytes, pointer);
            const view = new DataView(module.HEAPU8.buffer);
            view.setUint16(records + i * 12, bank, true);
            view.setUint32(records + i * 12 + 4, pointer, true);
            view.setUint32(records + i * 12 + 8, bytes.length, true);
        }
        assert.equal(module._dsfx_pack(records, items.length,
            result, result + 4), 1);
        output = module.HEAPU32[result >>> 2];
        const length = module.HEAPU32[(result + 4) >>> 2];
        const packed = Buffer.from(module.HEAPU8.slice(output, output + length));
        assert.equal(length, 4146);
        assert.equal(checksum(packed),
            '61af078b2e574937bd11aa174373fbaac509f1b12207f9e6f6ac930e876ae19b');
        const work = await mkdtemp(join(tmpdir(), 'gtasads-audio-pack-test-'));
        const argumentsList = [join(work, 'test.dsfx')];
        for (const [bank, bytes] of items) {
            const path = join(work, `${bank}.dsbk`);
            await writeFile(path, bytes);
            argumentsList.push(String(bank), path);
        }
        const run = spawnSync(join(root, 'build/audio_pack_cli'), argumentsList,
            {encoding: 'utf8'});
        assert.equal(run.status, 0, run.stderr);
        assert.deepEqual(await readFile(argumentsList[0]), packed);
        module._free(output);
        output = 0;
        const pointers = allocate(names.length * 4);
        for (const [i, name] of names.entries()) {
            const bytes = Uint8Array.from(name,
                char => char.codePointAt(0) <= 255 ? char.codePointAt(0) : 63);
            const pointer = allocate(bytes.length + 1);
            module.HEAPU8.set(bytes, pointer);
            module.HEAPU8[pointer + bytes.length] = 0;
            module.HEAPU32[(pointers >>> 2) + i] = pointer;
        }
        const entries = allocate(lookup.length * 4);
        module.HEAPU8.fill(0, entries, entries + lookup.length * 4);
        for (const [i, [pack, index]] of lookup.entries()) {
            const view = new DataView(module.HEAPU8.buffer);
            view.setUint8(entries + i * 4, pack);
            view.setUint16(entries + i * 4 + 2, index, true);
        }
        assert.equal(module._dsbl_pack(pointers, names.length, entries,
            lookup.length, result, result + 4), 1);
        output = module.HEAPU32[result >>> 2];
        const lookupBytes = module.HEAPU8.slice(output,
            output + module.HEAPU32[(result + 4) >>> 2]);
        assert.equal(lookupBytes.length, 72);
        assert.equal(checksum(lookupBytes),
            '46fb99b4a50ca06134f3868c44fcb65e65d2b388db8ed1116ef61a3296ce8233');
    } finally {
        if (output) module._free(output);
        for (const address of allocated) module._free(address);
    }
});

function decodeOgg(module, ogg) {
    const source = module._malloc(ogg.length);
    const info = module._malloc(24);
    const result = module._malloc(16);
    let output = 0;
    try {
        module.HEAPU8.set(ogg, source);
        assert.equal(module._ogg_read_stream_info(
            source, ogg.length, info, info + 4, info + 8), 1);
        const metadata = Array.from(module.HEAPU32.subarray(info >>> 2,
                                                            (info + 12) >>> 2));
        assert.equal(module._ogg_read_stream_info(
            source, Math.min(20, ogg.length), info, info + 4, info + 8), 0);
        assert.equal(module._ogg_decode_vorbis(
            source, Math.min(20, ogg.length),
            result, result + 4, result + 8, result + 12), 0);
        assert.equal(module._ogg_decode_vorbis(
            source, ogg.length, result, result + 4,
            result + 8, result + 12), 1);
        output = module.HEAPU32[result >>> 2];
        const samples = module.HEAPU32[(result + 4) >>> 2];
        const rate = module.HEAPU32[(result + 8) >>> 2];
        const channels = new DataView(module.HEAPU8.buffer)
            .getUint16(result + 12, true);
        assert.equal(rate, metadata[0]);
        assert.equal(channels, metadata[2]);
        assert.equal(samples, metadata[1]);
        return {pcm: Buffer.from(module.HEAPU8.slice(output,
            output + samples * channels * 2)), rate, channels, samples};
    } finally {
        if (output) module._free(output);
        module._free(result);
        module._free(info);
        module._free(source);
    }
}

function comparePCM(actual, expected) {
    assert.equal(actual.length, expected.length);
    let maximum = 0;
    for (let i = 0; i < actual.length; i += 2)
        maximum = Math.max(maximum,
            Math.abs(actual.readInt16LE(i) - expected.readInt16LE(i)));
    assert.ok(maximum <= 1, `PCM difference ${maximum}`);
}

test('OGG metadata, truncation and native decoding agree with ffmpeg', {
    skip: !wasm || spawnSync('ffmpeg', ['-version']).status !== 0
}, async t => {
    const built = spawnSync('make', ['-f', 'tools/portable/Makefile',
        'build/ogg_cli'], {cwd: root, encoding: 'utf8'});
    if (built.status !== 0) return t.skip('C compiler unavailable');
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const work = await mkdtemp(join(tmpdir(), 'gtasads-ogg-test-'));
    const source = join(work, 'synthetic.ogg');
    const encode = spawnSync('ffmpeg', ['-nostdin', '-loglevel', 'error',
        '-f', 'lavfi', '-i',
        'sine=frequency=440:duration=1:sample_rate=22050',
        '-c:a', 'libvorbis', source], {encoding: 'utf8'});
    assert.equal(encode.status, 0, encode.stderr);
    const ogg = await readFile(source);
    const {pcm, rate, channels, samples} = decodeOgg(module, ogg);
    assert.deepEqual([rate, channels], [22050, 1]);
    assert.ok(samples > 0);
    const output = join(work, 'decoded.pcm');
    const run = spawnSync(join(root, 'build/ogg_cli'), [source, output],
        {encoding: 'utf8'});
    assert.equal(run.status, 0, run.stderr);
    const native = await readFile(output);
    comparePCM(pcm, native);
    const reference = spawnSync('ffmpeg', ['-nostdin', '-loglevel', 'error',
        '-i', source, '-f', 's16le', '-ac', '1', '-ar', '22050', 'pipe:1']);
    assert.equal(reference.status, 0, reference.stderr?.toString());
    comparePCM(native, reference.stdout);
});

test('real decrypted Vorbis track decodes at its original rate', {
    skip: !wasm || !existsSync(join(root, 'game/audio/CONFIG/TrakLkup.dat')) ||
        spawnSync('ffmpeg', ['-version']).status !== 0
}, async () => {
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const table = await readFile(join(root, 'game/audio/CONFIG/TrakLkup.dat'));
    const names = await readFile(join(root, 'game/audio/CONFIG/StrmPaks.dat'));
    const pack = table[0];
    const name = names.subarray(pack * 16, pack * 16 + 16).toString('latin1')
        .split('\0')[0];
    const source = join(root, 'game/audio/streams', name);
    const raw = await readFile(source);
    const offset = table.readUInt32LE(4), size = table.readUInt32LE(8);
    const encrypted = raw.subarray(offset, offset + size + 0x1f84);
    const input = module._malloc(encrypted.length);
    try {
        module.HEAPU8.set(encrypted, input);
        module._audio_decrypt_parts(input, encrypted.length, offset >>> 0,
                                    Math.floor(offset / 2 ** 32), input);
        const ogg = module.HEAPU8.slice(input + 0x1f84,
            input + encrypted.length);
        const {pcm, rate, channels, samples} = decodeOgg(module, ogg);
        assert.equal(pcm.length, samples * channels * 2);
        const reference = spawnSync('ffmpeg', ['-nostdin', '-loglevel', 'error',
            '-i', 'pipe:0', '-f', 's16le', '-ac', String(channels),
            '-ar', String(rate), 'pipe:1'], {
            input: ogg, maxBuffer: 64 * 1048576
        });
        assert.equal(reference.status, 0, reference.stderr?.toString());
        comparePCM(pcm, reference.stdout);
    } finally {
        module._free(input);
    }
});

test('real encrypted audio stream matches Python header and native CLI bytes', {
    skip: !wasm || !existsSync(join(root, 'game/audio/CONFIG/TrakLkup.dat'))
}, async t => {
    const built = spawnSync('make', ['-f', 'tools/portable/Makefile',
        'build/audio_stream_cli'], {cwd: root, encoding: 'utf8'});
    if (built.status !== 0) return t.skip('C compiler unavailable');
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const table = await readFile(join(root, 'game/audio/CONFIG/TrakLkup.dat'));
    const pack = table[0];
    const names = await readFile(join(root, 'game/audio/CONFIG/StrmPaks.dat'));
    const name = names.subarray(pack * 16, pack * 16 + 16)
        .toString('latin1').split('\0')[0];
    const offset = table.readUInt32LE(4), size = table.readUInt32LE(8);
    const original = await readFile(join(root, 'game/audio/streams', name));
    const encrypted = original.subarray(offset, offset + size + 0x1f84);
    const wasmSound = Buffer.from(convertStreamNative(module, encrypted, offset,
        {sample_rate: 11025, format: 'adpcm', volume: 1, max_seconds: 0}));
    const work = await mkdtemp(join(tmpdir(), 'gtasads-stream-cli-test-'));
    const input = join(work, 'track.enc'), output = join(work, 'track.dsnd');
    await writeFile(input, encrypted);
    const run = spawnSync(join(root, 'build/audio_stream_cli'),
        [input, output, String(offset)], {encoding: 'utf8'});
    assert.equal(run.status, 0, run.stderr);
    const native = await readFile(output);
    assert.deepEqual(wasmSound.subarray(0, 24), native.subarray(0, 24));
    assert.equal(wasmSound.length, native.length);
    if (checksum(encrypted) ===
            '2e56288df0e8584dd4c4a82a2315201aec4c163b9bfff8b044ea3f3f70c58083')
        assert.equal(checksum(native),
            '5b4548319e0d405cf20e372ac929d414e9059162f296dad19f3406312c9110eb');
});

test('audio decrypt, lookup tables and seek-aligned track offsets match Python', {
    skip: !wasm
}, async () => {
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const key = [0xEA, 0x3A, 0xC4, 0xA1, 0x9A, 0xA8, 0x14, 0xF3,
        0x48, 0xB0, 0xD7, 0x23, 0x9D, 0xE8, 0xFF, 0xF1];
    const pointers = [];
    const allocate = size => {
        const address = module._malloc(Math.max(size, 1));
        assert.ok(address);
        pointers.push(address);
        return address;
    };
    try {
        const input = allocate(256), output = allocate(256);
        module.HEAPU8.set(Uint8Array.from({length: 256}, (_, i) => i), input);
        for (const position of [0, 7, 8191]) {
            module._audio_decrypt_parts(input, 256, position, 0, output);
            assert.deepEqual(module.HEAPU8.slice(output, output + 256),
                Uint8Array.from({length: 256},
                    (_, i) => i ^ key[(position + i) & 15]));
        }
        const config = join(root, 'game/audio/CONFIG');
        if (!existsSync(join(config, 'TrakLkup.dat'))) return;
        for (const [file, width] of [['PakFiles.dat', 52],
            ['StrmPaks.dat', 16]]) {
            const bytes = await readFile(join(config, file));
            const source = allocate(bytes.length), result = allocate(8);
            module.HEAPU8.set(bytes, source);
            assert.equal(module._audio_table_names(
                source, bytes.length, width, result, result + 4), 1);
            const entries = module.HEAPU32[result >>> 2];
            assert.deepEqual(Array.from({length: module.HEAPU32[(result + 4) >>> 2]},
                (_, i) => Buffer.from(module.HEAPU8.slice(
                    entries + i * 53, entries + i * 53 + 53))
                    .toString('latin1').split('\0')[0]), audioNames(bytes, width));
            if (entries) module._free(entries);
        }
        for (const file of ['BankLkup.dat', 'TrakLkup.dat']) {
            const bytes = await readFile(join(config, file));
            const source = allocate(bytes.length), result = allocate(8);
            module.HEAPU8.set(bytes, source);
            assert.equal(module._audio_table_entries(
                source, bytes.length, result, result + 4), 1);
            const entries = module.HEAPU32[result >>> 2];
            const actual = Array.from({length: module.HEAPU32[(result + 4) >>> 2]},
                (_, i) => ({
                    pack: module.HEAPU32[(entries + i * 12) >>> 2],
                    offset: module.HEAPU32[(entries + i * 12 + 4) >>> 2],
                    size: module.HEAPU32[(entries + i * 12 + 8) >>> 2]
                }));
            assert.deepEqual(actual, audioLookup(bytes));
            if (entries) module._free(entries);
        }
        const lookup = await readFile(join(config, 'TrakLkup.dat'));
        const names = audioNames(await readFile(join(config, 'StrmPaks.dat')), 16);
        let compared = 0;
        for (const {pack, offset} of audioLookup(lookup)) {
            if (!names[pack] || !existsSync(join(root, 'game/audio/streams', names[pack])))
                continue;
            const path = join(root, 'game/audio/streams', names[pack]);
            const source = await readFile(path);
            const start = Math.max(0, offset - 32768);
            const raw = source.subarray(start, offset + 32768 + 0x1f84 + 4);
            const data = allocate(raw.length), found = allocate(8);
            module.HEAPU8.set(raw, data);
            if (!module._audio_aligned_offset(
                data, raw.length, BigInt(start), BigInt(offset), found))
                continue;
            const aligned = Number(new DataView(module.HEAPU8.buffer)
                .getBigUint64(found, true));
            assert.ok(Math.abs(aligned - offset) <= 32768);
            if (names[pack] === 'AA' && compared < 5 &&
                checksum(lookup) ===
                    'adf22fcdfe798c7ad59f1b8fbf06f1ca39a074ad2b3163c341c0d9' &&
                checksum(source) ===
                    'e8e854b3155d20ab8e71920618f53ba9f6d62251bf48766ead123b3d876dbfd4')
                assert.equal(aligned, [0, 94112, 205931, 281133, 362498][compared]);
            if (++compared === 5) break;
        }
        assert.equal(compared, 5);
    } finally {
        for (const address of pointers) module._free(address);
    }
});

test('native DSST seekable and plain packs match Python stream ordering', {
    skip: !wasm
}, async () => {
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const pcm = Int16Array.from({length: 1000}, (_, i) => i * 51 - 25500);
    for (const seekable of [false, true]) {
        const sounds = [[12, 750], [2, 100], [12, 100]]
            .map(([track, n]) => [track, encodeSound(module,
                pcm.subarray(0, n), {seekable})]);
        const pointers = [];
        const allocate = size => {
            const address = module._malloc(size);
            assert.ok(address);
            pointers.push(address);
            return address;
        };
        const entries = allocate(sounds.length * 12);
        const result = allocate(8);
        let output = 0;
        try {
            for (const [i, [track, bytes]] of sounds.entries()) {
                const data = allocate(bytes.length);
                module.HEAPU8.set(bytes, data);
                const view = new DataView(module.HEAPU8.buffer);
                view.setUint32(entries + i * 12, track, true);
                view.setUint32(entries + i * 12 + 4, data, true);
                view.setUint32(entries + i * 12 + 8, bytes.length, true);
            }
            assert.equal(module._dsst_pack(entries, sounds.length,
                seekable ? 1 : 0, result, result + 4), 1);
            output = module.HEAPU32[result >>> 2];
            const packed = module.HEAPU8.slice(output,
                output + module.HEAPU32[(result + 4) >>> 2]);
            assert.equal(packed.length, seekable ? 644 : 608);
            assert.equal(checksum(packed), seekable ?
                '6295535552bb63cdc167bf066f25e5f9d78b747b42ece020aa43852f581e11ac' :
                '3f9497d7fb4ebf5e63bf572c99a46acce85b180a8ee499f8a7120c28129f1ea5');
        } finally {
            if (output) module._free(output);
            for (const address of pointers) module._free(address);
        }
    }
});

test('DSND formats, resampling, volume, limits and checkpoint prefixes match Python', {
    skip: !wasm
}, async () => {
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const samples = Int16Array.from({length: 8195}, (_, i) =>
        i * 1471 % 60000 - 30000);
    const golden = [
        ['pcm8', 22050, 11025, 4124, '9c6ecf00e5a3f3a4c43af5418bd58940026c6aab8a864d53ca283fc7e87b7faa',
            'a7adac6b10a27cdcf1f42cd71a2f6071d42e63406e800593211e01521961b092'],
        ['pcm8', 8000, 4000, 2024, 'b9af9c33626ddb2b3592ab6085fdf7603428d8906b3784d47166ad8698954868',
            '4b6fbc6f0fd5adb88b7ada61fc337f5cd52b34ecbc3f498dcc7202b1a5441929'],
        ['pcm8', 11025, 22050, 5536, '94ad8eca7df89be882aa6b8508971a2be59447c96dd3d2ddb343be97da5e5099',
            '560753c5263e8798d052f4face46b4178e08883294fa9387408c50c1ecefb73c'],
        ['pcm16', 22050, 11025, 8220, 'd97b3e65badd6d785610739999098f642a6ab8f05ed9719d41029dca9ae8de26',
            '7e2e122b444b510994ee18d7f581849c53366391a108348d5452ed2b5d3762f3'],
        ['pcm16', 8000, 4000, 4024, '071b4a5f04907718589693edcec9ae2764c91bd5992bb6c75536c3518f8f6808',
            '2faff742c1ef62637744b9cf842d41f5f6cff9041636a9b88dd103c84fbc43b7'],
        ['pcm16', 11025, 22050, 11048, '0b5124c3346be966a10785f907a2e9c99270caab797c16d4d94bf38120583f42',
            'f73d24b0395cc9434cf79c4654b46820c158eaab8b350ae0663ea100da703911'],
        ['adpcm', 22050, 11025, 2080, '3def683f689e2c86a1fa74c64d20f3fccf0e111419857d90a69d3c1f136a7e7c',
            '609595a94afd0072cc39de2e265b07f2a7ed5b91f29f0df6c6955d3e09fbf6a6'],
        ['adpcm', 8000, 4000, 1028, 'c8f7f70f4d0318259d2f15787b2c0740aed8620285bb02a8b205da24a371af2b',
            'b0fde5d4d9937e46459b25d7c590ba45981b4d95f15f1b1c53c8372fc6ad274b'],
        ['adpcm', 11025, 22050, 2784, '31be2d1c85db181a35bbbf972a0fcec02bf92778fc5605430cdea87eba56b8b2',
            '0220c5549c06238cd8fc8f3601ca70ae232ae3295c4b24be3eed9fcaebb19940']
    ];
    for (const [format, rate, targetRate, length, ordinary, seekable] of golden) {
        for (const enabled of [false, true]) {
            const actual = encodeSound(module, samples, {rate, targetRate,
                format, seekable: enabled, volume: 0.8, maxSeconds: 0.5,
                loop: 3, headroom: 5});
            const split = 24 + (actual.readUInt32LE(20) + 3 & ~3);
            const prefix = enabled ? Buffer.from([1, 2, 3, 4]) : Buffer.alloc(0);
            const bytes = Buffer.concat([actual.subarray(0, split), prefix,
                actual.subarray(split)]);
            assert.equal(bytes.length, length + (enabled ?
                format === 'adpcm' ? 8 : 4 : 0));
            assert.equal(checksum(bytes), enabled ? seekable : ordinary,
                `${format} ${rate} -> ${targetRate} seek=${enabled}`);
        }
    }
});

test('twenty real sound banks match frozen Python outputs and native CLI', {
    skip: !wasm || !existsSync(join(root, 'game/audio/CONFIG/BankLkup.dat'))
}, async t => {
    const built = spawnSync('make', ['-f', 'tools/portable/Makefile',
        'build/audio_cli'], {cwd: root, encoding: 'utf8'});
    if (built.status !== 0) return t.skip('C compiler unavailable');
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const config = join(root, 'game/audio/CONFIG');
    const names = audioNames(await readFile(join(config, 'PakFiles.dat')), 52);
    const entries = audioLookup(await readFile(join(config, 'BankLkup.dat')));
    const known = new Map([
        [0, ['377797e101133c0e271b04908842b53b2acf83d637568a8f1a06e14fcf3b273b', 'de0bb9f0d59b414b209d8719731eb9137bfeb07171a8e4f2eb27b857dd3341d2']],
        [1, ['d23c1eedd7b8826632c1bf8a395cea3bb065d6a28955eec46d14b6f189ca5d92', '6933e89d71e4f8d8998ddc5f1fba1e7427e70a44d51a545bc7ce40dcb9471c2f']],
        [2, ['b482c038b39b1fa8d598e6ae3b6f7059234593002247c44497680b29822440ba', 'a84c2533810907161047a2da2d435cff996c48beed4157266fe4d77071ffb4ff']],
        [3, ['09c6ed4ffadc0c9570f7e79e72530a9facc0887e49021bafa8709681fcb354c9', '19513e1009d8b56deadf8ca2e5ff6b43d0e06f77f1c1dc6e99c3f3c0fae3089a']],
        [4, ['8e127fe045b2d4482b5282b04dbc64b83eefec4d3c796cf725b8d0e366abd661', '4e79ab2fcf5855b6b67af4a7d87afcf2f2518154b14145e61e1a2d3a24348558']],
        [5, ['42437498dffb7bc03e717b3c3a578d7a0ed44f7444375d2a53b0c3984aba943f', '022b971caa21527372bdcb3049b149de8f907d9879678af9bab266b5d7a6460b']],
        [6, ['133db48eeb4c90f06256fc229073ed9b23ecf136ebb33765a7e089276673792a', 'e5d329611bd9eb554f6b23d1f29251fcd5ad4d193d55647c01a974fe29529bea']],
        [7, ['d1932e5d106805474adae21a25e63d845622cac605865f4f4820d8d6f5ef3c75', '8343c0b9568669d12b610b75aa8ac4b559e9ab927559ac1105d54b66e3012039']],
        [9, ['fc60bc323c79b8ccaa63798087cf1f1c0e5e24fba27dc193a210574f08755082', 'f194acc388e7efe2505117380f7ce34308977b5afa4338a8eac463c87a465e68']],
        [10, ['6263f0f0c9e5d6529f4a27bfa49695a911e0ea471dcbd5c072b1a5e2a6cf7d0a', '697525cab065a84b813ad40ba19e10a6556cc50688d1452692073b3977d55af6']],
        [11, ['3c3b2f274400bbed2a0f8a2addbe54636873ce29160044e5e18c55ebed004ef6', 'f9b4bdf5f6b9cde7ff26319b819399857344af3005cd477c6da27c66c1877c52']],
        [13, ['a8da12bde62959edffe467d71342cb99ce8ee9ba6fe8d3f0a3d76f7756e5e7b0', 'df5fee8e740f2fe586002b30727ee2d27c4ab1055938c07f0583779b68e8067f']],
        [14, ['e1f3aaf4161762d31848f4984b1d0e910703522038cf9125ec5e5f6297c99dfd', '53ea40df6bea900340a2c33d6cf32554c40d93d44003fd0ddb3d340b8558e7de']],
        [16, ['cfa344368926c4800f66af907c7bfcf60c5a7cf5904bf228db9c9b167a76a2bc', 'b1ff90a7ea6ec560f3c965db16d258cc3c436ba55888933626a13f09f9ec856a']],
        [18, ['93f8e393033b4e84b419e91b3e6300c41c3832e061f8f698984cf17a04480d10', 'a4577577daa26bdbe95b703df307805c4eab1ecfd88096940b909d478bd4fb22']],
        [19, ['2f6d54176fc4ca58407f1b54744ab9cbd79cea9682ee12433d2671f0ef1a5ab2', 'b486ac54ece52addd53000194a6612f71aa57ef83bdcd0c657d7d4034a6ce1ba']],
        [20, ['018f274c33feb35ba41a8e6530078cb706da3953657da091634c7c8baa669f92', '9f36739073eaf3d41c6894afcff2a739a03f79fe05eadc1bbafe2369087f0d53']],
        [21, ['9a59d75c699f155a2d921e80e1b01bd120cf3be4d2e195b2f996fa8d4d8e1baf', '01bea2e05e8828aa8ebfd3ca4992b36ff32dc4d599f3b1e650f3fd6770818e1f']],
        [22, ['d6a25116945a6c2a138f37ace89e790da7efa033b5d79963929471f01d089a85', '1d87337068e38fc90cede313c7a034df07591c7b3f4af33114329d50924a6dcb']],
        [23, ['56ea9901ed87cb50a3e5a5a6bb1a7ca156bcd451a1723baea0f2f43cac5f907f', '7b0967f4c81c62e3c24f3bf7ca356494b3ebc02699361f3cb3ccfb4e534da9d2']]
    ]);
    const work = await mkdtemp(join(tmpdir(), 'gtasads-real-banks-test-'));
    let compared = 0;
    for (const [index, {pack, offset, size}] of entries.entries()) {
        if (size < 0x12c4 || size > 150000 || !names[pack]) continue;
        const path = join(root, 'game/audio/sfx', names[pack]);
        if (!existsSync(path)) continue;
        const raw = (await readFile(path)).subarray(offset, offset + size);
        if (raw.length !== size) continue;
        const converted = convertBank(module, raw);
        if (!converted) continue;
        const input = join(work, `${index}.bank`);
        const output = join(work, `${index}.dsbk`);
        await writeFile(input, raw);
        const run = spawnSync(join(root, 'build/audio_cli'),
            ['bank', input, output], {encoding: 'utf8'});
        assert.equal(run.status, 0, run.stderr);
        const native = await readFile(output);
        assert.equal(checksum(native), checksum(converted), `bank ${index}`);
        if (known.has(index) && checksum(raw) === known.get(index)[0])
            assert.equal(checksum(native), known.get(index)[1], `bank ${index}`);
        if (++compared === 20) break;
    }
    assert.equal(compared, 20);
});

test('audio CLI matches Python when limiting and downsampling PCM sound', {
    skip: !wasm
}, async t => {
    const built = spawnSync('make', ['-f', 'tools/portable/Makefile',
        'build/audio_cli'], {cwd: root, encoding: 'utf8'});
    if (built.status !== 0) return t.skip('C compiler unavailable');
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const samples = Int16Array.from({length: 8195}, (_, i) =>
        i * 761 % 60000 - 30000);
    const expected = '7c08fcdaae609d269a7a01234474177acb868a7f8d6c2f54fc88b1cebb5d064c';
    const wasmSound = encodeSound(module, samples, {rate: 22050,
        targetRate: 11025, volume: 0.8, maxSeconds: 0.25, loop: 3});
    assert.equal(wasmSound.length, 1408);
    assert.equal(checksum(wasmSound), expected);
    const work = await mkdtemp(join(tmpdir(), 'gtasads-audio-cli-test-'));
    const input = join(work, 'sound.pcm'), output = join(work, 'sound.dsnd');
    await writeFile(input, Buffer.from(samples.buffer));
    const run = spawnSync(join(root, 'build/audio_cli'),
        ['sound', input, output, '22050', '11025', 'adpcm', '0.8',
            '0.25', '3'], {encoding: 'utf8'});
    assert.equal(run.status, 0, run.stderr);
    assert.equal(checksum(await readFile(output)), expected);
});

test('native ADPCM CLI and WASM match Python bitstream and seek checkpoints', {
    skip: !wasm
}, async t => {
    const built = spawnSync('make', ['-f', 'tools/portable/Makefile',
        'build/adpcm_cli'], {cwd: root, encoding: 'utf8'});
    if (built.status !== 0) return t.skip('C compiler unavailable');
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const samples = Int16Array.from({length: 25000}, (_, i) =>
        i * 173 % 50000 - 25000);
    const work = await mkdtemp(join(tmpdir(), 'gtasads-adpcm-cli-test-'));
    const input = join(work, 'samples.pcm');
    const output = join(work, 'samples.adpcm');
    const seek = join(work, 'seek.bin');
    await writeFile(input, Buffer.from(samples.buffer));
    const run = spawnSync(join(root, 'build/adpcm_cli'),
        [input, output, seek], {encoding: 'utf8'});
    assert.equal(run.status, 0, run.stderr);
    const raw = await readFile(output), checkpoints = await readFile(seek);
    assert.equal(raw.length, 12504);
    assert.equal(checksum(raw),
        'fe384b73ddbe678f013344cc8d37a3fbfa460bb2ba003236da85b2b07cfebffc');
    assert.equal(checkpoints.length, 16);
    assert.equal(checksum(checkpoints),
        'f08f22320037fa21d017d1c74cdb1410d6e068271b8fe88e6284b2a6de266fe7');
    const sound = encodeSound(module, samples, {seekable: true});
    const dataSize = sound.readUInt32LE(20);
    assert.equal(checksum(sound.subarray(24, 24 + dataSize)), checksum(raw));
    assert.equal(checksum(sound.subarray(24 + (dataSize + 3 & ~3))),
        checksum(checkpoints));
});

test('ADPCM length and checkpoint boundaries match Python for empty and long clips', {
    skip: !wasm
}, async () => {
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const golden = [
        [0, '6973317040b5be5a2109d0558a11730a8029af169ec00323fd9caaf729e005d2',
            '6973317040b5be5a2109d0558a11730a8029af169ec00323fd9caaf729e005d2'],
        [1, 'c54ba2227ad979da843f07c77c284adea6d7f56389c65a66dad976d99709e46f',
            'c2031b4b4521a13847e4e04445af5d6e49656912bc8ac8d46328d35744a16c3b'],
        [2, '4aa3cdd7c28ed6c2647264e81069c13c9352cd75955e8cfcd33899d704798b26',
            'dd8a37ae08bcb50fca90f098976c671b1b2db5b4c91c6d7aedb56a04b12ca9a9'],
        [19, '36c9947e8b8c54b8f3c814209882d2e7c8845500e40664fc949d0e79272fdfb0',
            '5f8e2a2b4c15a474394b4e04f878491f838b29e44a05fcdc81bbff449d70aa47'],
        [8192, '54d17d756c9ef3c193293a25151a5378f1fc5e4b8a01276a9ff4c4acbc062e4c',
            'ab12df6b6c840f6e6139de5af0286ffd8bbde690ba563f29c849e5a6aeaa9e09'],
        [8193, '4ef564d8040fe327f845520cd5fe36af0662b512636120ab08568d1794e5eb27',
            '42aaa0c241a27b4e3d5307d9062cb4c7b06e28505f565ef15cf6fcecd1b73a71'],
        [25000, 'cbe79fb4cdcabd7e136ee892f4937eadf716665c17f350d38acf7501992e903d',
            'dc9d01b058a13a7fcc6aeba834d75fdcbe57e1eaba560203c56903e875ef3d95']
    ];
    for (const [length, plain, seekable] of golden) {
        const samples = Int16Array.from({length}, (_, i) =>
            i * 173 % 50000 - 25000);
        assert.equal(checksum(encodeSound(module, samples)), plain,
            `length ${length}`);
        assert.equal(checksum(encodeSound(module, samples, {seekable: true})),
            seekable, `seekable length ${length}`);
    }
});
