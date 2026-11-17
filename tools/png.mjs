import {writeFile} from 'node:fs/promises';
import {deflateSync, crc32} from 'node:zlib';

function chunk(type, data) {
    const name = Buffer.from(type);
    const result = Buffer.alloc(12 + data.length);
    result.writeUInt32BE(data.length);
    name.copy(result, 4);
    data.copy(result, 8);
    result.writeUInt32BE(crc32(result.subarray(4, 8 + data.length)), 8 + data.length);
    return result;
}

export async function writePNG(path, width, height, pixels, channels = 4) {
    if (!Number.isSafeInteger(width) || !Number.isSafeInteger(height) ||
        width < 1 || height < 1 || ![1, 4].includes(channels) ||
        pixels.length !== width * height * channels)
        throw new Error('Invalid PNG dimensions or pixels.');
    const header = Buffer.alloc(13);
    header.writeUInt32BE(width);
    header.writeUInt32BE(height, 4);
    header[8] = 8;
    header[9] = channels === 4 ? 6 : 0;
    const rows = Buffer.alloc(height * (1 + width * channels));
    for (let y = 0; y < height; y++)
        Buffer.from(pixels.buffer, pixels.byteOffset + y * width * channels,
                    width * channels).copy(rows, y * (1 + width * channels) + 1);
    await writeFile(path, Buffer.concat([
        Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]),
        chunk('IHDR', header), chunk('IDAT', deflateSync(rows)),
        chunk('IEND', Buffer.alloc(0))
    ]));
}
