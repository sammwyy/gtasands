import {readFile} from 'node:fs/promises';
import {resolve} from 'node:path';
import {pathToFileURL} from 'node:url';

const root = resolve(import.meta.dirname, '..');

export function nearbyCollision(data, x, y, z, radius = 1) {
    if (data.subarray(0, 4).toString() !== 'DWLD' ||
        data.readUInt16LE(4) !== 6)
        throw new Error('Expected DWLD version 6.');
    const modelCount = data.readUInt16LE(16);
    const instanceCount = data.readUInt32LE(20);
    const modelsOffset = data.readUInt32LE(24);
    const instancesOffset = data.readUInt32LE(32);
    const collisionsOffset = data.readUInt32LE(36);
    const names = Array.from({length: modelCount}, (_, i) =>
        data.subarray(modelsOffset + i * 36, modelsOffset + i * 36 + 24)
            .toString('latin1').split('\0')[0]);
    const results = [];
    for (let i = 0; i < instanceCount; i++) {
        const at = instancesOffset + i * 40;
        const pos = Array.from({length: 3}, (_, j) =>
            data.readInt32LE(at + j * 4) / 4096);
        const rotation = Array.from({length: 9}, (_, j) =>
            data.readInt16LE(at + 12 + j * 2) / 4096);
        const model = data.readUInt16LE(at + 30);
        const index = collisionsOffset + model * 12;
        const offset = data.readUInt32LE(index);
        const distance = data.readUInt16LE(index + 8) / 16;
        if (!offset || Math.hypot(pos[0] - x, pos[1] - y) - distance > radius)
            continue;
        const spheres = data.readUInt16LE(offset + 12);
        const boxes = data.readUInt16LE(offset + 14);
        const nodes = data.readUInt16LE(offset + 16);
        const vertices = data.readUInt16LE(offset + 18);
        const faces = data.readUInt16LE(offset + 20);
        const boxOffset = offset + 24 + spheres * 12;
        const faceOffset = boxOffset + boxes * 16 + nodes * 16;
        const vertexOffset = faceOffset + faces * 16;
        const world = vertex => {
            const local = vertex.map(value => value / 128);
            return pos.map((value, axis) => Math.round((value +
                local[0] * rotation[axis] + local[1] * rotation[3 + axis] +
                local[2] * rotation[6 + axis]) * 100) / 100);
        };
        const near = points => {
            const deltaX = Math.max(Math.min(...points.map(point => point[0])) - x,
                0, x - Math.max(...points.map(point => point[0])));
            const deltaY = Math.max(Math.min(...points.map(point => point[1])) - y,
                0, y - Math.max(...points.map(point => point[1])));
            return Math.hypot(deltaX, deltaY) < radius &&
                Math.max(...points.map(point => point[2])) > z - 0.5 &&
                Math.min(...points.map(point => point[2])) < z + 2.5;
        };
        const triangles = [];
        for (let face = 0; face < faces; face++) {
            const f = faceOffset + face * 16;
            const triangle = Array.from({length: 3}, (_, j) => {
                const vertex = data.readUInt16LE(f + j * 2);
                if (vertex >= vertices) throw new Error('Invalid collision vertex.');
                return world(Array.from({length: 3}, (_, k) =>
                    data.readInt16LE(vertexOffset + vertex * 6 + k * 2)));
            });
            if (!near(triangle)) continue;
            const normal = Array.from({length: 3}, (_, j) =>
                data.readInt16LE(f + 8 + j * 2) / 4096);
            const nz = Math.round((normal[0] * rotation[2] +
                normal[1] * rotation[5] + normal[2] * rotation[8]) * 100) / 100;
            triangles.push({nz, points: triangle});
        }
        const nearbyBoxes = [];
        for (let box = 0; box < boxes; box++) {
            const b = boxOffset + box * 16;
            const points = [0, 6].map(start => world(
                Array.from({length: 3}, (_, j) => data.readInt16LE(b + start + j * 2))));
            if (near(points)) nearbyBoxes.push(points);
        }
        if (triangles.length || nearbyBoxes.length)
            results.push({name: names[model], position: pos,
                triangles, boxes: nearbyBoxes});
    }
    return results;
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
    try {
        const args = process.argv.slice(2);
        const flag = args.indexOf('--dwld');
        const file = flag < 0 ? resolve(root, 'nitrofs/data/world.dwld') :
            args.splice(flag, 2)[1];
        if (args.length < 3 || !args.slice(0, 3).every(value => Number.isFinite(Number(value))) ||
            args[3] !== undefined && !(Number(args[3]) > 0))
            throw new Error('Usage: node tools/colnear.mjs X Y Z [R] [--dwld FILE]');
        const [x, y, z] = args.map(Number);
        for (const entry of nearbyCollision(await readFile(file),
            x, y, z, Number(args[3] ?? 1))) {
            console.log(`${entry.name} pos ${entry.position.map(value =>
                value.toFixed(2)).join(',')} faces ${entry.triangles.length} ` +
                `boxes ${JSON.stringify(entry.boxes)}`);
            for (const face of entry.triangles.slice(0, 16))
                console.log(`   nz ${face.nz.toFixed(2)} ${JSON.stringify(face.points)}`);
        }
    } catch (error) {
        console.error(error.message);
        process.exitCode = 1;
    }
}
