import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import {existsSync} from 'node:fs';
import {mkdtemp, open, readFile, readdir, writeFile} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join, relative, resolve, sep} from 'node:path';
import test from 'node:test';
import {imgEntries} from '../../web/img_assets.mjs';
import {convertModel} from '../../web/native_convert.mjs';
import {Parameters} from '../../web/params.mjs';

const root = resolve(import.meta.dirname, '../..');
const game = join(root, 'game');
const wasm = existsSync(join(root, 'web/assets.mjs'));
const checksum = bytes => createHash('sha256').update(bytes).digest('hex');
const known = {
    ground_models: [
        '98099192fe07f8bfeb8a4ecd919859112725cdcdd6f8912db4054a402205571a',
        '2d4c957e6468bd4eb8560ef3d586371f5a735804793d9206f64d422a2c49b375',
        83328
    ],
    cli: [
        'a6970eb696566d62ad0a7f5ce771f76b51529a9e21bc84c41a0c9f45edf721df',
        'f4ddf38ba74a4ffd0ba0d95f110ea67290125159acc8c6ce19dcffdf6f2886f5',
        1664
    ],
    complete: [
        '25a1c1ee53a05bc69a7d728e9313a230748eacf51d9d0be050fccffa606c8c20',
        '2177ca7e53852fcc7a2293a1ec484ce640ad0fac750222a4a087acee4c626aaa',
        18784
    ],
    geometry: [
        'f1643196fe620e2845da1cbf1433c9bbf6506c3c6a8462d1e59749d0b2328b06',
        '0cb8848c3ca6a8c95a5896fd4e88d359529b701c60de4f277da413f6015ddae5',
        20168
    ],
    skinned: [
        'cfd863250de91f7489317af87869813a526530ec0afeb9b9409bb76e277cb120',
        '77f72972b68317fd182abb46f55209e334400b96ee03913a38d79150b95f6f74',
        34136
    ]
};

async function archive(path, module) {
    const handle = await open(path, 'r');
    const read = async (start, size) => {
        const bytes = Buffer.alloc(size);
        let used = 0;
        while (used < size) {
            const {bytesRead} = await handle.read(bytes,
                used, size - used, start + used);
            if (!bytesRead) throw new Error(`Truncated archive: ${path}`);
            used += bytesRead;
        }
        return bytes;
    };
    const size = (await handle.stat()).size;
    const file = {
        size,
        slice(start, end) {
            return {async arrayBuffer() {
                const bytes = await read(start, end - start);
                return bytes.buffer.slice(bytes.byteOffset,
                    bytes.byteOffset + bytes.byteLength);
            }};
        }
    };
    return {toc: await imgEntries(module, file), read, close: () => handle.close()};
}

function reference(group, input, output, length) {
    assert.equal(input.length, output.length);
    const digest = items => checksum(Buffer.from(items.map(
        ([name, raw]) => `${name}:${checksum(raw)};`).join('')));
    if (digest(input) === known[group][0]) {
        assert.equal(digest(output), known[group][1], group);
        assert.equal(length, known[group][2], group);
    }
}

async function compare(group, module, source, entries, params) {
    const inputs = [], outputs = [];
    let length = 0;
    for (const entry of entries) {
        const data = await source.read(entry.offset, entry.size);
        const options = typeof params === 'function' ?
            await params(entry.name) : params;
        const converted = Buffer.from(convertModel(module, data, options));
        assert.equal(converted.subarray(0, 4).toString(), 'DMDL', entry.name);
        inputs.push([entry.name, data]);
        outputs.push([entry.name, converted]);
        length += converted.length;
    }
    reference(group, inputs, outputs, length);
    return {inputs, outputs};
}

test('real ground DFFs match Python with resolved section rules', {
    skip: !wasm || !existsSync(join(game, 'models/gta3.img'))
}, async () => {
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const source = await archive(join(game, 'models/gta3.img'), module);
    try {
        const names = ['cw_barnie.dff', 'gaz23_law.dff', 'groundbit84_sfs.dff',
            'newcomprd_las2.dff', 'road_hill04b.dff',
            'vegaseroad022.dff', 'vgssairportland12.dff'];
        const lookup = new Map(source.toc.map(entry => [entry.name, entry]));
        const files = new Map((await readdir(join(game, 'data'), {
            recursive: true, withFileTypes: true
        })).filter(entry => entry.isFile() && /\.ide$/i.test(entry.name))
            .map(entry => {
                const path = join(entry.parentPath, entry.name);
                return [relative(game, path).split(sep).join('/').toLowerCase(),
                    {text: () => readFile(path, 'utf8')}];
            }));
        const planner = new Parameters(JSON.parse(await readFile(
            join(root, 'web/resources.json'))), files);
        await compare('ground_models', module, source, names.map(name => {
            assert.ok(lookup.has(name), name);
            return lookup.get(name);
        }), async name => {
            const {params, removed} = await planner.params('gta3_img', 'model', name);
            assert.equal(removed, false, name);
            return params;
        });
    } finally {
        await source.close();
    }
});

test('native DMDL CLI matches Python and browser for the first real model', {
    skip: !wasm || !existsSync(join(game, 'models/gta3.img'))
}, async t => {
    const built = spawnSync('make', ['-f', 'tools/portable/Makefile',
        'build/dmdl_cli'], {cwd: root, encoding: 'utf8'});
    if (built.status !== 0) return t.skip('C compiler unavailable');
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const source = await archive(join(game, 'models/gta3.img'), module);
    try {
        const entry = source.toc.find(({name}) => /\.dff$/i.test(name));
        assert.ok(entry);
        const defaults = JSON.parse(await readFile(join(root,
            'web/resources.json'))).defaults.model;
        const params = {...defaults, ratio: 0.5, min_tris: 32,
            drop_damage: false};
        const {inputs, outputs} = await compare('cli', module, source,
            [entry], params);
        const work = await mkdtemp(join(tmpdir(), 'gtasads-dmdl-cli-test-'));
        const input = join(work, entry.name), output = join(work, 'model.dmdl');
        await writeFile(input, inputs[0][1]);
        const run = spawnSync(join(root, 'build/dmdl_cli'),
            [input, output, '0.5', '32'], {encoding: 'utf8'});
        assert.equal(run.status, 0, run.stderr);
        assert.deepEqual(await readFile(output), outputs[0][1]);
    } finally {
        await source.close();
    }
});

test('complete real models match Python over varied archive positions', {
    skip: !wasm || !existsSync(join(game, 'models/gta3.img'))
}, async () => {
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const source = await archive(join(game, 'models/gta3.img'), module);
    try {
        const entries = source.toc.filter(({name}) => /\.dff$/i.test(name));
        const defaults = JSON.parse(await readFile(join(root,
            'web/resources.json'))).defaults.model;
        await compare('complete', module, source, [
            ...entries.slice(0, 10), ...entries.slice(500, 510),
            ...entries.filter(({name}) => name === 'ws_floodbeams.dff')
        ], {...defaults, ratio: 0.37, min_tris: 32, max_tris: 280,
            normals: true, drop_damage: true, drop_vlo: true});
    } finally {
        await source.close();
    }
});

test('ground QEM DMDL geometry matches Python around triangle limits', {
    skip: !wasm || !existsSync(join(game, 'models/gta3.img'))
}, async () => {
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const source = await archive(join(game, 'models/gta3.img'), module);
    try {
        const entries = source.toc.filter(({name}) => /\.dff$/i.test(name));
        const defaults = JSON.parse(await readFile(join(root,
            'web/resources.json'))).defaults.model;
        await compare('geometry', module, source, [
            ...entries.slice(0, 5), ...entries.slice(500, 510),
            ...entries.slice(2000, 2005)
        ], {...defaults, ratio: 0.25, min_tris: 12, max_tris: 192,
            min_area: 0.85, max_shift: 0.5, ground_qem: true,
            drop_damage: false});
    } finally {
        await source.close();
    }
});

test('skinned character and rigid cutscene model match Python variants', {
    skip: !wasm || !existsSync(join(game, 'models/player.img')) ||
        !existsSync(join(game, 'models/cutscene/cshandcuff.dff'))
}, async () => {
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const source = await archive(join(game, 'models/player.img'), module);
    try {
        const first = source.toc.find(({name}) => /\.dff$/i.test(name));
        const data = await source.read(first.offset, first.size);
        const rigidPath = join(game, 'models/cutscene/cshandcuff.dff');
        const rigid = await readFile(rigidPath);
        const defaults = JSON.parse(await readFile(join(root,
            'web/resources.json'))).defaults.model;
        const params = [
            {...defaults, ratio: 0.5, normals: true, decimate: 'qem',
                drop_damage: false},
            {...defaults, ratio: 0.5, normals: true, drop_damage: false},
            {...defaults, ratio: 0.28, rigid_skin: true, drop_damage: false}
        ];
        const inputs = [[first.name, data], [first.name, data],
            ['cshandcuff.dff', rigid]];
        const outputs = inputs.map(([name, raw], i) => [name,
            Buffer.from(convertModel(module, raw, params[i]))]);
        for (const [, bytes] of outputs)
            assert.equal(bytes.subarray(0, 4).toString(), 'DMDL');
        reference('skinned', inputs, outputs,
            outputs.reduce((sum, [, bytes]) => sum + bytes.length, 0));
    } finally {
        await source.close();
    }
});

test('first 120 real DFF bounding radii match the Python world planner', {
    skip: !wasm || !existsSync(join(game, 'models/gta3.img'))
}, async () => {
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const source = await archive(join(game, 'models/gta3.img'), module);
    const inputHash = createHash('sha256');
    const radii = [];
    try {
        for (const {name, offset, size} of source.toc.filter(({name}) =>
            /\.dff$/i.test(name)).slice(0, 120)) {
            const data = await source.read(offset, size);
            inputHash.update(name).update(Buffer.from([0]))
                .update(createHash('sha256').update(data).digest());
            const input = module._malloc(data.length), result = module._malloc(2);
            try {
                module.HEAPU8.set(data, input);
                radii.push(module._dff_radius(input, data.length, result) ?
                    new DataView(module.HEAPU8.buffer).getUint16(result, true) : 64);
            } finally {
                module._free(input);
                module._free(result);
            }
        }
        assert.equal(radii.length, 120);
        if (inputHash.digest('hex') ===
            '671149e28c04ca041f7a37cf9accfe892a89c77a200d81f06eb5e60e202346d9') {
            const packed = Buffer.alloc(radii.length * 2);
            for (const [i, radius] of radii.entries())
                packed.writeUInt16LE(radius, i * 2);
            assert.equal(checksum(packed),
                'f197700fbd137cfbf264fc292620d175e7594278ae0f2571969a8d1cb7077f65');
        }
    } finally {
        await source.close();
    }
});
