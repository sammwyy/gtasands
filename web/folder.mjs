export const requiredFiles = [
    'models/gta3.img', 'models/gta_int.img', 'anim/anim.img',
    'data/gta.dat', 'text/american.gxt'
];

function normalize(path) {
    const parts = path.replaceAll('\\', '/').split('/');
    if (parts.some(part => !part || part === '.' || part === '..'))
        throw new Error('Ruta inválida dentro de la instalación.');
    return parts.join('/').toLowerCase();
}

export function indexFiles(entries) {
    const files = new Map();
    for (const {path, file} of entries) {
        const key = normalize(path);
        if (files.has(key))
            throw new Error(`Rutas ambiguas que solo difieren en mayúsculas: ${key}`);
        files.set(key, file);
    }
    return files;
}

export function missingFiles(files) {
    return requiredFiles.filter(path => !files.has(path));
}

export async function missingConversionFiles(files) {
    const required = [
        ...requiredFiles, 'models/player.img', 'anim/ped.ifp',
        'data/peds.ide', 'data/vehicles.ide', 'data/handling.cfg',
        'data/carcols.dat', 'data/surface.dat', 'data/surfinfo.dat',
        'audio/config/pakfiles.dat', 'audio/config/banklkup.dat',
        'audio/config/strmpaks.dat', 'audio/config/traklkup.dat',
        'movies/logo.mpg', 'movies/gtatitles.mpg'
    ];
    if (files.has('data/gta.dat')) {
        const gta = await files.get('data/gta.dat').text();
        for (const line of gta.split(/\r?\n/)) {
            const record = /^\s*(IDE|IPL)\s+([^\s#]+)/i.exec(line);
            if (record && (record[1].toUpperCase() === 'IDE' ||
                           record[2].toLowerCase().endsWith('.ipl')))
                required.push(record[2].replaceAll('\\', '/').toLowerCase());
        }
    }
    const streamPacks = files.get('audio/config/strmpaks.dat');
    if (streamPacks) {
        const bytes = new Uint8Array(await streamPacks.arrayBuffer());
        for (let offset = 0; offset + 16 <= bytes.length; offset += 16) {
            const name = new TextDecoder('latin1').decode(
                bytes.subarray(offset, offset + 16)).split('\0', 1)[0].toLowerCase();
            if (['aa', 'ambience', 'cutscene'].includes(name))
                required.push(`audio/streams/${name}`);
        }
    }
    return [...new Set(required)].filter(path => !files.has(path));
}

export async function* directoryFiles(directory, prefix = '') {
    for await (const [name, entry] of directory.entries()) {
        const path = prefix ? `${prefix}/${name}` : name;
        if (entry.kind === 'directory') yield* directoryFiles(entry, path);
        else if (entry.kind === 'file') yield {path, file: await entry.getFile()};
    }
}

export function selectedFiles(files) {
    return [...files].map(file => ({
        path: file.webkitRelativePath.split('/').slice(1).join('/'),
        file
    }));
}
