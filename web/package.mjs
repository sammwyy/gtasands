import {ZipWriter} from './zip.mjs';

const required = [
    'models/gta3.dspk', 'models/gta_int.dspk',
    'data/world.dwld', 'data/vehicles.dveh', 'data/peds.dped'
];

export async function packageConverted({variant, rom, files, destination,
                                        progress = () => {}}) {
    try {
        if (!['nds', 'dsi'].includes(variant)) throw new Error('Unknown ROM version.');
        const romSize = rom instanceof Uint8Array ? rom.byteLength : rom?.size;
        if (!romSize || romSize > 16 * 1024 * 1024)
            throw new Error('The ROM must be a valid SD build (16 MiB maximum).');
        const paths = new Map();
        for (const {path, file} of files) {
            if (path === '.stamp' || path.endsWith('.key')) continue;
            const key = path.toLowerCase();
            if (paths.has(key)) throw new Error(`Duplicate converted file: ${path}`);
            paths.set(key, {path, file});
        }
        const missing = required.filter(path => !paths.has(path));
        if (missing.length)
            throw new Error(`Missing converted assets: ${missing.join(', ')}`);
        const archive = new ZipWriter(destination);
        const total = paths.size + 1;
        await archive.add(`gtasads.${variant}`, rom);
        progress(`ZIP: 1/${total} files…`);
        let written = 1;
        for (const {path, file} of [...paths.values()].sort((a, b) =>
            a.path < b.path ? -1 : a.path > b.path ? 1 : 0)) {
            await archive.add(`gtasads/${path}`, file);
            if (++written < total) progress(`ZIP: ${written}/${total} files…`);
        }
        await archive.close();
        progress(`ZIP: ${total}/${total} files…`);
    } catch (error) {
        if (destination.abort) await destination.abort();
        throw error;
    }
}
