import {convertAsset} from './native_convert.mjs';
import {packDSPK} from './native_pack.mjs';

const decoder = new TextDecoder('latin1');
const kinds = {'.txd': 'texture', '.dff': 'model',
    '.col': 'collision', '.ifp': 'anim'};

export async function imgEntries(module, file) {
    const head = new Uint8Array(await file.slice(0, 8).arrayBuffer());
    let source = 0, needed = 0, output = 0, count = 0;
    let index = 0, parsed = 0;
    try {
        source = module._malloc(8);
        needed = module._malloc(4);
        output = module._malloc(4);
        count = module._malloc(4);
        if (!source || !needed || !output || !count)
            throw new Error('No hay memoria para el índice IMG.');
        module.HEAPU8.set(head, source);
        if (!module._img_index_size(source, head.length, needed))
            throw new Error('Archivo IMG inválido.');
        const length = module.HEAPU32[needed >>> 2];
        if (length > file.size || length > 0xffffffff)
            throw new Error('Índice IMG incompleto.');
        index = module._malloc(length);
        if (!index) throw new Error('No hay memoria para el índice IMG.');
        module.HEAPU8.set(new Uint8Array(await file.slice(0, length).arrayBuffer()), index);
        if (!module._img_parse_index(index, length, output, count))
            throw new Error('No se pudo interpretar el índice IMG.');
        parsed = module.HEAPU32[output >>> 2];
        const entries = [];
        const view = new DataView(module.HEAPU8.buffer);
        const total = module.HEAPU32[count >>> 2];
        for (let i = 0; i < total; i++) {
            const entry = parsed + i * 48;
            const chars = module.HEAPU8.subarray(entry, entry + 25);
            const end = chars.indexOf(0);
            const name = decoder.decode(chars.subarray(0, end < 0 ? chars.length : end));
            const offset = view.getUint32(entry + 32, true) +
                view.getUint32(entry + 36, true) * 2 ** 32;
            const size = view.getUint32(entry + 40, true);
            if (offset + size > file.size)
                throw new Error(`IMG truncado: ${name}`);
            entries.push({name, offset, size});
        }
        return entries;
    } finally {
        if (parsed) module._img_free(parsed);
        if (index) module._free(index);
        if (source) module._free(source);
        if (needed) module._free(needed);
        if (output) module._free(output);
        if (count) module._free(count);
    }
}

export async function convertIMG(module, planner, section, source, progress = () => {},
                                 maxEntries = Infinity) {
    const files = new Map();
    const toc = (await imgEntries(module, source)).slice(0, maxEntries);
    let skipped = 0;
    for (const [index, {name, offset, size}] of toc.entries()) {
        const kind = kinds[name.slice(name.lastIndexOf('.')).toLowerCase()] ?? 'copy';
        const {params, overrides, removed} = await planner.params(section,
            kind === 'copy' ? 'texture' : kind, name);
        if (removed) continue;
        const bytes = new Uint8Array(await source.slice(offset, offset + size).arrayBuffer());
        try {
            files.set(name, convertAsset(module, kind, bytes, params, overrides));
        } catch (error) {
            skipped++;
            progress(`${name}: ${error.message}`);
        }
        if (index % 100 === 0)
            progress(`${section}: ${index + 1}/${toc.length} archivos…`);
    }
    if (!files.size && skipped)
        throw new Error(`No se pudo convertir ningún asset de ${section}.`);
    return {blob: packDSPK(module, files), files: files.size, skipped};
}
