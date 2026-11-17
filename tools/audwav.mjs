import {readFile} from 'node:fs/promises';
import {resolve} from 'node:path';
import {pathToFileURL} from 'node:url';
import {writePNG} from './png.mjs';

export function readWave(bytes) {
    if (bytes.length < 12 || bytes.toString('ascii', 0, 4) !== 'RIFF' ||
        bytes.toString('ascii', 8, 12) !== 'WAVE')
        throw new Error('Not a WAV file.');
    let rate, format, left, right;
    for (let offset = 12; offset + 8 <= bytes.length;) {
        const length = bytes.readUInt32LE(offset + 4);
        if (offset + 8 + length > bytes.length) throw new Error('Truncated WAV chunk.');
        const name = bytes.toString('ascii', offset, offset + 4);
        if (name === 'fmt ' && length >= 16) {
            format = [bytes.readUInt16LE(offset + 8), bytes.readUInt16LE(offset + 10),
                bytes.readUInt16LE(offset + 22)];
            rate = bytes.readUInt32LE(offset + 12);
        }
        if (name === 'data') {
            if (length % 4) throw new Error('Truncated stereo samples.');
            left = new Int16Array(length / 4);
            right = new Int16Array(length / 4);
            for (let i = 0; i < left.length; i++) {
                left[i] = bytes.readInt16LE(offset + 8 + i * 4);
                right[i] = bytes.readInt16LE(offset + 10 + i * 4);
            }
        }
        offset += 8 + length + (length & 1);
    }
    if (!rate || format?.[0] !== 1 || format[1] !== 2 ||
        format[2] !== 16 || !left)
        throw new Error('Expected stereo PCM16 WAV data.');
    return {rate, left, right};
}

function level(peak) {
    return peak > 0 ? 20 * Math.log10(peak / 32768) : -120;
}

function measure(values) {
    let peak = 0, squares = 0, clipped = 0;
    for (const value of values) {
        peak = Math.max(peak, Math.abs(value));
        squares += value * value;
        if (Math.abs(value) >= 32767) clipped++;
    }
    return {peak: level(peak),
        rms: level(values.length ? Math.sqrt(squares / values.length) : 0), clipped};
}

async function waveform(path, left, right) {
    const width = 800, height = 200, half = height / 2;
    const pixels = Buffer.alloc(width * height, 0x10);
    for (const [values, top] of [[left, 0], [right, half]]) {
        for (let x = 0; x < width; x++) {
            const start = Math.floor(x * values.length / width);
            const end = Math.floor((x + 1) * values.length / width);
            let lo = 0, hi = 0;
            for (let i = start; i < end; i++) {
                lo = Math.min(lo, values[i]);
                hi = Math.max(hi, values[i]);
            }
            const y0 = top + half / 2 - Math.floor(hi * half / 2 / 32768);
            const y1 = top + half / 2 - Math.floor(lo * half / 2 / 32768);
            for (let y = Math.max(top, y0); y < Math.min(top + half, y1 + 1); y++)
                pixels[y * width + x] = 0xe0;
            pixels[(top + half / 2) * width + x] = 0x60;
        }
    }
    await writePNG(path, width, height, pixels, 1);
}

export async function analyzeWave(path, ms = 100, png) {
    const {rate, left, right} = readWave(await readFile(path));
    const results = [`${path}: ${left.length} samples @ ${rate} Hz ` +
        `(${(left.length / rate).toFixed(2)} s)`];
    for (const [name, values] of [['L', left], ['R', right]]) {
        const {peak, rms, clipped} = measure(values);
        results.push(`  ${name}: peak ${peak.toFixed(1)} dBFS  ` +
            `rms ${rms.toFixed(1)} dBFS  clipped ${clipped}`);
    }
    const window = Math.max(1, Math.floor(rate * ms / 1000));
    const levels = [];
    let previous, transients = 0;
    for (let i = 0; i + window <= left.length; i += window) {
        let squares = 0;
        for (let j = i; j < i + window; j++)
            squares += left[j] ** 2 + right[j] ** 2;
        const value = level(Math.sqrt(squares / (2 * window)));
        if (previous !== undefined && value - previous > 6) transients++;
        previous = value;
        levels.push(value.toFixed(0));
    }
    results.push(`  rms per ${ms} ms (L+R): ${levels.join(' ')}`);
    results.push(`  transients (+6 dB jumps): ${transients}`);
    if (png) {
        await waveform(png, left, right);
        results.push(`  waveform -> ${png}`);
    }
    return results.join('\n');
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
    try {
        const args = process.argv.slice(2);
        const msIndex = args.indexOf('--ms'), pngIndex = args.indexOf('--png');
        const ms = msIndex < 0 ? 100 : Number(args[msIndex + 1]);
        const png = pngIndex < 0 ? undefined : args[pngIndex + 1];
        const wav = args[0];
        if (!wav || !Number.isSafeInteger(ms) || ms < 1 ||
            msIndex >= 0 && !args[msIndex + 1] ||
            pngIndex >= 0 && !png)
            throw new Error('Usage: node tools/audwav.mjs INPUT.wav [--ms N] [--png OUTPUT.png]');
        console.log(await analyzeWave(wav, ms, png));
    } catch (error) {
        console.error(error.message);
        process.exitCode = 1;
    }
}
