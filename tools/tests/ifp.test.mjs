import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import {existsSync} from 'node:fs';
import {mkdtemp, readFile, writeFile} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';
import test from 'node:test';
import {imgEntries} from '../../web/img_assets.mjs';
import {convertAnimation} from '../../web/native_convert.mjs';

const root = resolve(import.meta.dirname, '../..');
const wasm = existsSync(join(root, 'web/assets.mjs'));
const checksum = bytes => createHash('sha256').update(bytes).digest('hex');

function chunk(tag, body) {
    const header = Buffer.alloc(8);
    header.write(tag);
    header.writeUInt32LE(body.length, 4);
    return Buffer.concat([header, body,
        Buffer.alloc(-body.length & 3)]);
}

function frame(x, y, z, time) {
    const bytes = Buffer.alloc(32);
    for (const [i, value] of [0, 0, 0, 1, x, y, z, time].entries())
        bytes.writeFloatLE(value, i * 4);
    return bytes;
}

function sample(clip, block, sequences) {
    const clips = sequences.map(({name, frames, bone = -1}) => {
        const anim = Buffer.alloc(44);
        anim.write(name);
        anim.writeUInt32LE(frames.length, 28);
        anim.writeInt32LE(bone, 40);
        return chunk('CPAN', Buffer.concat([
            chunk('ANIM', anim), chunk('KRT0', Buffer.concat(frames))
        ]));
    });
    const info = Buffer.alloc(8);
    info.writeUInt32LE(sequences.length);
    return chunk('ANPK', Buffer.concat([
        chunk('INFO', Buffer.concat([Buffer.from([1, 0, 0, 0]),
            Buffer.from(block + '\0')])),
        chunk('NAME', Buffer.from(clip + '\0')),
        chunk('DGAN', Buffer.concat([chunk('INFO', info), ...clips]))
    ]));
}

async function convert(data, options = {}, module) {
    if (!module) {
        const {default: createModule} = await import('../../web/assets.mjs');
        module = await createModule();
    }
    const defaults = JSON.parse(await readFile(join(root, 'web/resources.json')))
        .defaults.anim;
    return Buffer.from(convertAnimation(module, data,
        {...defaults, keep_every: 1, ...options}));
}

test('cutscene IFP frames convert to a streamable DANM clip', {
    skip: !wasm
}, async () => {
    const frames = [frame(1, 2, 3, 0), frame(2, 3, 4, 0.5)];
    const data = sample('csplay', 'PROLOG3',
        [{name: 'root', frames, bone: 0}]);
    assert.equal(checksum(data),
        '0898dc6e970f5608c033d2a224ca73459a90fadbdb0d59e71da6b7f6fc67d9df');
    const result = await convert(data);
    assert.equal(result.subarray(0, 4).toString(), 'DANM');
    assert.equal(checksum(result),
        'cb3ff1171890d1aef29725d7ad5e174ec1c37d46eaf59cd162935b495afc39c8');
});

test('cutscene root translations are not clamped at 32 metres', {
    skip: !wasm
}, async () => {
    const data = sample('csplay', 'PROLOG1', [{
        name: 'root', bone: 0,
        frames: [frame(-100, 75, 3, 0), frame(-120, 80, 3, 1)]
    }]);
    assert.equal(checksum(data),
        '3442f87c73965797c7767a85aefc629fcbbc16b36afb8921c7e3e84fbd1c5f28');
    const result = await convert(data);
    assert.equal(result.readUInt16LE(4), 3);
    assert.equal(result[32 + 32 + 28 + 27], 2);
    assert.equal(result.readInt16LE(32 + 32 + 28 + 32 + 10), -25600);
    assert.equal(checksum(result),
        'cc9820f6aa65349ab68675403a685d61ce2eaed9064a133fea4e3fccda239844');
});

test('rigid cutscene sequences preserve HAnim ordering', {
    skip: !wasm
}, async () => {
    const frames = [frame(1, 2, 3, 0), frame(1, 2, 3, 1)];
    const data = sample('cshandcuff', 'PROLOG3',
        [{name: 'DummyRoot', frames}, {name: 'cuff01', frames}]);
    assert.equal(checksum(data),
        'b3f30f34ac4bfc0f6e8aa4605462be740eb8179b9e69358256b0f078879696db');
    const result = await convert(data, {rigid_clips: ['cshandcuff']});
    assert.equal(result.readInt16LE(64 + 28 + 24), 0);
    assert.equal(result.readInt16LE(64 + 28 + 32 + 32 + 24), 1);
    assert.equal(checksum(result),
        '528e33af0b4e37b0b034c33476440749f4415346df1ca7c5b924e4ef0f22037a');
});

test('native IFP CLI equals Python and the browser WASM clip', {
    skip: !wasm
}, async t => {
    const built = spawnSync('make', ['-f', 'tools/portable/Makefile',
        'build/ifp_cli'], {cwd: root, encoding: 'utf8'});
    if (built.status !== 0) return t.skip('C compiler unavailable');
    const data = sample('csgizmo', 'PROLOG3', [{
        name: 'root',
        frames: [frame(-100, 75, 3, 0), frame(-120, 80, 3, 1)]
    }]);
    assert.equal(checksum(data),
        '68db77e467a399a71bd69ae190a9a10593bd1666eecabb7a8b5a05269cacb818');
    const expected = await convert(data, {keep_every: 2});
    assert.equal(checksum(expected),
        'f32e70ec3d9793c1984783a5c2e068bbd5076f0ada32e1b76c2a7b715021fdcd');
    const work = await mkdtemp(join(tmpdir(), 'gtasads-ifp-cli-test-'));
    const input = join(work, 'sample.ifp');
    const output = join(work, 'sample.danm');
    await writeFile(input, data);
    const run = spawnSync(join(root, 'build/ifp_cli'),
        [input, output, '2', '0', '0'], {encoding: 'utf8'});
    assert.equal(run.status, 0, run.stderr);
    assert.deepEqual(await readFile(output), expected);
});

test('IFP parameters match Python on synthetic, ped and archive animations', {
    skip: !wasm
}, async () => {
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const variants = [
        {keep_every: 2},
        {keep_every: 4, max_frames: 3, drop_translation: true,
            rigid_clips: ['csgizmo']},
        {remove: ['csgizmo', 'idle*']},
        {remove: ['[c]sgizmo', 'idle[0-9]*']},
        {remove: ['[!c]sgizmo', 'csg[!z]mo']},
        {rigid_clips: ['CSGIZMO']}
    ];
    const fixed = {
        'sample.ifp': ['68db77e467a399a71bd69ae190a9a10593bd1666eecabb7a8b5a05269cacb818', [
            'f32e70ec3d9793c1984783a5c2e068bbd5076f0ada32e1b76c2a7b715021fdcd',
            'f39e1092b4b0cd313bbc53febcbac4bdb094e3116e8dfbd6c64e98c574c45142',
            '8f259e2060469af595481b14d5db9411b20e0d3d65412d3ad604e3d4ffe72da3',
            '8f259e2060469af595481b14d5db9411b20e0d3d65412d3ad604e3d4ffe72da3',
            'f32e70ec3d9793c1984783a5c2e068bbd5076f0ada32e1b76c2a7b715021fdcd',
            'f32e70ec3d9793c1984783a5c2e068bbd5076f0ada32e1b76c2a7b715021fdcd']],
        'anim/ped.ifp': ['f7483f49080a0f833dc73ef09aa2224f99a9e90757fa5c2b7db5505cbef33eac', [
            'e7cc69d61da24eea9dfae04dae6aba1038da7edc0a8ae452b53c7aec4da033cd',
            '321470a5e53ef3dbc189fa6dc428c249dcfe9f769f3b16d218f9e689a9f431d4',
            '11b4205286879155354e55da2581555ad60ec9950e06967b5ce54f734fce541c',
            'bb52d1a07c6a7a1d57163ba17f65a1b7e1da8735aea3e126585dbd186829a019',
            'bb52d1a07c6a7a1d57163ba17f65a1b7e1da8735aea3e126585dbd186829a019',
            'bb52d1a07c6a7a1d57163ba17f65a1b7e1da8735aea3e126585dbd186829a019']],
        'anim/anim.img:airport.ifp': ['8c0fd17d336344399a004ea82d444882296cb71927c8c30fd6f4749b83d73ac2', [
            'f8e41bf4cf018239b64e14ea96ae2f73a6e2ca495c66e9822e127303c30f66f2',
            '5d958f28acc4be35d6f185d4a549420a323f5195400fb39d2b184614c9ccac41',
            '93c6065976bd7587a9cd60376ee561a316ba6314190760304eb56584de41a0ca',
            '93c6065976bd7587a9cd60376ee561a316ba6314190760304eb56584de41a0ca',
            '93c6065976bd7587a9cd60376ee561a316ba6314190760304eb56584de41a0ca',
            '93c6065976bd7587a9cd60376ee561a316ba6314190760304eb56584de41a0ca']],
        'anim/anim.img:attractors.ifp': ['d5c6b3be5f38e6158c0f522e9e31ad796853e9f1e0427e9929a529a398eeb43b', [
            '7c9088cee8f9494bb82a005aa26ae5f95939ce83d10fc65d029dfd0bf59f5d2e',
            '588cbd0be359fc37ba4c4c52baf957cda25eb5f0dee6679a100cd25679aa2a81',
            'fea8ea81d84fa23e6d26644c282d60ce54ec88fe62f4a2f257a308ce85540cc8',
            'fea8ea81d84fa23e6d26644c282d60ce54ec88fe62f4a2f257a308ce85540cc8',
            'fea8ea81d84fa23e6d26644c282d60ce54ec88fe62f4a2f257a308ce85540cc8',
            'fea8ea81d84fa23e6d26644c282d60ce54ec88fe62f4a2f257a308ce85540cc8']],
        'anim/anim.img:bar.ifp': ['b7c53808b00e5ffa747e700745fdd7587b18fdd53c691faeba615a97e843f862', [
            '88cc56a229046e065693ea861ce3e82276ad4ae7779fde5011fd5f9508a27cb1',
            '87a7091a81594f69e7b38783074d4826842a10b45166f0dcb5d1e9dd5a96dab1',
            'b52dcf7f3fb518408db38db889d4dcc595e41b051565f5170fa2c1d7087f3ce1',
            'b52dcf7f3fb518408db38db889d4dcc595e41b051565f5170fa2c1d7087f3ce1',
            'b52dcf7f3fb518408db38db889d4dcc595e41b051565f5170fa2c1d7087f3ce1',
            'b52dcf7f3fb518408db38db889d4dcc595e41b051565f5170fa2c1d7087f3ce1']]
    };
    const inputs = new Map([['sample.ifp',
        sample('csgizmo', 'PROLOG3', [{
            name: 'root',
            frames: [frame(-100, 75, 3, 0), frame(-120, 80, 3, 1)]
        }])]]);
    const ped = join(root, 'game/anim/ped.ifp');
    if (existsSync(ped)) inputs.set('anim/ped.ifp', await readFile(ped));
    const archivePath = join(root, 'game/anim/anim.img');
    if (existsSync(archivePath)) {
        const archive = new Blob([await readFile(archivePath)]);
        for (const {name, offset, size} of (await imgEntries(module, archive))
            .slice(0, 3)) {
            if (name.toLowerCase().endsWith('.ifp'))
                inputs.set(`anim/anim.img:${name}`,
                    Buffer.from(await archive.slice(offset, offset + size)
                        .arrayBuffer()));
        }
    }
    for (const [name, bytes] of inputs) {
        const expected = fixed[name];
        for (const [i, options] of variants.entries()) {
            const output = await convert(bytes, options, module);
            assert.equal(output.subarray(0, 4).toString(), 'DANM');
            if (expected && checksum(bytes) === expected[0])
                assert.equal(checksum(output), expected[1][i],
                    `${name} variant ${i}`);
        }
    }
});

test('all real cutscene IFP packages match frozen Python outputs', {
    skip: !wasm || !existsSync(join(root, 'game/anim/cuts'))
}, async () => {
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    const fixed = {
        'intro1a.ifp': ['92f0171e45436684a144f78eea5d18489a550e8cac287784e65d86eef48958c1',
            '92f37af9c35987385d61a7d24a490cb4b15180a622bca25d3cde612a71155125'],
        'intro1b.ifp': ['0ec75a28897b5fa1f7c768b3d195d5299a513d8e32669343aecc7d785269979d',
            'ea08ee7eab349c23bb2fd6b7a03b815f56a976e88100505fbdc0e42afefd6616'],
        'prolog1.ifp': ['e06cc1d000ddb8b7a2db757050b3b4736c11ee659d86d5827adb6a83f042b609',
            '4807bb6852b2ec77aab49f56f3da0bde4c595e2883f022e2b65977be9a912efb'],
        'prolog3.ifp': ['6982d8c11d9ec3b46c6505811294043b2c736bd6dea222837e6997cbd87cc251',
            '4b29d266a1731145cff10a910b1fbcd4228888252664a86f635f3ddb4287cf0a']
    };
    for (const [name, [source, golden]] of Object.entries(fixed)) {
        const path = join(root, 'game/anim/cuts', name);
        if (!existsSync(path)) continue;
        const bytes = await readFile(path);
        const output = await convert(bytes, {keep_every: 4,
            rigid_clips: ['csgizmo', 'cshandcuff', 'csmoneystck']}, module);
        assert.equal(output.subarray(0, 4).toString(), 'DANM');
        if (checksum(bytes) === source)
            assert.equal(checksum(output), golden, name);
    }
});
