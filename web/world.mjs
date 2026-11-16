const text = new TextDecoder('latin1');

export function pathAt(module, address) {
    const bytes = module.HEAPU8;
    let end = address;
    while (bytes[end]) ++end;
    const name = text.decode(bytes.subarray(address, end))
        .replaceAll('\\', '/').toLowerCase();
    if (name.split('/').some(part => !part || part === '.' || part === '..' ||
                                 part.includes(':'))) return null;
    return name;
}

export function convertWorld(module, files, options = {},
                             readRange = (file, start, length) => new Uint8Array(
                                 new FileReaderSync().readAsArrayBuffer(
                                     file.slice(start, start + length)))) {
    const archives = [files.get('models/gta3.img'), files.get('models/gta_int.img')];
    if (archives.some(file => !file)) throw new Error('Faltan los archivos IMG del mundo.');
    const sector = options.sector ?? 50;
    const bounds = options.bounds;
    if (!Number.isInteger(sector) || sector < 1 || sector > 65535 ||
        (bounds && (bounds.length !== 4 || bounds.some(value =>
            !Number.isFinite(value)))))
        throw new Error('Límites o sectores del mundo inválidos.');
    const allocations = [];
    const callbacks = [];
    const errors = [];
    const malloc = size => {
        const pointer = module._malloc(Math.max(size, 1));
        if (!pointer) throw new Error('No hay memoria para el mundo.');
        allocations.push(pointer);
        return pointer;
    };
    let encoded = 0;
    try {
        const source = module.addFunction((_context, path, destination, length) => {
            const key = pathAt(module, path);
            const file = key && files.get(key);
            if (!file) return 0;
            try {
                const data = readRange(file, 0, file.size);
                if (data.length !== file.size || data.length > 0xffffffff)
                    return 0;
                const pointer = module._malloc(Math.max(data.length, 1));
                if (!pointer) return 0;
                module.HEAPU8.set(data, pointer);
                module.HEAPU32[destination >>> 2] = pointer;
                module.HEAPU32[length >>> 2] = data.length;
                return 1;
            } catch (error) {
                errors.push(error);
                return 0;
            }
        }, 'iiiii');
        callbacks.push(source);
        const range = module.addFunction((_context, pack, low, high, length, destination) => {
            const file = archives[pack];
            if (!file) return 0;
            try {
                const start = (high >>> 0) * 2 ** 32 + (low >>> 0);
                if (start + length > file.size) return 0;
                const data = readRange(file, start, length);
                if (data.length !== length) return 0;
                module.HEAPU8.set(data, destination);
                return 1;
            } catch (error) {
                errors.push(error);
                return 0;
            }
        }, 'iiiiiii');
        callbacks.push(range);
        const settings = malloc(56);
        module.HEAPU8.fill(0, settings, settings + 56);
        const view = new DataView(module.HEAPU8.buffer);
        view.setUint32(settings + 4, source, true);
        view.setUint32(settings + 8, range, true);
        for (let i = 0; i < 4; ++i)
            view.setFloat64(settings + 16 + i * 8, bounds?.[i] ?? 0, true);
        view.setUint16(settings + 48, sector, true);
        view.setUint8(settings + 50, bounds ? 1 : 0);
        view.setUint8(settings + 51, options.lods === false ? 0 : 1);
        const result = malloc(8), stats = malloc(48);
        const ok = module._worldbuild_convert(settings, result, result + 4, stats);
        if (errors.length) throw errors[0];
        if (!ok) throw new Error('No se pudo convertir el mundo a DWLD.');
        encoded = module.HEAPU32[result >>> 2];
        const size = module.HEAPU32[(result + 4) >>> 2];
        return {
            data: module.HEAPU8.slice(encoded, encoded + size),
            instances: module.HEAPU32[(stats + 8) >>> 2],
            models: module.HEAPU32[stats >>> 2]
        };
    } finally {
        if (encoded) module._free(encoded);
        for (const pointer of allocations) module._free(pointer);
        for (const callback of callbacks) module.removeFunction(callback);
    }
}
