import {pcm16FromAudioBuffer} from './audio_pcm.mjs';

const TRACK_HEADER = 0x1f84;
const FORMATS = {pcm8: 0, pcm16: 1, adpcm: 2};

async function decodeAtSourceRate(ogg, rate) {
    const context = new OfflineAudioContext(1, 1, rate);
    return context.decodeAudioData(ogg.buffer);
}

export function convertStreamNative(module, encrypted, offset, options) {
    if (!(encrypted instanceof Uint8Array) || encrypted.length <= TRACK_HEADER ||
        !Number.isSafeInteger(offset) || offset < 0 ||
        !Number.isInteger(options.sample_rate) || options.sample_rate < 1 ||
        !Object.hasOwn(FORMATS, options.format))
        throw new Error('Pista de audio o parámetros inválidos.');
    const input = module._malloc(encrypted.length);
    if (!input) throw new Error('No hay memoria para convertir el audio.');
    let pointers = 0, encoded = 0;
    try {
        module.HEAPU8.set(encrypted, input);
        pointers = module._malloc(8);
        if (!pointers) throw new Error('No hay memoria para convertir el audio.');
        if (!module._audio_stream_convert(
            input, encrypted.length, offset >>> 0, Math.floor(offset / 2 ** 32),
            options.sample_rate, FORMATS[options.format], options.seekable ? 1 : 0,
            options.volume ?? 1, options.max_seconds ?? 0,
            pointers, pointers + 4))
            throw new Error('No se pudo convertir Vorbis a DSND.');
        encoded = module.HEAPU32[pointers >>> 2];
        const length = module.HEAPU32[(pointers + 4) >>> 2];
        return module.HEAPU8.slice(encoded, encoded + length);
    } finally {
        if (encoded) module._free(encoded);
        if (pointers) module._free(pointers);
        module._free(input);
    }
}

export async function convertStream(module, encrypted, offset, options,
                                    decode = decodeAtSourceRate) {
    if (decode === decodeAtSourceRate && module._audio_stream_convert)
        return convertStreamNative(module, encrypted, offset, options);
    if (!(encrypted instanceof Uint8Array) || encrypted.length <= TRACK_HEADER ||
        !Number.isSafeInteger(offset) || offset < 0 ||
        !Number.isInteger(options.sample_rate) ||
        !Object.hasOwn(FORMATS, options.format))
        throw new Error('Pista de audio o parámetros inválidos.');
    const allocations = [];
    const malloc = size => {
        const pointer = module._malloc(Math.max(size, 1));
        if (!pointer) throw new Error('No hay memoria para convertir el audio.');
        allocations.push(pointer);
        return pointer;
    };
    let encoded = 0;
    try {
        const data = malloc(encrypted.byteLength);
        module.HEAPU8.set(encrypted, data);
        module._audio_decrypt_parts(data, encrypted.length, offset >>> 0,
                                    Math.floor(offset / 2 ** 32), data);
        const ogg = data + TRACK_HEADER;
        const size = encrypted.length - TRACK_HEADER;
        const meta = malloc(12);
        if (!module._ogg_read_stream_info(ogg, size, meta, meta + 4, meta + 8))
            throw new Error('La pista original no contiene Vorbis válido.');
        const rate = module.HEAPU32[meta >>> 2];
        const samples = module.HEAPU32[(meta + 4) >>> 2];
        const channels = module.HEAPU32[(meta + 8) >>> 2];
        if (!rate || !samples || ![1, 2].includes(channels))
            throw new Error('La pista Vorbis no tiene canales compatibles.');
        const decoded = await decode(module.HEAPU8.slice(ogg, ogg + size), rate);
        const pcm = pcm16FromAudioBuffer(decoded, samples);
        const input = malloc(pcm.byteLength);
        module.HEAP16.set(pcm, input >>> 1);
        const pointers = malloc(8);
        if (!module._dsnd_encode_simple(
            input, pcm.length, rate, options.sample_rate, FORMATS[options.format],
            options.seekable ? 1 : 0, options.volume ?? 1,
            options.max_seconds ?? 0, -1, 0, pointers, pointers + 4))
            throw new Error('No se pudo convertir PCM a DSND.');
        encoded = module.HEAPU32[pointers >>> 2];
        const length = module.HEAPU32[(pointers + 4) >>> 2];
        return module.HEAPU8.slice(encoded, encoded + length);
    } finally {
        if (encoded) module._free(encoded);
        for (const pointer of allocations) module._free(pointer);
    }
}
