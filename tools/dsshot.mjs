import {readFile} from 'node:fs/promises';
import {resolve, extname} from 'node:path';
import {pathToFileURL} from 'node:url';
import {writePNG} from './png.mjs';

export async function convertShot(source, target, scale = 1) {
    const data = await readFile(source);
    if (data.subarray(0, 4).toString() !== 'DSCR' || data.length < 8)
        throw new Error(`${source}: not a DSCR dump`);
    const width = data.readUInt16LE(4), height = data.readUInt16LE(6);
    if (!Number.isSafeInteger(scale) || scale < 1 ||
        !width || !height || data.length < 8 + width * height * 2 ||
        width * scale * height * scale > 16 * 1024 * 1024)
        throw new Error(`${source}: invalid dimensions or incomplete pixels`);
    const pixels = Buffer.alloc(width * scale * height * scale * 4);
    const lineWidth = width * scale;
    for (let y = 0; y < height; y++) {
        for (let x = 0; x < width; x++) {
            const color = data.readUInt16LE(8 + (y * width + x) * 2);
            const channels = [color & 31, color >> 5 & 31, color >> 10 & 31];
            for (let dy = 0; dy < scale; dy++)
                for (let dx = 0; dx < scale; dx++) {
                    const offset = ((y * scale + dy) * lineWidth + x * scale + dx) * 4;
                    for (let i = 0; i < 3; i++)
                        pixels[offset + i] = channels[i] << 3 | channels[i] >> 2;
                    pixels[offset + 3] = 255;
                }
        }
    }
    await writePNG(target, lineWidth, height * scale, pixels);
    console.log(`${source} -> ${target}`);
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
    try {
        const args = process.argv.slice(2);
        const index = args.indexOf('--scale');
        const scale = index < 0 ? 1 : Number(args.splice(index, 2)[1]);
        if (!args[0] || args.length > 2)
            throw new Error('Usage: node tools/dsshot.mjs INPUT.dscr [OUTPUT.png] [--scale N]');
        await convertShot(args[0], args[1] ||
            args[0].slice(0, -extname(args[0]).length) + '.png', scale);
    } catch (error) {
        console.error(error.message);
        process.exitCode = 1;
    }
}
