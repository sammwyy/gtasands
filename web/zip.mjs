const encoder = new TextEncoder();
const table = Uint32Array.from({length: 256}, (_, index) => {
    let value = index;
    for (let i = 0; i < 8; i++) value = value & 1 ? 0xedb88320 ^ (value >>> 1) : value >>> 1;
    return value >>> 0;
});

function crc32(value, chunk) {
    for (const byte of chunk) value = table[(value ^ byte) & 255] ^ (value >>> 8);
    return value >>> 0;
}

function validName(name) {
    return typeof name === 'string' && name.length > 0 && !name.includes('\\') &&
        !name.includes(':') && !name.includes('\0') &&
        !name.split('/').some(part => !part || part === '.' || part === '..');
}

export class ZipWriter {
    constructor(destination) {
        this.destination = destination;
        this.entries = [];
        this.offset = 0;
        this.finished = false;
    }

    async write(bytes) {
        if (this.offset + bytes.byteLength > 0xffffffff)
            throw new Error('El ZIP requiere ZIP64 y supera el límite de 4 GiB.');
        await this.destination.write(bytes);
        this.offset += bytes.byteLength;
    }

    async add(name, data) {
        if (this.finished || this.entries.length >= 65535 ||
            !validName(name) || this.entries.some(entry => entry.name === name))
            throw new Error('Nombre de archivo inválido o duplicado en el ZIP.');
        const label = encoder.encode(name);
        if (label.length > 65535) throw new Error('Nombre de archivo demasiado largo.');
        const offset = this.offset;
        const header = new Uint8Array(30 + label.length);
        const view = new DataView(header.buffer);
        view.setUint32(0, 0x04034b50, true);
        view.setUint16(4, 20, true);
        view.setUint16(6, 0x808, true);
        view.setUint16(8, 0, true);
        view.setUint16(12, 33, true);
        view.setUint16(26, label.length, true);
        header.set(label, 30);
        await this.write(header);
        let size = 0;
        let checksum = 0xffffffff;
        const source = data instanceof Uint8Array ? [data] :
            typeof data.stream === 'function' ? data.stream() : data;
        for await (const part of source) {
            const chunk = part instanceof Uint8Array ? part : new Uint8Array(part);
            if (size + chunk.byteLength > 0xffffffff)
                throw new Error('Un archivo supera el límite de 4 GiB.');
            size += chunk.byteLength;
            checksum = crc32(checksum, chunk);
            await this.write(chunk);
        }
        const crc = (checksum ^ 0xffffffff) >>> 0;
        const descriptor = new Uint8Array(16);
        const desc = new DataView(descriptor.buffer);
        desc.setUint32(0, 0x08074b50, true);
        desc.setUint32(4, crc, true);
        desc.setUint32(8, size, true);
        desc.setUint32(12, size, true);
        await this.write(descriptor);
        this.entries.push({name, label, offset, size, crc});
    }

    async close() {
        if (this.finished) throw new Error('El ZIP ya está cerrado.');
        const start = this.offset;
        if (this.entries.length > 65535) throw new Error('Demasiados archivos para ZIP32.');
        for (const entry of this.entries) {
            const header = new Uint8Array(46 + entry.label.length);
            const view = new DataView(header.buffer);
            view.setUint32(0, 0x02014b50, true);
            view.setUint16(4, 20, true);
            view.setUint16(6, 20, true);
            view.setUint16(8, 0x808, true);
            view.setUint16(10, 0, true);
            view.setUint16(14, 33, true);
            view.setUint32(16, entry.crc, true);
            view.setUint32(20, entry.size, true);
            view.setUint32(24, entry.size, true);
            view.setUint16(28, entry.label.length, true);
            view.setUint32(42, entry.offset, true);
            header.set(entry.label, 46);
            await this.write(header);
        }
        const directorySize = this.offset - start;
        const end = new Uint8Array(22);
        const view = new DataView(end.buffer);
        view.setUint32(0, 0x06054b50, true);
        view.setUint16(8, this.entries.length, true);
        view.setUint16(10, this.entries.length, true);
        view.setUint32(12, directorySize, true);
        view.setUint32(16, start, true);
        await this.write(end);
        this.finished = true;
        await this.destination.close();
    }
}
