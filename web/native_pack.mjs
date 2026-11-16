const encoder = new TextEncoder();

export function packDSPK(module, files) {
    const entries = [...files];
    const allocations = [];
    const callbacks = [];
    const chunks = [];
    const malloc = size => {
        const address = module._malloc(Math.max(size, 1));
        if (!address) throw new Error('No hay memoria para el paquete DSPK.');
        allocations.push(address);
        return address;
    };
    try {
        const records = malloc(entries.length * 12);
        module.HEAPU8.fill(0, records, records + entries.length * 12);
        for (let i = 0; i < entries.length; i++) {
            const [name, data] = entries[i];
            if (data.length > 0xffffffff)
                throw new Error('Asset demasiado grande para DSPK.');
            const label = encoder.encode(name);
            const pointer = malloc(label.length + 1);
            module.HEAPU8.set(label, pointer);
            module.HEAPU8[pointer + label.length] = 0;
            const view = new DataView(module.HEAPU8.buffer);
            view.setUint32(records + i * 12, pointer, true);
            view.setUint32(records + i * 12 + 4, data.length, true);
            view.setUint32(records + i * 12 + 8, i + 1, true);
        }
        const read = module.addFunction((_ctx, source, offset, destination, length) => {
            const data = entries[source - 1]?.[1];
            if (!data || offset + length > data.length) return 0;
            module.HEAPU8.set(data.subarray(offset, offset + length), destination);
            return 1;
        }, 'iiiiii');
        callbacks.push(read);
        const write = module.addFunction((_ctx, source, length) => {
            chunks.push(module.HEAPU8.slice(source, source + length));
            return 1;
        }, 'iiii');
        callbacks.push(write);
        if (!module._dspk_write(records, entries.length, read, write, 0))
            throw new Error('No se pudo empaquetar DSPK.');
        return new Blob(chunks, {type: 'application/octet-stream'});
    } finally {
        for (const address of allocations) module._free(address);
        for (const callback of callbacks) module.removeFunction(callback);
    }
}
