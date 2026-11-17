import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {existsSync} from 'node:fs';
import {mkdtemp, readFile} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';
import test from 'node:test';

const root = resolve(import.meta.dirname, '../..');
const available = spawnSync('ffmpeg', ['-version']).status === 0;
const tools = ['build/mpeg_video_cli', 'build/mpeg_audio_cli',
    'build/mpeg_movie_cli'];

function buildCLIs() {
    return spawnSync('make', ['-f', 'tools/portable/Makefile', ...tools],
        {cwd: root, encoding: 'utf8'});
}

function compareFrames(decoded, reference) {
    assert.equal(decoded.length, reference.length);
    let greatestDifference = 0;
    for (let i = 0; i < decoded.length; i++)
        greatestDifference = Math.max(greatestDifference,
            Math.abs(decoded[i] - reference[i]));
    assert.ok(greatestDifference <= 8, `pixel difference: ${greatestDifference}`);
}

test('synthetic MPEG converts video, audio and movie with the native core', {
    skip: !available
}, async t => {
    const compiled = buildCLIs();
    if (compiled.status !== 0) return t.skip('C compiler unavailable');
    const work = await mkdtemp(join(tmpdir(), 'gtasads-mpeg-test-'));
    const source = join(work, 'sample.mpg');
    const encode = spawnSync('ffmpeg', ['-nostdin', '-loglevel', 'error',
        '-f', 'lavfi', '-i', 'testsrc=size=64x48:rate=30:duration=1',
        '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=44100:duration=1',
        '-c:v', 'mpeg1video', '-q:v', '5', '-c:a', 'mp2', '-b:a', '128k',
        '-shortest', source], {encoding: 'utf8'});
    assert.equal(encode.status, 0, encode.stderr);
    const video = join(work, 'frames.yuv');
    const decoded = spawnSync(join(root, tools[0]), [source, video, '10']);
    assert.equal(decoded.status, 0, decoded.stderr?.toString());
    const reference = spawnSync('ffmpeg', ['-nostdin', '-loglevel', 'error',
        '-i', source, '-frames:v', '10', '-pix_fmt', 'yuv420p', '-f',
        'rawvideo', 'pipe:1'], {maxBuffer: 64 * 1048576});
    assert.equal(reference.status, 0, reference.stderr?.toString());
    compareFrames(await readFile(video), reference.stdout);
    const audio = join(work, 'sound.pcm');
    const sound = spawnSync(join(root, tools[1]), [source, audio, '22050']);
    assert.equal(sound.status, 0, sound.stderr?.toString());
    assert.ok((await readFile(audio)).length > 22050);
    const movie = join(work, 'movie.dvid');
    const converted = spawnSync(join(root, tools[2]), [source, movie]);
    assert.equal(converted.status, 0, converted.stderr?.toString());
    const data = await readFile(movie);
    assert.deepEqual([data.subarray(0, 4).toString(),
        data.readUInt16LE(4), data.readUInt16LE(6), data.readUInt16LE(8),
        data.readUInt16LE(10), data.readUInt16LE(12),
        data.readUInt32LE(16), data.readUInt32LE(20)],
    ['DVID', 1, 256, 256, 10, 0, 10, 22050]);
    assert.ok(data.length > 10 * 65536);
    assert.ok(new Set(data.subarray(24, 536)).size > 10);
});

test('real video decoded by the C core stays close to ffmpeg', {
    skip: !available || !existsSync(join(root, 'game/movies/GTAtitles.mpg'))
}, async t => {
    const compiled = buildCLIs();
    if (compiled.status !== 0) return t.skip('C compiler unavailable');
    const work = await mkdtemp(join(tmpdir(), 'gtasads-mpeg-real-test-'));
    const source = join(root, 'game/movies/GTAtitles.mpg');
    const output = join(work, 'frames.yuv');
    const decoded = spawnSync(join(root, tools[0]), [source, output, '50']);
    assert.equal(decoded.status, 0, decoded.stderr?.toString());
    const reference = spawnSync('ffmpeg', ['-nostdin', '-loglevel', 'error',
        '-i', source, '-frames:v', '50', '-pix_fmt', 'yuv420p', '-f',
        'rawvideo', 'pipe:1'], {maxBuffer: 64 * 1048576});
    assert.equal(reference.status, 0, reference.stderr?.toString());
    compareFrames(await readFile(output), reference.stdout);
});
