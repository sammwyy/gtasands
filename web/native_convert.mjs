import {match} from './params.mjs';

const decoder = new TextDecoder('latin1');
const encoder = new TextEncoder();
const formats = {auto: 0, a3i5: 1, pal4: 2, pal16: 3,
    pal256: 4, a5i3: 6, direct: 7};
const decimators = {cluster: 0, qem: 1, wedge: 2};

function withNativeBytes(module, data, convert) {
    const source = module._malloc(Math.max(data.byteLength, 1));
    const result = module._malloc(8);
    if (!source || !result) {
        if (source) module._free(source);
        if (result) module._free(result);
        throw new Error('Out of memory while converting assets.');
    }
    let output = 0;
    try {
        module.HEAPU8.set(data, source);
        module.HEAPU8.fill(0, result, result + 8);
        if (!convert(source, result))
            throw new Error('Native asset conversion failed.');
        output = module.HEAPU32[result >>> 2];
        const size = module.HEAPU32[(result + 4) >>> 2];
        return module.HEAPU8.slice(output, output + size);
    } finally {
        if (output) module._free(output);
        module._free(source);
        module._free(result);
    }
}

function allocateStrings(module, values, pointers) {
    if (!values?.length) return 0;
    const array = module._malloc(values.length * 4);
    if (!array) throw new Error('Out of memory while applying asset rules.');
    pointers.push(array);
    for (let i = 0; i < values.length; i++) {
        const text = encoder.encode(values[i]);
        const address = module._malloc(text.length + 1);
        if (!address) throw new Error('Out of memory while applying asset rules.');
        pointers.push(address);
        module.HEAPU8.set(text, address);
        module.HEAPU8[address + text.length] = 0;
        module.HEAPU32[(array >>> 2) + i] = address;
    }
    return array;
}

export function convertModel(module, bytes, params) {
    if (!(params.decimate in decimators))
        throw new Error('Unsupported mesh simplification method.');
    return withNativeBytes(module, bytes, (source, result) => {
        const pointers = [];
        try {
            const dropped = allocateStrings(module, params.drop_frames, pointers);
            const options = module._malloc(52), before = module._malloc(4);
            const after = module._malloc(4);
            if (!options || !before || !after)
                throw new Error('Out of memory while converting models.');
            pointers.push(options, before, after);
            module.HEAPU8.fill(0, options, options + 52);
            const view = new DataView(module.HEAPU8.buffer);
            view.setFloat64(options, params.ratio, true);
            view.setFloat64(options + 8, params.min_area, true);
            view.setFloat64(options + 16, params.max_shift, true);
            view.setUint32(options + 24, params.min_tris, true);
            view.setUint32(options + 28, params.max_tris, true);
            for (const [i, key] of ['uvs', 'normals', 'vertex_colors', 'skin',
                'drop_damage', 'drop_vlo'].entries())
                view.setUint8(options + 32 + i, params[key] ? 1 : 0);
            view.setUint8(options + 38, decimators[params.decimate]);
            view.setUint8(options + 39, params.ground_qem ? 1 : 0);
            view.setUint8(options + 40, params.rigid_skin ? 1 : 0);
            view.setUint32(options + 44, dropped, true);
            view.setUint32(options + 48, params.drop_frames.length, true);
            return module._dmdl_convert(source, bytes.byteLength, options,
                                        result, result + 4, before, after);
        } finally {
            for (const address of pointers) module._free(address);
        }
    });
}

export function convertTexture(module, bytes, params, overrides = []) {
    return withNativeBytes(module, bytes, (source, result) => {
        let info = 0;
        const pointers = [];
        try {
            const infoPtr = module._malloc(4), countPtr = module._malloc(4);
            if (!infoPtr || !countPtr)
                throw new Error('Out of memory while converting textures.');
            pointers.push(infoPtr, countPtr);
            if (!module._texture_inspect(source, bytes.byteLength,
                                         infoPtr, countPtr))
                throw new Error('Invalid TXD file.');
            info = module.HEAPU32[infoPtr >>> 2];
            const count = module.HEAPU32[countPtr >>> 2];
            const targets = module._malloc(Math.max(16 * count, 1));
            if (!targets) throw new Error('Out of memory while converting textures.');
            pointers.push(targets);
            module.HEAPU8.fill(0, targets, targets + 16 * count);
            const view = new DataView(module.HEAPU8.buffer);
            for (let i = 0; i < count; ++i) {
                const address = info + i * 38;
                const raw = module.HEAPU8.subarray(address, address + 33);
                const name = decoder.decode(raw.subarray(0, raw.indexOf(0)));
                let settings = params;
                for (const [pattern, value] of overrides)
                    if (match(name, pattern))
                        settings = {...settings, ...value};
                const target = targets + i * 16;
                for (const [j, size] of [view.getUint16(address + 34, true),
                    view.getUint16(address + 36, true)].entries()) {
                    let scaled = Math.max(1, Math.trunc(size * settings.scale));
                    let power = 1;
                    while (power * 2 <= scaled) power *= 2;
                    view.setUint16(target + j * 2,
                        Math.max(settings.min_size,
                                 Math.min(settings.max_size, power)), true);
                }
                view.setInt32(target + 4, formats[settings.format], true);
                view.setUint8(target + 8, settings.alpha_threshold);
                view.setUint16(target + 10, settings.auto_colors, true);
                view.setUint8(target + 12, settings.opaque ? 1 : 0);
                view.setUint8(target + 13, settings.additive ? 1 : 0);
            }
            return module._texture_convert_txd(source, bytes.byteLength,
                                               targets, count, result, result + 4);
        } finally {
            if (info) module._free(info);
            for (const address of pointers) module._free(address);
        }
    });
}

export function convertCollision(module, bytes, params) {
    const mode = {full: 0, bounds: 1, simple: 2}[params.mode];
    if (mode === undefined) throw new Error('Unsupported collision mode.');
    return withNativeBytes(module, bytes, (source, result) => {
        const counts = module._malloc(8);
        if (!counts) throw new Error('Out of memory while converting collisions.');
        try {
            return module._col_convert(source, bytes.byteLength, mode,
                                       params.face_ratio, params.min_faces,
                                       result, result + 4, counts, counts + 4);
        } finally {
            module._free(counts);
        }
    });
}

export function convertAnimation(module, bytes, params) {
    return withNativeBytes(module, bytes, (source, result) => {
        const pointers = [];
        try {
            const removed = allocateStrings(module, params.remove, pointers);
            const clips = allocateStrings(module, params.rigid_clips, pointers);
            const options = module._malloc(28), counts = module._malloc(8);
            if (!options || !counts)
                throw new Error('Out of memory while converting animations.');
            pointers.push(options, counts);
            module.HEAPU8.fill(0, options, options + 28);
            const view = new DataView(module.HEAPU8.buffer);
            view.setUint32(options, params.keep_every, true);
            view.setUint32(options + 4, params.max_frames, true);
            view.setUint8(options + 8, params.drop_translation ? 1 : 0);
            view.setUint32(options + 12, removed, true);
            view.setUint32(options + 16, params.remove.length, true);
            view.setUint32(options + 20, clips, true);
            view.setUint32(options + 24, params.rigid_clips.length, true);
            return module._ifp_convert(source, bytes.byteLength, options,
                                       result, result + 4, counts, counts + 4);
        } finally {
            for (const address of pointers) module._free(address);
        }
    });
}

export function convertAsset(module, kind, bytes, params, overrides = []) {
    if (kind === 'texture') return convertTexture(module, bytes, params, overrides);
    if (kind === 'model') return convertModel(module, bytes, params);
    if (kind === 'collision') return convertCollision(module, bytes, params);
    if (kind === 'anim') return convertAnimation(module, bytes, params);
    return bytes;
}
