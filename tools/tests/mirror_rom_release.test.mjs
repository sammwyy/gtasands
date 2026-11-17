import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {mkdtemp, readFile} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import test from 'node:test';
import {mirrorRelease} from '../mirror_rom_release.mjs';

test('only verified code-only ROMs are mirrored', async () => {
    const output = await mkdtemp(join(tmpdir(), 'gtasads-rom-test-'));
    const bytes = Buffer.from('code-only-rom');
    const asset = {
        name: 'gtasads.nds', id: 123, size: bytes.length,
        digest: `sha256:${createHash('sha256').update(bytes).digest('hex')}`,
        browser_download_url:
            'https://github.com/sammwyy/gtasands/releases/download/v1/gtasads.nds'
    };
    const path = join(output, '123/gtasads.nds');
    await assert.rejects(mirrorRelease({assets: [
        {...asset, digest: `sha256:${'0'.repeat(64)}`}
    ]}, output, async () => new Response(bytes)), /hash or size mismatch/);
    const paths = await mirrorRelease({assets: [
        {...asset, name: 'fat-gtasads.nds'}, asset
    ]}, output, async url => {
        assert.equal(url, asset.browser_download_url);
        return new Response(bytes);
    });
    assert.deepEqual(paths, [path]);
    assert.deepEqual(await readFile(path), bytes);
    assert.deepEqual(await mirrorRelease({assets: [asset]}, output,
        () => assert.fail('Verified asset should not be downloaded twice')), [path]);
});
