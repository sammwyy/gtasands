import assert from 'node:assert/strict';
import {crc32} from 'node:zlib';
import test from 'node:test';
import {packageConverted} from '../../web/package.mjs';
import {ZipWriter} from '../../web/zip.mjs';

function destination() {
    const chunks = [];
    return {chunks, write: async bytes => chunks.push(Buffer.from(bytes)),
        close: async () => {}};
}

function entries(chunks) {
    const data = Buffer.concat(chunks);
    const count = data.readUInt16LE(data.length - 12);
    const directory = data.readUInt32LE(data.length - 6);
    const files = new Map();
    let offset = directory;
    for (let i = 0; i < count; i++) {
        assert.equal(data.readUInt32LE(offset), 0x02014b50);
        const size = data.readUInt32LE(offset + 24);
        const nameLength = data.readUInt16LE(offset + 28);
        const extraLength = data.readUInt16LE(offset + 30);
        const commentLength = data.readUInt16LE(offset + 32);
        const local = data.readUInt32LE(offset + 42);
        const name = data.subarray(offset + 46, offset + 46 + nameLength).toString();
        assert.equal(data.readUInt32LE(local), 0x04034b50);
        const start = local + 30 + data.readUInt16LE(local + 26) +
            data.readUInt16LE(local + 28);
        const bytes = data.subarray(start, start + size);
        assert.equal(crc32(bytes), data.readUInt32LE(offset + 16));
        assert.ok(!files.has(name));
        files.set(name, bytes);
        offset += 46 + nameLength + extraLength + commentLength;
    }
    assert.equal(offset, data.length - 22);
    return files;
}

test('packageConverted produces the SD layout and ignores build stamps', async () => {
    const sink = destination();
    const progress = [];
    sink.close = async () => assert.equal(progress.at(-1), 'ZIP: 6/7 files…');
    const files = ['models/gta3.dspk', 'models/gta_int.dspk',
        'data/world.dwld', 'data/vehicles.dveh', 'data/peds.dped',
        'data/map_points.dpoi'].map(path => ({path, file: new Blob([path])}));
    files.push({path: '.stamp', file: new Blob(['ignored'])});
    await packageConverted({variant: 'nds', rom: Uint8Array.of(1, 2, 3),
        files, destination: sink, progress: message => progress.push(message)});
    const archive = entries(sink.chunks);
    assert.deepEqual([...archive.get('gtasads.nds')], [1, 2, 3]);
    assert.equal(archive.get('gtasads/data/world.dwld').toString(), 'data/world.dwld');
    assert.ok(!archive.has('gtasads/.stamp'));
    assert.equal(archive.size, 7);
    assert.deepEqual(progress, Array.from({length: 7}, (_, i) =>
        `ZIP: ${i + 1}/7 files…`));
});

test('ZipWriter streams data, preserves UTF-8 and rejects unsafe names', async () => {
    const sink = destination(), writer = new ZipWriter(sink);
    await writer.add('gtasads.dsi', Buffer.from('DSI'));
    await writer.add('gtasads/data/assets.dspk', (async function* () {
        yield Uint8Array.of(0, 1, 2);
        yield Uint8Array.of(3, 4, 255);
    })());
    await writer.add('gtasads/data/á.dat', new Uint8Array());
    for (const name of ['../source.img', '/absolute', 'gtasads/../bad',
                        'gtasads/data/assets.dspk'])
        await assert.rejects(writer.add(name, new Uint8Array()), /Invalid or duplicate/);
    await writer.close();
    const archive = entries(sink.chunks);
    assert.deepEqual([...archive], [
        ['gtasads.dsi', Buffer.from('DSI')],
        ['gtasads/data/assets.dspk', Buffer.from([0, 1, 2, 3, 4, 255])],
        ['gtasads/data/á.dat', Buffer.alloc(0)]
    ]);
});
