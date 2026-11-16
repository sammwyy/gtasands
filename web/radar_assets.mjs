import {imgEntries} from './img_assets.mjs';

export async function convertRadar(module, file, grid = 12,
                                   progress = () => {}) {
    if (!Number.isInteger(grid) || grid < 1 || grid > 256)
        throw new Error('Invalid radar grid.');
    const count = grid * grid;
    const tiles = new Array(count).fill(null);
    const entries = await imgEntries(module, file);
    let converted = 0;
    for (const {name, offset, size} of entries) {
        const match = /^radar([0-9]+)\.txd$/i.exec(name);
        if (!match || Number(match[1]) >= count) continue;
        const data = new Uint8Array(await file.slice(offset, offset + size).arrayBuffer());
        const source = module._malloc(Math.max(data.length, 1));
        const result = module._malloc(8);
        if (!source || !result) {
            if (source) module._free(source);
            if (result) module._free(result);
        throw new Error('Out of memory while converting the radar.');
        }
        let output = 0;
        try {
            module.HEAPU8.set(data, source);
            if (!module._radar_convert(source, data.length, result, result + 4))
                continue;
            output = module.HEAPU32[result >>> 2];
            const length = module.HEAPU32[(result + 4) >>> 2];
            if (length !== 64 * 64 * 2 + 32 * 32 * 2)
            throw new Error(`Invalid radar tile: ${name}`);
            tiles[Number(match[1])] = module.HEAPU8.slice(output, output + length);
            if (++converted % 25 === 0)
            progress(`${converted} radar tiles converted…`);
        } finally {
            if (output) module._free(output);
            module._free(source);
            module._free(result);
        }
    }
    const fullSize = 64 * 64 * 2, mipSize = 32 * 32 * 2;
    const header = 24 + ((count + 3) & ~3);
    const mip = header + count * fullSize;
    const bytes = new Uint8Array(mip + count * mipSize);
    bytes.set(new TextEncoder().encode('DSRT'));
    const view = new DataView(bytes.buffer);
    for (const [i, value] of [1, count, grid, 64, 32, 0].entries())
        view.setUint16(4 + i * 2, value, true);
    view.setUint32(16, header, true);
    view.setUint32(20, mip, true);
    for (let i = 0; i < count; i++) {
        const tile = tiles[i];
        if (!tile) continue;
        bytes[24 + i] = 1;
        bytes.set(tile.subarray(0, fullSize), header + i * fullSize);
        bytes.set(tile.subarray(fullSize), mip + i * mipSize);
    }
    return {data: bytes, tiles: converted};
}
