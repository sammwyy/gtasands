import assert from 'node:assert/strict';
import {mkdtemp, readFile, writeFile} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import test from 'node:test';
import {inflateSync} from 'node:zlib';
import {convertShot} from '../dsshot.mjs';

test('DS screenshot converter preserves RGB15 and nearest-neighbor scaling', async () => {
    const directory = await mkdtemp(join(tmpdir(), 'gtasads-dsshot-test-'));
    const source = join(directory, 'shot.dscr'), destination = join(directory, 'shot.png');
    const capture = Buffer.alloc(12);
    capture.write('DSCR');
    capture.writeUInt16LE(2, 4);
    capture.writeUInt16LE(1, 6);
    capture.writeUInt16LE(31, 8);
    capture.writeUInt16LE(31 << 5, 10);
    await writeFile(source, capture);
    await convertShot(source, destination, 2);
    const png = await readFile(destination);
    assert.equal(png.readUInt32BE(16), 4);
    assert.equal(png.readUInt32BE(20), 2);
    const payload = inflateSync(png.subarray(41, 41 + png.readUInt32BE(33)));
    assert.deepEqual([...payload.subarray(0, 9)], [
        0, 255, 0, 0, 255, 255, 0, 0, 255
    ]);
    assert.deepEqual([...payload.subarray(9, 17)], [
        0, 255, 0, 255, 0, 255, 0, 255
    ]);
});
