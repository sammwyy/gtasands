export function convertMovie(module, source, fps = 10, rate = 22050) {
    if (!(source instanceof Uint8Array) || !source.length ||
        !Number.isInteger(fps) || fps < 1 || fps > 65535 ||
        !Number.isInteger(rate) || rate < 1 || rate > 65535)
        throw new Error('Invalid MPEG video or conversion settings.');
    const allocations = [];
    const malloc = size => {
        const pointer = module._malloc(Math.max(size, 1));
    if (!pointer) throw new Error('Out of memory while converting the video.');
        allocations.push(pointer);
        return pointer;
    };
    let encoded = 0;
    try {
        const input = malloc(source.byteLength);
        module.HEAPU8.set(source, input);
        const pointers = malloc(12);
        if (!module._mpeg_movie_convert(input, source.byteLength, fps, rate,
                                        pointers, pointers + 4, pointers + 8))
        throw new Error('Could not convert MPEG to DVID.');
        encoded = module.HEAPU32[pointers >>> 2];
        const length = module.HEAPU32[(pointers + 4) >>> 2];
        const frames = module.HEAPU32[(pointers + 8) >>> 2];
        return {data: module.HEAPU8.slice(encoded, encoded + length), frames};
    } finally {
        if (encoded) module._free(encoded);
        for (const pointer of allocations) module._free(pointer);
    }
}
