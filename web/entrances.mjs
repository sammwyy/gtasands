import {pathAt} from './world.mjs';

export function convertEntrances(module, files,
                                 readRange = (file, start, length) =>
                                     new Uint8Array(new FileReaderSync().readAsArrayBuffer(
                                         file.slice(start, start + length)))) {
    if (!files.has('data/gta.dat'))
        throw new Error('Falta data/gta.dat para las entradas.');
    const allocations = [];
    const errors = [];
    let callback, enex = 0, points = 0;
    const malloc = size => {
        const pointer = module._malloc(size);
        if (!pointer) throw new Error('No hay memoria para entradas.');
        allocations.push(pointer);
        return pointer;
    };
    try {
        callback = module.addFunction((_context, path, destination, length) => {
            const key = pathAt(module, path);
            const file = key && files.get(key);
            if (!file) return 0;
            try {
                const data = readRange(file, 0, file.size);
                if (data.length !== file.size || data.length > 0xffffffff) return 0;
                const address = module._malloc(Math.max(data.length, 1));
                if (!address) return 0;
                module.HEAPU8.set(data, address);
                module.HEAPU32[destination >>> 2] = address;
                module.HEAPU32[length >>> 2] = data.length;
                return 1;
            } catch (error) {
                errors.push(error);
                return 0;
            }
        }, 'iiiii');
        const settings = malloc(8), result = malloc(8), counts = malloc(12);
        const pointsResult = malloc(8);
        module.HEAPU8.fill(0, settings, settings + 8);
        module.HEAPU32[(settings + 4) >>> 2] = callback;
        if (!module._enexbuild_convert(settings, result, result + 4, counts))
            throw errors[0] ?? new Error('No se pudo convertir IPL a DENX.');
        enex = module.HEAPU32[result >>> 2];
        const length = module.HEAPU32[(result + 4) >>> 2];
        if (!module._points_from_denx(enex, length,
                                      pointsResult, pointsResult + 4))
            throw new Error('No se pudo generar el mapa DPOI.');
        points = module.HEAPU32[pointsResult >>> 2];
        const size = module.HEAPU32[(pointsResult + 4) >>> 2];
        return {
            enex: module.HEAPU8.slice(enex, enex + length),
            points: module.HEAPU8.slice(points, points + size),
            count: module.HEAPU32[counts >>> 2]
        };
    } finally {
        if (enex) module._free(enex);
        if (points) module._free(points);
        for (const address of allocations) module._free(address);
        if (callback !== undefined) module.removeFunction(callback);
    }
}
