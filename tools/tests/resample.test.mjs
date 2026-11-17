import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import {existsSync} from 'node:fs';
import {mkdtemp, readFile, writeFile} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';
import test from 'node:test';
import {resampleHeader} from '../generate_resample_table.mjs';

const root = resolve(import.meta.dirname, '../..');

test('Node resample tables match the checked-in C coefficients', async () => {
    const expected = await readFile(resolve(import.meta.dirname,
        '../portable/resample_coeffs.h'), 'utf8');
    assert.equal(resampleHeader(), expected);
});

function convert(module, samples, source, target) {
    const data = module._malloc(Math.max(2, samples.length * 2));
    const pointers = module._malloc(8);
    let output = 0;
    try {
        module.HEAP16.set(samples, data >>> 1);
        assert.equal(module._resample_pcm16(
            data, samples.length, source, target, pointers, pointers + 4), 1);
        output = module.HEAPU32[pointers >>> 2];
        const count = module.HEAPU32[(pointers + 4) >>> 2];
        return Buffer.from(module.HEAPU8.slice(output, output + 2 * count));
    } finally {
        if (output) module._free(output);
        module._free(pointers);
        module._free(data);
    }
}

test('native and WASM resamplers agree across rates and boundaries', {
    skip: !existsSync(join(root, 'web/assets.mjs'))
}, async t => {
    const work = await mkdtemp(join(tmpdir(), 'gtasads-resample-test-'));
    const native = join(work, 'resample');
    const compile = spawnSync('cc', ['-std=c11', '-O2', '-Wall',
        '-Wextra', '-Werror', join(root, 'tools/tests/portable_resample_cli.c'),
        join(root, 'tools/portable/resample.c'), '-o', native]);
    if (compile.status !== 0) return t.skip('C compiler unavailable');
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const python = [
        ['579b6cc24588e23a1b4dae7da39b218371b5a43ea3715a52b860db3a9c0c9979',
            'e9d1dfcf8435690a5d69804f336f7a37b3e4ad12c8cc27d9588b9e7202984a10',
            'c4ffe0186026d2571410115f2a4e60cb554d0c7025c64b41c9ef6b6dcc7b780f',
            '8811e8c41aa60c6340f2829895c67e8302fb556ea75be47f9009cfc3068d770a'],
        ['3c9df467f6d74a0ca821b52542f8dd9404d39a0ae70ef1d488955536ce4f95f8',
            '762e7807049a1b080a0211d5eb303c566f1d6bcad36868f915ad3e5386f31268',
            'ff5e1b4d00a36f0ae61828eacfc84892741b9c13fe6c13e8a132af04c3b11fe6',
            'a6e09eb0b88d8f222571532b4402d64cde3a2792b15f48f7b941cbc548a39d59'],
        ['99ff31f7289a579a15ee200f4fa8e003f73e24d557e98e34e00f326e3f09cd70',
            '733b77aa2a9be182ba17597f4f1675a149e4b8a98f9e2fb59cd6e3c21cd82a52',
            '0d77ec5cd7486be8e020eb08b35de3fd08039f0776da01a1e40ffb2743b46737',
            '3bd06f3f69bd66d031f059af1b5341efbdaacdb62edd0984156df24539db96f0'],
        ['1e1394b63f4a320e0b67caff4f362c539f21c1a7e3ac86edb03ad6ff4a20164c',
            '67b39d022381506420beac9613333b98e9e2da7a20f4262c1db09698763a6cfc',
            '9ca919ffccb2dc223b0539eb37cdb618e73594ef2c4a8f7b4928d5db30bbfea5',
            '35ff0efef4c749b48bfbd3101f276670b5e586d3afdaf149cd1e670cc6b4a525'],
        ['74bb424b22ad618a0af9ef1fc64df56f3d3a8ec930839e7c151aabc8cb413073',
            'e8f45a091d0a07057e7647da724e04cc1e8d2e3909101e65bcac6664afeb6230',
            '91ca4f061e0f6fedc1f6a67f8c80b2abcb446fa97255e1b018c90ee800344ebc',
            '6293e37f62ef623e9ba24e70f438e16ae5ce3d01326166df52c54aa172ead650'],
        ['f6a65fa61f6b243a400544253337faf46da08da590b0876dc22a9ca94191f998',
            'c6a01ab6a6c426627e3b8d09cf2b1ebef811bb740d3ae4eb1da15bb8fcce3d07',
            '1d1e65f6ec74540203ebbdfbc5236d3669de36cdb9c46c7c18ddb7f9dd34d05a',
            '79365bcb28b10fddb490cc00d70f1bef1db62a5108a64d66608e05648c91d069']
    ];
    let state = 1234;
    for (const [rateIndex, [source, target]] of [
        [22050, 11025], [32000, 11025], [44100, 16000],
        [48000, 22050], [96000, 11025], [32000, 8000]
    ].entries()) {
        for (const [lengthIndex, count] of [0, 1, 17, 257, 4096].entries()) {
            const samples = Int16Array.from({length: count}, () => {
                state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
                return (state >>> 16) - 32768;
            });
            const input = join(work, `in-${source}-${target}-${count}.raw`);
            const output = join(work, `out-${source}-${target}-${count}.raw`);
            await writeFile(input, Buffer.from(samples.buffer));
            const run = spawnSync(native, [input, output, String(source), String(target)]);
            assert.equal(run.status, 0, `${source}/${target}/${count}: ${run.stderr}`);
            const wasm = convert(module, samples, source, target);
            assert.deepEqual(wasm, await readFile(output),
                `${source}/${target}/${count}`);
            assert.equal(createHash('sha256').update(wasm).digest('hex'),
                count ? python[rateIndex][lengthIndex - 1] :
                    'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855');
        }
    }
});

test('downsampling suppresses aliased high frequencies but keeps low tones', {
    skip: !existsSync(join(root, 'web/assets.mjs'))
}, async () => {
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const source = 32000, target = 11025;
    const low = Int16Array.from({length: source}, (_, i) =>
        Math.round(Math.sin(i * 2 * Math.PI * 1000 / source) * 12000));
    const high = Int16Array.from({length: source}, (_, i) =>
        Math.round(Math.sin(i * 2 * Math.PI * 8000 / source) * 12000));
    const rms = samples => {
        const middle = samples.subarray(300, samples.length - 300);
        return Math.sqrt(middle.reduce((total, x) => total + x * x, 0) /
                         middle.length);
    };
    const toSamples = bytes =>
        new Int16Array(bytes.buffer, bytes.byteOffset, bytes.length / 2);
    const downLow = toSamples(convert(module, low, source, target));
    const downHigh = toSamples(convert(module, high, source, target));
    assert.ok(Math.abs(rms(downLow) / rms(low) - 1) <= 0.01);
    assert.ok(rms(downHigh) < 10);
});
