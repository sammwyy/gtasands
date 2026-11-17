import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {existsSync} from 'node:fs';
import {copyFile, mkdir, mkdtemp, readFile, stat, utimes, writeFile} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';
import test from 'node:test';
import {build} from '../assets.mjs';
import {convertMovie} from '../../web/movies.mjs';

const root = resolve(import.meta.dirname, '../..');
const wasm = existsSync(join(root, 'web/assets.mjs'));

function expectedVideo(palette, frames, audio, fps, rate) {
    const pixels = 256 * 256;
    const stride = frames.length % pixels && !(frames.length % (pixels + 1024)) ?
        pixels + 1024 : pixels;
    const count = Math.trunc(frames.length / stride);
    const header = Buffer.alloc(536);
    header.write('DVID');
    header.writeUInt16LE(1, 4);
    header.writeUInt16LE(256, 6);
    header.writeUInt16LE(256, 8);
    header.writeUInt16LE(fps, 10);
    header.writeUInt32LE(count, 16);
    header.writeUInt32LE(rate, 20);
    for (let i = 0; i < 256; i++) {
        const colour = (palette[3 * i] >> 3) |
            ((palette[3 * i + 1] >> 3) << 5) |
            ((palette[3 * i + 2] >> 3) << 10);
        header.writeUInt16LE(colour, 24 + i * 2);
    }
    const parts = [header];
    let consumed = 0;
    for (let i = 0; i < count; i++) {
        const next = Math.min(audio.length,
            Math.trunc((i + 1) * (rate * 2.0 / fps)));
        const chunk = audio.subarray(consumed, next);
        const padded = chunk.length + chunk.length % 2;
        const size = Buffer.alloc(4);
        size.writeUInt32LE(padded);
        parts.push(size, chunk);
        if (padded !== chunk.length) parts.push(Buffer.alloc(1));
        parts.push(frames.subarray(i * stride, i * stride + pixels));
        consumed = next;
    }
    if (consumed < audio.length) {
        const size = Buffer.alloc(4);
        size.writeUInt32LE(audio.length - consumed);
        parts.push(size, audio.subarray(consumed));
    }
    parts.push(Buffer.alloc(4));
    return Buffer.concat(parts);
}

function packWasm(module, palette, frames, audio, fps, rate) {
    const data = [palette, frames, audio].map(bytes => {
        const pointer = module._malloc(Math.max(bytes.length, 1));
        module.HEAPU8.set(bytes, pointer);
        return pointer;
    });
    const result = module._malloc(12);
    let output = 0;
    try {
        assert.equal(module._dvid_pack(
            data[0], palette.length, data[1], frames.length,
            data[2], audio.length, fps, rate,
            result, result + 4, result + 8), 1);
        output = module.HEAPU32[result >>> 2];
        const size = module.HEAPU32[(result + 4) >>> 2];
        return [Buffer.from(module.HEAPU8.slice(output, output + size)),
            module.HEAPU32[(result + 8) >>> 2]];
    } finally {
        if (output) module._free(output);
        module._free(result);
        for (const address of data) module._free(address);
    }
}

test('synthetic DVID packs match reference layout in both native C and WASM', {
    skip: !wasm
}, async t => {
    const native = spawnSync('make', ['-f', 'tools/portable/Makefile',
        'build/dvid_cli'], {cwd: root, encoding: 'utf8'});
    if (native.status !== 0) return t.skip('C compiler unavailable');
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const work = await mkdtemp(join(tmpdir(), 'gtasads-dvid-test-'));
    const palette = Buffer.from({length: 768}, (_, i) => i & 255);
    const pixels = 256 * 256;
    const frames = Buffer.concat([Buffer.alloc(pixels, 0), Buffer.alloc(1024, 255),
        Buffer.alloc(pixels, 1), Buffer.alloc(1024, 254)]);
    const cases = [
        [frames, Buffer.from([1, 2, 3, 4, 5]), 7, 3],
        [frames.subarray(0, pixels), Buffer.from({length: 100},
            (_, i) => i), 10, 22050],
        [Buffer.alloc(0), Buffer.from([1, 2, 3]), 11, 32000]
    ];
    for (const [frameData, audio, fps, rate] of cases) {
        const prefix = `${frameData.length}-${audio.length}`;
        const files = [
            join(work, `palette-${prefix}.rgb`),
            join(work, `frames-${prefix}.pal`),
            join(work, `audio-${prefix}.pcm`)
        ];
        for (const [i, bytes] of [palette, frameData, audio].entries())
            await writeFile(files[i], bytes);
        const expected = expectedVideo(palette, frameData, audio, fps, rate);
        const [actual, count] = packWasm(module, palette, frameData, audio,
            fps, rate);
        assert.deepEqual(actual, expected);
        assert.equal(actual.readUInt32LE(16), count);
        const output = join(work, `video-${prefix}.dvid`);
        const run = spawnSync(join(root, 'build/dvid_cli'),
            [...files, output, String(fps), String(rate)], {encoding: 'utf8'});
        assert.equal(run.status, 0, run.stderr);
        assert.deepEqual(await readFile(output), expected);
    }
});

test('intro conversion cache preserves output bytes and timestamp', {
    skip: !wasm || spawnSync('ffmpeg', ['-version']).status !== 0
}, async () => {
    const work = await mkdtemp(join(tmpdir(), 'gtasads-intro-cache-test-'));
    const source = join(work, 'game'), output = join(work, 'output');
    await mkdir(join(source, 'movies'), {recursive: true});
    const movie = join(source, 'movies/Logo.mpg');
    const encode = spawnSync('ffmpeg', ['-nostdin', '-loglevel', 'error',
        '-f', 'lavfi', '-i', 'testsrc=size=64x48:rate=30:duration=1',
        '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=44100:duration=1',
        '-c:v', 'mpeg1video', '-q:v', '5', '-c:a', 'mp2', '-b:a', '128k',
        '-shortest', movie], {encoding: 'utf8'});
    assert.equal(encode.status, 0, encode.stderr);
    await copyFile(movie, join(source, 'movies/GTAtitles.mpg'));
    await build({source, output, sections: ['movies']});
    const target = join(output, 'movies/logo.dvid');
    const bytes = await readFile(target);
    assert.equal(bytes.subarray(0, 4).toString(), 'DVID');
    const old = new Date('2020-01-01T00:00:00Z');
    await utimes(target, old, old);
    await build({source, output, sections: ['movies']});
    assert.equal((await stat(target)).mtimeMs, old.getTime());
    assert.deepEqual(await readFile(target), bytes);
});

test('real logo video is a playable DVID and matches the native MPEG CLI', {
    skip: !wasm || !existsSync(join(root, 'game/movies/Logo.mpg'))
}, async t => {
    const built = spawnSync('make', ['-f', 'tools/portable/Makefile',
        'build/mpeg_movie_cli'], {cwd: root, encoding: 'utf8'});
    if (built.status !== 0) return t.skip('C compiler unavailable');
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const source = join(root, 'game/movies/Logo.mpg');
    const actual = convertMovie(module, await readFile(source));
    const blob = Buffer.from(actual.data);
    assert.equal(blob.subarray(0, 4).toString(), 'DVID');
    assert.deepEqual([blob.readUInt16LE(4), blob.readUInt16LE(6),
        blob.readUInt16LE(8), blob.readUInt16LE(10),
        blob.readUInt32LE(16), blob.readUInt32LE(20)],
    [1, 256, 256, 10, actual.frames, 22050]);
    assert.ok(blob.length > actual.frames * 256 * 256);
    assert.ok(blob.subarray(24, 536).filter(value => value !== 0).length > 100);
    const output = join(await mkdtemp(join(tmpdir(), 'gtasads-movie-cli-test-')),
        'logo.dvid');
    const run = spawnSync(join(root, 'build/mpeg_movie_cli'),
        [source, output], {encoding: 'utf8'});
    assert.equal(run.status, 0, run.stderr);
    assert.deepEqual(blob, await readFile(output));
    if (existsSync(join(root, 'nitrofs/movies/logo.dvid')))
        assert.ok(Math.abs(actual.frames -
            (await readFile(join(root, 'nitrofs/movies/logo.dvid')))
                .readUInt32LE(16)) <= 2);
});

test('real GTAtitles movie has complete DVID frame and audio records', {
    skip: !wasm || !existsSync(join(root, 'game/movies/GTAtitles.mpg'))
}, async () => {
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const {data, frames} = convertMovie(module,
        await readFile(join(root, 'game/movies/GTAtitles.mpg')));
    const blob = Buffer.from(data);
    assert.ok(frames > 100);
    assert.deepEqual([blob.subarray(0, 4).toString(),
        blob.readUInt16LE(4), blob.readUInt16LE(6), blob.readUInt16LE(8),
        blob.readUInt16LE(10), blob.readUInt16LE(12),
        blob.readUInt32LE(16), blob.readUInt32LE(20)],
    ['DVID', 1, 256, 256, 10, 0, frames, 22050]);
    let offset = 536, audioBytes = 0;
    for (let i = 0; i < frames; i++) {
        const size = blob.readUInt32LE(offset);
        assert.equal(size % 2, 0);
        audioBytes += size;
        offset += 4 + size + 256 * 256;
        assert.ok(offset <= blob.length);
    }
    assert.ok(audioBytes > 0);
    assert.deepEqual(blob.subarray(offset), Buffer.alloc(4));
});
