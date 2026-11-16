import {pathAt} from './world.mjs';

export function convertPeds(module, files, options = {},
                            readRange = (file, start, length) => new Uint8Array(
                                new FileReaderSync().readAsArrayBuffer(
                                    file.slice(start, start + length)))) {
    const bounds = options.bounds ?? [-3000, -3000, 3000, 3000];
    if (bounds.length !== 4 || bounds.some(value => !Number.isFinite(value)))
        throw new Error('Invalid pedestrian bounds.');
    const mandatory = ['data/peds.ide', 'data/pedstats.dat', 'data/pedgrp.dat',
        'data/cargrp.dat', 'data/vehicles.ide', 'data/popcycle.dat',
        'data/animgrp.dat', 'data/info.zon'];
    const missing = mandatory.filter(path => !files.has(path));
    if (missing.length) throw new Error(`Missing original files: ${missing.join(', ')}`);
    const allocations = [];
    const errors = [];
    let callback, encoded = 0;
    const malloc = size => {
        const pointer = module._malloc(size);
        if (!pointer) throw new Error('Out of memory while converting pedestrians.');
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
        const settings = malloc(40);
        module.HEAPU8.fill(0, settings, settings + 40);
        const view = new DataView(module.HEAPU8.buffer);
        view.setUint32(settings + 4, callback, true);
        for (let i = 0; i < 4; ++i)
            view.setFloat64(settings + 8 + i * 8, bounds[i], true);
        const result = malloc(8), stats = malloc(52);
        const ok = module._pedbuild_convert(settings, result, result + 4, stats);
        if (errors.length) throw errors[0];
        if (!ok) throw new Error('Could not convert pedestrians to DPED.');
        encoded = module.HEAPU32[result >>> 2];
        const size = module.HEAPU32[(result + 4) >>> 2];
        return {
            data: module.HEAPU8.slice(encoded, encoded + size),
            models: module.HEAPU32[stats >>> 2],
            nodes: module.HEAPU32[(stats + 20) >>> 2]
        };
    } finally {
        if (encoded) module._free(encoded);
        for (const address of allocations) module._free(address);
        if (callback !== undefined) module.removeFunction(callback);
    }
}
