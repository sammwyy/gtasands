import {pathAt} from './world.mjs';

export function convertVehicles(module, files,
                                readRange = (file, start, length) =>
                                    new Uint8Array(new FileReaderSync().readAsArrayBuffer(
                                        file.slice(start, start + length)))) {
    const required = ['data/vehicles.ide', 'data/handling.cfg',
        'data/carcols.dat', 'data/surface.dat', 'data/surfinfo.dat'];
    const missing = required.filter(path => !files.has(path));
    if (missing.length)
        throw new Error(`Missing vehicle files: ${missing.join(', ')}`);
    const allocations = [];
    const errors = [];
    let callback, encoded = 0;
    const malloc = size => {
        const pointer = module._malloc(size);
        if (!pointer) throw new Error('Out of memory while converting vehicles.');
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
        const settings = malloc(8), result = malloc(8), stats = malloc(24);
        module.HEAPU8.fill(0, settings, settings + 8);
        module.HEAPU32[(settings + 4) >>> 2] = callback;
        if (!module._vehbuild_convert(settings, result, result + 4, stats))
            throw errors[0] ?? new Error('Could not convert vehicles to DVEH.');
        encoded = module.HEAPU32[result >>> 2];
        const size = module.HEAPU32[(result + 4) >>> 2];
        return {
            data: module.HEAPU8.slice(encoded, encoded + size),
            models: module.HEAPU32[stats >>> 2]
        };
    } finally {
        if (encoded) module._free(encoded);
        for (const address of allocations) module._free(address);
        if (callback !== undefined) module.removeFunction(callback);
    }
}
