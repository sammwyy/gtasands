import {convertStreamNative} from './streams.mjs';

const decoder = new TextDecoder('latin1');
const encoder = new TextEncoder();
const formats = {pcm8: 0, pcm16: 1, adpcm: 2};

export function audioNames(data, width) {
    const names = [];
    for (let offset = 0; offset + width <= data.length; offset += width) {
        const field = data.subarray(offset, offset + width);
        const end = field.indexOf(0);
        names.push(decoder.decode(end < 0 ? field : field.subarray(0, end)));
    }
    return names;
}

export function audioLookup(data) {
    const entries = [];
    const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
    for (let offset = 0; offset + 12 <= data.length; offset += 12)
        entries.push({
            pack: view.getUint8(offset),
            offset: view.getUint32(offset + 4, true),
            size: view.getUint32(offset + 8, true)
        });
    return entries;
}

function nativePack(module, entries, kind, names = [], seekable = false) {
    const pointers = [];
    const malloc = size => {
        const pointer = module._malloc(Math.max(size, 1));
        if (!pointer) throw new Error('No hay memoria para audio.');
        pointers.push(pointer);
        return pointer;
    };
    let output = 0;
    try {
        const result = malloc(8);
        const view = () => new DataView(module.HEAPU8.buffer);
        let success = false;
        if (kind === 'sfx') {
            const records = malloc(entries.length * 12);
            for (const [i, [bank, bytes]] of entries.entries()) {
                const pointer = malloc(bytes.length);
                module.HEAPU8.set(bytes, pointer);
                const memory = view();
                memory.setUint16(records + i * 12, bank, true);
                memory.setUint32(records + i * 12 + 4, pointer, true);
                memory.setUint32(records + i * 12 + 8, bytes.length, true);
            }
            success = module._dsfx_pack(records, entries.length,
                                        result, result + 4);
        } else if (kind === 'streams') {
            const records = malloc(entries.length * 12);
            for (const [i, [track, bytes]] of entries.entries()) {
                const pointer = malloc(bytes.length);
                module.HEAPU8.set(bytes, pointer);
                const memory = view();
                memory.setUint32(records + i * 12, track, true);
                memory.setUint32(records + i * 12 + 4, pointer, true);
                memory.setUint32(records + i * 12 + 8, bytes.length, true);
            }
            success = module._dsst_pack(records, entries.length,
                seekable ? 1 : 0, result, result + 4);
        } else {
            const pointersToNames = malloc(names.length * 4);
            for (const [i, name] of names.entries()) {
                const label = encoder.encode(name);
                const pointer = malloc(label.length + 1);
                module.HEAPU8.set(label, pointer);
                module.HEAPU8[pointer + label.length] = 0;
                module.HEAPU32[(pointersToNames >>> 2) + i] = pointer;
            }
            const lookups = malloc(entries.length * 4);
            module.HEAPU8.fill(0, lookups, lookups + entries.length * 4);
            for (const [i, [pack, index]] of entries.entries()) {
                const memory = view();
                memory.setUint8(lookups + i * 4, pack);
                memory.setUint16(lookups + i * 4 + 2, index, true);
            }
            success = module._dsbl_pack(pointersToNames, names.length,
                lookups, entries.length, result, result + 4);
        }
        if (!success) throw new Error(`No se pudo empaquetar ${kind}.`);
        output = module.HEAPU32[result >>> 2];
        const length = module.HEAPU32[(result + 4) >>> 2];
        return module.HEAPU8.slice(output, output + length);
    } finally {
        if (output) module._free(output);
        for (const pointer of pointers) module._free(pointer);
    }
}

function convertBank(module, data, params) {
    const input = module._malloc(data.length);
    const options = module._malloc(40), result = module._malloc(12);
    if (!input || !options || !result) {
        if (input) module._free(input);
        if (options) module._free(options);
        if (result) module._free(result);
        throw new Error('No hay memoria para el banco de sonidos.');
    }
    let encoded = 0;
    try {
        module.HEAPU8.set(data, input);
        module.HEAPU8.fill(0, options, options + 40);
        const memory = new DataView(module.HEAPU8.buffer);
        memory.setUint32(options + 4, params.sample_rate, true);
        memory.setUint8(options + 8, formats[params.format]);
        memory.setFloat64(options + 16, params.volume, true);
        memory.setFloat64(options + 24, params.max_seconds, true);
        memory.setInt32(options + 32, -1, true);
        if (!module._dsbk_convert(input, data.length, options,
                                  result, result + 4, result + 8))
            throw new Error('Banco SFX inválido.');
        encoded = module.HEAPU32[result >>> 2];
        const length = module.HEAPU32[(result + 4) >>> 2];
        return module.HEAPU8.slice(encoded, encoded + length);
    } finally {
        if (encoded) module._free(encoded);
        module._free(input);
        module._free(options);
        module._free(result);
    }
}

async function config(original, names, lookup) {
    const nameFile = original.get(`audio/config/${names.toLowerCase()}`);
    const lookupFile = original.get(`audio/config/${lookup.toLowerCase()}`);
    if (!nameFile || !lookupFile)
        throw new Error(`Falta audio/CONFIG/${!nameFile ? names : lookup}.`);
    return [
        new Uint8Array(await nameFile.arrayBuffer()),
        new Uint8Array(await lookupFile.arrayBuffer())
    ];
}

export async function convertSFX(module, planner, original, existing,
                                 progress = () => {}, maxBanks = Infinity) {
    const [namesData, lookupData] = await config(original, 'PakFiles.dat',
                                                'BankLkup.dat');
    const names = audioNames(namesData, 52);
    const entries = audioLookup(lookupData);
    const groups = new Map();
    const lookup = new Array(entries.length).fill(null).map(() => [255, 0]);
    let converted = 0;
    for (const [index, {pack, offset, size}] of entries.entries()) {
        if (index >= maxBanks) break;
        const name = names[pack];
        if (!name) continue;
        const {params, removed} = await planner.params('audio_sfx', 'audio', name);
        const file = original.get(`audio/sfx/${name.toLowerCase()}`);
        if (removed || !file) continue;
        try {
            const data = new Uint8Array(
                await file.slice(offset, offset + size + 0x12c4).arrayBuffer());
            const sound = convertBank(module, data, params);
            const group = groups.get(pack) || [];
            group.push([index, sound]);
            groups.set(pack, group);
            if (++converted % 50 === 0)
                progress(`SFX: ${converted} bancos convertidos…`);
        } catch (error) {
            progress(`${name}/${index}: ${error.message}`);
        }
    }
    const out = [];
    for (const [pack, items] of [...groups].sort(([a], [b]) => a - b)) {
        items.sort(([a], [b]) => a - b);
        items.forEach(([bank], i) => lookup[bank] = [pack, i]);
        const path = `audio/sfx/${names[pack].toLowerCase()}.dsfx`;
        if (!existing.has(path))
            out.push({path, file: new Blob([nativePack(module, items, 'sfx')])});
    }
    if (!existing.has('audio/sfx/banks.dsbl'))
        out.push({path: 'audio/sfx/banks.dsbl',
                  file: new Blob([nativePack(module, lookup, 'lookup', names)])});
    return {files: out, banks: converted};
}

async function alignedTrack(module, file, expected) {
    const start = Math.max(0, expected - 32768);
    const raw = new Uint8Array(await file.slice(
        start, expected + 32768 + 0x1f84 + 4).arrayBuffer());
    const input = module._malloc(raw.length), found = module._malloc(8);
    if (!input || !found) {
        if (input) module._free(input);
        if (found) module._free(found);
        throw new Error('No hay memoria para buscar la pista Vorbis.');
    }
    try {
        module.HEAPU8.set(raw, input);
        if (!module._audio_aligned_offset(input, raw.length, BigInt(start),
                                          BigInt(expected), found))
            throw new Error('No se encontró una pista Vorbis en CUTSCENE.');
        return Number(new DataView(module.HEAPU8.buffer).getBigUint64(found, true));
    } finally {
        module._free(input);
        module._free(found);
    }
}

export async function convertStreams(module, planner, original, existing,
                                     progress = () => {}, maxTracks = Infinity) {
    const [namesData, lookupData] = await config(original, 'StrmPaks.dat',
                                                'TrakLkup.dat');
    const names = audioNames(namesData, 16);
    const entries = audioLookup(lookupData);
    const groups = new Map();
    const processed = new Map();
    let converted = 0;
    for (const [index, {pack, offset, size}] of entries.entries()) {
        if (index >= maxTracks) break;
        const name = names[pack];
        if (!name) continue;
        const output = `audio/streams/${name.toLowerCase()}.dsst`;
        if (existing.has(output)) continue;
        const {params, removed} = await planner.params(
            'audio_streams', 'audio', name);
        const file = original.get(`audio/streams/${name.toLowerCase()}`);
        const count = processed.get(name) ?? 0;
        const limit = params.max_tracks ?? 0;
        if (removed || !file || (params.track_ids && !params.track_ids.includes(index)) ||
            (limit && count >= limit))
            continue;
        processed.set(name, count + 1);
        try {
            const start = name.toUpperCase() === 'CUTSCENE' ?
                await alignedTrack(module, file, offset) : offset;
            const data = new Uint8Array(await file.slice(
                start, start + size + 0x1f84).arrayBuffer());
            const sound = convertStreamNative(module, data, start, params);
            const group = groups.get(name) || [];
            group.push([index, sound]);
            groups.set(name, group);
            if (++converted % 10 === 0)
                progress(`Audio: ${converted} pistas convertidas…`);
        } catch (error) {
            progress(`${name}/${index}: ${error.message}`);
        }
    }
    const files = [];
    for (const [name, tracks] of [...groups].sort(([a], [b]) =>
        a < b ? -1 : a > b ? 1 : 0)) {
        const path = `audio/streams/${name.toLowerCase()}.dsst`;
        files.push({path, file: new Blob([nativePack(module, tracks, 'streams',
            [], !!planner.manifest.sections.audio_streams.audio?.seekable)])});
    }
    return {files, tracks: converted};
}
