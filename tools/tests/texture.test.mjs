import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import {existsSync, readFileSync} from 'node:fs';
import {mkdtemp, readFile, writeFile} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';
import test from 'node:test';
import {convertTexture} from '../../web/native_convert.mjs';

const root = resolve(import.meta.dirname, '../..');
const game = join(root, 'game');
const golden = JSON.parse(readFileSync(new URL('./texture_golden.json', import.meta.url)));
const wasm = existsSync(join(root, 'web/assets.mjs'));
const compiler = spawnSync('cc', ['--version']).status === 0;
const checksum = bytes => createHash('sha256').update(bytes).digest('hex');
let work, dtexCLI, txdCLI;

async function compile(name, files) {
    work ??= await mkdtemp(join(tmpdir(), 'gtasads-texture-test-'));
    const path = join(work, name);
    const run = spawnSync('cc', ['-std=c11', '-Wall', '-Wextra',
        '-Werror', '-O2', ...files.map(file =>
            join(root, 'tools/portable', file)), '-o', path], {encoding: 'utf8'});
    assert.equal(run.status, 0, run.stderr);
    return path;
}

async function dtex() {
    dtexCLI ??= await compile('dtex_cli', ['dtex.c', 'dtex_cli.c']);
    return dtexCLI;
}

async function txd() {
    txdCLI ??= await compile('txd_cli', ['txd.c', 'txd_cli.c']);
    return txdCLI;
}

async function encodeRGBA(bytes, width, height, options, name) {
    const input = join(work, `${name}.rgba`), output = join(work, `${name}.dtex`);
    await writeFile(input, bytes);
    const run = spawnSync(await dtex(), [
        input, String(width), String(height), options.format || 'auto', output,
        '24', options.opaque ? '1' : '0',
        String(options.auto_colors || 16), options.additive ? '1' : '0'
    ], {encoding: 'utf8'});
    assert.equal(run.status, 0, run.stderr);
    return readFile(output);
}

async function decodedTexture(path, index, level) {
    const output = join(work, `decoded-${index}-${level}.rgba`);
    const run = spawnSync(await txd(), [
        path, String(index), String(level), output
    ], {encoding: 'utf8'});
    assert.equal(run.status, 0, run.stderr);
    return readFile(output);
}

function resizeRGBA(px, sw, sh, tw, th) {
    if (sw === tw && sh === th) return px;
    const output = Buffer.alloc(tw * th * 4);
    if (sw % tw === 0 && sh % th === 0) {
        const fx = sw / tw, fy = sh / th;
        for (let y = 0; y < th; y++)
            for (let x = 0; x < tw; x++) {
                let r = 0, g = 0, b = 0, a = 0;
                for (let yy = y * fy; yy < y * fy + fy; yy++)
                    for (let xx = x * fx; xx < x * fx + fx; xx++) {
                        const p = (yy * sw + xx) * 4, alpha = px[p + 3];
                        r += px[p] * alpha;
                        g += px[p + 1] * alpha;
                        b += px[p + 2] * alpha;
                        a += alpha;
                    }
                if (a) {
                    const p = (y * tw + x) * 4;
                    output[p] = Math.trunc(r / a);
                    output[p + 1] = Math.trunc(g / a);
                    output[p + 2] = Math.trunc(b / a);
                    output[p + 3] = Math.trunc(a / (fx * fy));
                }
            }
    } else {
        for (let y = 0; y < th; y++)
            for (let x = 0; x < tw; x++) {
                const sx = Math.min(sw - 1, Math.trunc(x * sw / tw));
                const sy = Math.min(sh - 1, Math.trunc(y * sh / th));
                px.copy(output, (y * tw + x) * 4,
                    (sy * sw + sx) * 4, (sy * sw + sx) * 4 + 4);
            }
    }
    return output;
}

test('DTEX formats and alpha modes match the Python palette oracle', {
    skip: !compiler
}, async () => {
    await dtex();
    for (const mode of ['opaque', 'binary', 'gradient', 'clear']) {
        const rgba = Buffer.alloc(16 * 16 * 4);
        for (let i = 0; i < 256; i++) {
            const p = i * 4;
            rgba[p] = (i * 73 + 17) % 256;
            rgba[p + 1] = (i * 37 + 61) % 256;
            rgba[p + 2] = (i * 19 + 5) % 256;
            rgba[p + 3] = mode === 'opaque' ? 255 :
                mode === 'binary' ? i % 2 ? 255 : 0 :
                mode === 'gradient' ? (i * 91 + 43) % 256 : 0;
        }
        for (const format of ['auto', 'direct', 'pal4', 'pal16',
            'pal256', 'a3i5', 'a5i3']) {
            const data = await encodeRGBA(rgba, 16, 16, {format},
                `synthetic-${mode}`);
            assert.equal(checksum(data), golden.formats[`${mode}:${format}`],
                `${mode}:${format}`);
        }
        for (const [name, options] of [['additive', {additive: true}],
            ['opaque', {opaque: true, auto_colors: 256}]]) {
            const data = await encodeRGBA(rgba, 16, 16, options,
                `synthetic-${mode}`);
            assert.equal(checksum(data), golden.formats[`${mode}:${name}`],
                `${mode}:${name}`);
        }
    }
});

test('real HUD texture samples decode, resize and encode like Python', {
    skip: !compiler || !existsSync(join(game, 'models/hud.txd'))
}, async () => {
    await dtex();
    const path = join(game, 'models/hud.txd');
    const known = checksum(await readFile(path)) ===
        golden.real['models/hud.txd'].source;
    for (let i = 0; i < 8; i++) {
        const rgba = await decodedTexture(path, i, 0);
        const pixels = resizeRGBA(rgba.subarray(4),
            rgba.readUInt16LE(0), rgba.readUInt16LE(2), 32, 32);
        const data = await encodeRGBA(pixels, 32, 32, {},
            `hud-${i}`);
        assert.equal(data.subarray(0, 4).toString(), 'DTEX');
        if (known)
            assert.equal(checksum(data), golden.real['models/hud.txd'].resize[i],
                `HUD texture ${i}`);
    }
});

test('first real HUD texture pipeline output matches Python', {
    skip: !compiler || !existsSync(join(game, 'models/hud.txd'))
}, async () => {
    await dtex();
    const path = join(game, 'models/hud.txd');
    const source = await readFile(path), rgba = await decodedTexture(path, 0, 0);
    const pixels = resizeRGBA(rgba.subarray(4),
        rgba.readUInt16LE(0), rgba.readUInt16LE(2), 16, 16);
    const data = await encodeRGBA(pixels, 16, 16, {}, 'hud-first');
    assert.equal(data.subarray(0, 4).toString(), 'DTEX');
    if (checksum(source) === golden.real['models/hud.txd'].source)
        assert.equal(checksum(data), golden.real['models/hud.txd'].single);
});

test('full HUD dictionary applies per-texture overrides like Python', {
    skip: !wasm || !existsSync(join(game, 'models/hud.txd'))
}, async () => {
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const source = await readFile(join(game, 'models/hud.txd'));
    const defaults = JSON.parse(await readFile(join(root, 'web/resources.json')))
        .defaults.texture;
    const converted = Buffer.from(convertTexture(module, source,
        {...defaults, scale: 0.5, max_size: 64},
        [['*.png', {format: 'direct'}], ['*', {max_size: 32}]]));
    assert.equal(converted.subarray(0, 4).toString(), 'DTXD');
    if (checksum(source) === golden.real['models/hud.txd'].source)
        assert.equal(checksum(converted), golden.real['models/hud.txd'].overrides);
});

test('TXD decoder agrees with Python for HUD, fonts, menus and vehicles', {
    skip: !compiler
}, async t => {
    await txd();
    let checked = 0;
    for (const [name, fixture] of Object.entries(golden.real)) {
        const path = join(game, name);
        if (!existsSync(path)) continue;
        const known = checksum(await readFile(path)) === fixture.source;
        for (const [index, expected] of fixture.decode.entries()) {
            const data = await decodedTexture(path, index, 0);
            assert.ok(data.readUInt16LE(0) > 0 && data.readUInt16LE(2) > 0);
            assert.equal(data.length,
                4 + data.readUInt16LE(0) * data.readUInt16LE(2) * 4);
            if (known) assert.equal(checksum(data), expected, `${name}:${index}`);
            checked++;
        }
    }
    if (!checked) t.skip('local GTA installation unavailable');
});

test('native full TXD CLI equals Python and browser WASM', {
    skip: !compiler || !wasm ||
        !existsSync(join(game, 'models/fronten1.txd'))
}, async () => {
    const built = spawnSync('make', ['-f', 'tools/portable/Makefile',
        'build/texture_cli'], {cwd: root, encoding: 'utf8'});
    assert.equal(built.status, 0, built.stderr);
    work ??= await mkdtemp(join(tmpdir(), 'gtasads-texture-test-'));
    const source = join(game, 'models/fronten1.txd');
    const output = join(work, 'fronten1.dtxd');
    const run = spawnSync(join(root, 'build/texture_cli'),
        [source, output, '0.5', '128', 'auto'], {encoding: 'utf8'});
    assert.equal(run.status, 0, run.stderr);
    const converted = await readFile(output);
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const original = await readFile(source);
    const defaults = JSON.parse(await readFile(join(root, 'web/resources.json')))
        .defaults.texture;
    assert.equal(checksum(convertTexture(module, original,
        {...defaults, scale: 0.5, max_size: 128})), checksum(converted));
    if (checksum(original) === golden.real['models/fronten1.txd'].source)
        assert.equal(checksum(converted), golden.real['models/fronten1.txd'].full);
});
