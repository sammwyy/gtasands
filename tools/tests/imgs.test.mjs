import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import {existsSync} from 'node:fs';
import {mkdtemp, open, readFile, readdir, writeFile} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join, relative, resolve, sep} from 'node:path';
import test from 'node:test';
import {convertIMG, imgEntries} from '../../web/img_assets.mjs';
import {convertOriginalIMGs} from '../../web/local_imgs.mjs';
import {convertModel} from '../../web/native_convert.mjs';
import {Parameters} from '../../web/params.mjs';

const root = resolve(import.meta.dirname, '../..');
const game = resolve(root, 'game');
const golden = {
    'cw_barnie.dff': [
        '4395066234813b6c3ecb66011a3b110e10efc1f064151c9464d2cfda0700430e',
        '81efb89e1501291bfdf714a997f71bbc46d94b6c260facd109d475cd945163da'],
    'gaz23_law.dff': [
        '54209c1974c16caa93ee146ddcb6f81e992a8a9a7f89a411a08cf92e42a58226',
        '64304b8e1d7f271580f5a0a84f70910eafeb1998a210600bd751f4566ecac83d'],
    'groundbit84_sfs.dff': [
        '57fd71946bd267c2285c2f5e4b877afecf70897e4cb88e1ee252ab1273c8feb6',
        'fd8bd31a1e20a517abef76035348ce8ce41c816af07b6d0875b91fcff79ad7aa'],
    'newcomprd_las2.dff': [
        '55fb7f18938c076ef77315b1b3f1e13ad32fb89f8375759e2d1ae544229741e4',
        '83d5d542f0c18aae74f593629e4f301d396a7fbc3a2cf20e2b7009997bb94ce5'],
    'road_hill04b.dff': [
        'cbd78607d73f6170d1612304eb3365f24b2f826dfa81bd75f35344245230472e',
        'a23fde47aa41ba6ac29cd46c1ea4e8867df4c161ce440b56f8dfcb1d45c7d429'],
    'vegaseroad022.dff': [
        '8f09ce729d1524d64990d25a80806d7b192e96c842cad2a12b12dd9934d24904',
        'd0d35621b4540e2868ba2f0b78e30c9338f887599f4ba50726b9aad09ac12744'],
    'vgssairportland12.dff': [
        'dba4a592e191830c91318a484e2431197013d897709509088c871766b624468c',
        '085093c9a4dfac50e734a849a7dbe467c5c03ef3a3a77888679a17cf4e33edab']
};

function hash(value) {
    return createHash('sha256').update(value).digest('hex');
}

test('IMG v2 index matches Python for synthetic, real and invalid archives', {
    skip: !existsSync(join(root, 'web/assets.mjs'))
}, async t => {
    const build = spawnSync('make', ['-f', 'tools/portable/Makefile',
        'build/img_cli'], {cwd: root, encoding: 'utf8'});
    if (build.status !== 0) return t.skip('C compiler unavailable');
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const work = await mkdtemp(join(tmpdir(), 'gtasads-img-index-test-'));
    const archive = join(work, 'test.img');
    const sample = Buffer.alloc(9 * 2048);
    sample.write('VER2');
    sample.writeUInt32LE(2, 4);
    sample.writeUInt32LE(3, 8);
    sample.writeUInt16LE(1, 12);
    sample.writeUInt16LE(2, 14);
    sample.write('HELLO.TXD', 16);
    sample.writeUInt32LE(5, 40);
    sample.writeUInt16LE(4, 44);
    sample.write('other.dff', 48);
    await writeFile(archive, sample);
    const fixtures = [
        {path: archive, entries: [
            {name: 'HELLO.TXD', offset: 3 * 2048, size: 2 * 2048},
            {name: 'other.dff', offset: 5 * 2048, size: 4 * 2048}
        ]}
    ];
    const real = join(game, 'models/gta3.img');
    if (existsSync(real)) fixtures.push({path: real});
    for (const {path, entries} of fixtures) {
        const handle = await open(path);
        try {
            const file = {
                size: (await handle.stat()).size,
                slice(start, end) {
                    return {async arrayBuffer() {
                        const bytes = Buffer.alloc(end - start);
                        const {bytesRead} = await handle.read(bytes, 0,
                            bytes.length, start);
                        assert.equal(bytesRead, bytes.length);
                        return bytes;
                    }};
                }
            };
            const parsed = await imgEntries(module, file);
            if (entries) assert.deepEqual(parsed, entries);
            const run = spawnSync(join(root, 'build/img_cli'), [path],
                {encoding: 'latin1', maxBuffer: 2 ** 22});
            assert.equal(run.status, 0, run.stderr);
            const text = parsed.map(({name, offset, size}) =>
                `${name}\t${offset}\t${size}\n`).join('');
            assert.equal(run.stdout, text);
            if (path === real && file.size === 940064768) {
                assert.equal(parsed.length, 16316);
                assert.equal(hash(Buffer.from(text, 'latin1')),
                    'ad9aa2a670070a6cbb410f1c52ebd1cffe2dd05e15f1f8b561b7c56abd90de2c');
            }
        } finally {
            await handle.close();
        }
    }
    const invalid = join(work, 'invalid.img');
    await writeFile(invalid, Buffer.from([86, 69, 82, 50, 231, 3, 0, 0]));
    assert.notEqual(spawnSync(join(root, 'build/img_cli'), [invalid]).status, 0);
    await assert.rejects(imgEntries(module, new Blob([
        await readFile(invalid)])), /Incomplete IMG index/);
});

test('missing IMG archives are built locally only when necessary', async () => {
    const manifest = {sections: {
        gta3_img: {src: 'models/gta3.img', out: 'models/gta3.dspk'},
        gta_int_img: {src: 'models/gta_int.img', out: 'models/gta_int.dspk'},
        player_img: {src: 'models/player.img', out: 'models/player.dspk'},
        anim_img: {src: 'anim/anim.img', out: 'anim/anim.dspk'}
    }};
    const original = new Map(Object.values(manifest.sections).map(section =>
        [section.src, new Blob([])]));
    let terminated = false;
    const workerFactory = () => ({
        postMessage({files, section}) {
            assert.equal(files, original);
            queueMicrotask(() => this.onmessage({
                data: {path: manifest.sections[section].out,
                       file: new Blob(['DSPK']), entries: 1, skipped: 0}
            }));
        },
        terminate() {terminated = true;}
    });
    const built = await convertOriginalIMGs(original, [], manifest,
        () => {}, workerFactory);
    assert.equal(built.length, 4);
    assert.equal(terminated, true);
    assert.deepEqual(await convertOriginalIMGs(original, built, manifest,
        () => assert.fail('already converted'),
        () => assert.fail('worker not needed')), []);
});

test('real ground DFFs match the frozen Python reference when input matches', {
    skip: !existsSync(resolve(root, 'web/assets.mjs')) ||
        !existsSync(resolve(game, 'models/gta3.img'))
}, async () => {
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const manifest = JSON.parse(await readFile(resolve(root, 'web/resources.json')));
    const paths = (await readdir(resolve(game, 'data'), {
        recursive: true, withFileTypes: true
    })).filter(entry => entry.isFile() && entry.name.toLowerCase().endsWith('.ide'));
    const original = new Map(paths.map(entry => {
        const absolute = resolve(entry.parentPath, entry.name);
        return [relative(game, absolute).split(sep).join('/').toLowerCase(),
            {text: () => readFile(absolute, 'utf8')}];
    }));
    const planner = new Parameters(manifest, original);
    const handle = await open(resolve(game, 'models/gta3.img'));
    try {
        const readRange = async (start, size) => {
            const bytes = Buffer.allocUnsafe(size);
            let offset = 0;
            while (offset < size) {
                const {bytesRead} = await handle.read(bytes, offset, size - offset,
                                                      start + offset);
                if (!bytesRead) throw new Error('Truncated GTA IMG archive.');
                offset += bytesRead;
            }
            return bytes;
        };
        const file = {size: (await handle.stat()).size,
            slice(start, end) {
                return {arrayBuffer: () => readRange(start, end - start)};
            }};
        const entries = (await imgEntries(module, file)).filter(item =>
            Object.hasOwn(golden, item.name));
        assert.deepEqual(new Set(entries.map(item => item.name)),
                         new Set(Object.keys(golden)));
        for (const {name, offset, size} of entries) {
            const input = await readRange(offset, size);
            const {params} = await planner.params('gta3_img', 'model', name);
            const actual = convertModel(module, input, params);
            assert.equal(Buffer.from(actual).subarray(0, 4).toString(), 'DMDL');
            if (hash(input) === golden[name][0])
                assert.equal(hash(actual), golden[name][1], name);
        }
    } finally {
        await handle.close();
    }
});

test('real interior IMG entries make the same mixed DSPK as the Python reference', {
    skip: !existsSync(resolve(root, 'web/assets.mjs')) ||
        !existsSync(resolve(game, 'models/gta_int.img'))
}, async () => {
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const manifest = JSON.parse(await readFile(resolve(root, 'web/resources.json')));
    const names = ['8stad.txd', '8bar5.dff', 'gen_int2_3.col', 'gen_int5.ifp'];
    const goldenSources = {
        '8bar5.dff': '79973f1ccfe9ba011fdf811e9d9c9152ce986a8625ea7cfd71d6ff2cbdb54e8d',
        '8stad.txd': '46f32f6386ef895cc264d0a25a1021e67e64c076fc65fd44a33f01e219c08eac',
        'gen_int2_3.col': '573ebe72dfd2a3c9ce5575c594456035dfebd225b3d76c72756910eb40f93993',
        'gen_int5.ifp': 'bfe08ead2edeff2008a5e1b69ea09712e63108c6e7e86a867e1bff0e9ce1225c'
    };
    const handle = await open(resolve(game, 'models/gta_int.img'));
    try {
        const size = (await handle.stat()).size;
        const file = {size, slice(start, end) {
            return {arrayBuffer: async () => {
                const bytes = Buffer.allocUnsafe(end - start);
                const {bytesRead} = await handle.read(bytes, 0, bytes.length, start);
                assert.equal(bytesRead, bytes.length);
                return bytes;
            }};
        }};
        const toc = new Map((await imgEntries(module, file)).map(entry =>
            [entry.name, entry]));
        const sources = new Map();
        for (const name of names) {
            const entry = toc.get(name);
            assert.ok(entry, name);
            sources.set(name, Buffer.from(await file.slice(entry.offset,
                entry.offset + entry.size).arrayBuffer()));
        }
        sources.set('sample.dat', Buffer.concat([Buffer.from('local copy'),
            Buffer.alloc(2048 - 10)]));
        const index = Buffer.alloc(2048);
        index.write('VER2');
        index.writeUInt32LE(sources.size, 4);
        const parts = [index];
        let sector = 1, position = 0;
        for (const [name, bytes] of sources) {
            const count = Math.ceil(bytes.length / 2048);
            index.writeUInt32LE(sector, 8 + position * 32);
            index.writeUInt16LE(count, 12 + position * 32);
            index.writeUInt16LE(count, 14 + position * 32);
            index.write(name, 16 + position * 32);
            parts.push(Buffer.concat([bytes, Buffer.alloc(count * 2048 - bytes.length)]));
            sector += count;
            position++;
        }
        const planner = new Parameters(manifest, new Map());
        const result = await convertIMG(module, planner, 'gta_int_img', new Blob(parts));
        assert.equal(result.files, 5);
        assert.equal(result.skipped, 0);
        const output = Buffer.from(await result.blob.arrayBuffer());
        assert.equal(output.subarray(0, 4).toString(), 'DSPK');
        if (names.every(name => hash(sources.get(name)) === goldenSources[name]))
            assert.equal(hash(output),
                '6df5ef84f3e1db0e5e4b1ddea084cfdab43ace240200a71f7b7a72eb374413db');
    } finally {
        await handle.close();
    }
});
