import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {existsSync} from 'node:fs';
import {mkdtemp, readFile, writeFile} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';
import test from 'node:test';
import {packDSPK} from '../../web/native_pack.mjs';

const root = resolve(import.meta.dirname, '../..');
const fixtures = [
    [],
    [['Z.TXD', Buffer.from('a')], ['lower.dff', Buffer.from('bb')],
        ['empty.col', Buffer.alloc(0)]],
    [['foo.dat', Uint8Array.from({length: 256 * 320}, (_, i) => i & 255)],
        ['FOO.DAT', Buffer.from('upper')], ['b.ifp', Uint8Array.of(0, 1, 2)]]
];

function hash(name) {
    let value = 0x811c9dc5;
    for (const char of name.toLowerCase())
        value = Math.imul(value ^ char.charCodeAt(0), 0x01000193) >>> 0;
    return value;
}

function packReference(entries) {
    const ordered = entries.map(([name, bytes]) => ({name, bytes, hash: hash(name)}))
        .sort((a, b) => a.hash - b.hash ||
            (a.name.toLowerCase() < b.name.toLowerCase() ? -1 :
             a.name.toLowerCase() > b.name.toLowerCase() ? 1 : 0));
    const names = Buffer.concat(ordered.map(item => Buffer.from(item.name + '\0')));
    const namesOffset = 16 + ordered.length * 16;
    const dataOffset = namesOffset + (names.length + 3 & ~3);
    const header = Buffer.alloc(dataOffset);
    header.write('DSPK');
    header.writeUInt16LE(1, 4);
    header.writeUInt32LE(ordered.length, 8);
    header.writeUInt32LE(namesOffset, 12);
    names.copy(header, namesOffset);
    let position = dataOffset, namePosition = 0;
    const chunks = [header];
    for (let i = 0; i < ordered.length; i++) {
        const {name, bytes, hash: value} = ordered[i];
        const padding = (4 - position % 4) % 4;
        if (padding) chunks.push(Buffer.alloc(padding));
        position += padding;
        const row = 16 + 16 * i;
        header.writeUInt32LE(value, row);
        header.writeUInt32LE(namePosition, row + 4);
        header.writeUInt32LE(position, row + 8);
        header.writeUInt32LE(bytes.length, row + 12);
        chunks.push(Buffer.from(bytes));
        position += bytes.length;
        namePosition += Buffer.byteLength(name) + 1;
    }
    return Buffer.concat(chunks);
}

test('WASM DSPK writer matches reference order, padding and binary contents', {
    skip: !existsSync(join(root, 'web/assets.mjs'))
}, async () => {
    const {default: createModule} = await import('../../web/assets.mjs');
    const module = await createModule();
    for (const entries of fixtures) {
        const actual = Buffer.from(await packDSPK(module, entries).arrayBuffer());
        assert.deepEqual(actual, packReference(entries));
    }
});

test('native DSPK CLI matches reference order, padding and binary contents', async t => {
    const build = spawnSync('make', ['-f', 'tools/portable/Makefile', 'build/dspk_cli'],
        {cwd: root, encoding: 'utf8'});
    if (build.status !== 0) return t.skip('C compiler unavailable');
    const work = await mkdtemp(join(tmpdir(), 'gtasads-pack-test-'));
    for (const entries of fixtures) {
        const args = [join(work, 'actual.dspk')];
        for (let i = 0; i < entries.length; i++) {
            const path = join(work, `entry${i}`);
            await writeFile(path, entries[i][1]);
            args.push(entries[i][0], path);
        }
        const run = spawnSync(join(root, 'build/dspk_cli'), args);
        assert.equal(run.status, 0, run.stderr?.toString());
        assert.deepEqual(await readFile(args[0]), packReference(entries));
    }
});
