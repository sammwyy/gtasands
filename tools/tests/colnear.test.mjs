import assert from 'node:assert/strict';
import {existsSync} from 'node:fs';
import {readFile} from 'node:fs/promises';
import {resolve} from 'node:path';
import test from 'node:test';
import {nearbyCollision} from '../colnear.mjs';

const world = resolve(import.meta.dirname, '../../nitrofs/data/world.dwld');

test('collision inspector reads version 6 world geometry', {
    skip: !existsSync(world)
}, async () => {
    const matches = nearbyCollision(await readFile(world),
        -1404.55, -2937.12, 46.44, 10);
    assert.ok(matches.some(entry => entry.name === 'cs_landbit_79' &&
        entry.triangles.length > 0));
});
