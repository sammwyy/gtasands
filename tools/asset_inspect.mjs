import {spawnSync} from 'node:child_process';
import {existsSync} from 'node:fs';
import {mkdir, mkdtemp, readFile, rmdir, stat, unlink, writeFile} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {basename, extname, isAbsolute, join, resolve} from 'node:path';
import {imgEntries} from '../web/img_assets.mjs';
import {convertTexture} from '../web/native_convert.mjs';
import {Parameters} from '../web/params.mjs';
import {writePNG} from './png.mjs';

const root = resolve(import.meta.dirname, '..');
const decoder = new TextDecoder('latin1');
const handlers = {'.txd': 'texture', '.dff': 'model',
    '.col': 'collision', '.ifp': 'anim'};
const keys = {texture: ['scale', 'max_size', 'format'],
    model: ['ratio', 'max_tris', 'normals'],
    collision: ['mode', 'face_ratio'], anim: ['keep_every']};

function string(bytes) {
    const zero = bytes.indexOf(0);
    return decoder.decode(bytes.subarray(0, zero < 0 ? bytes.length : zero));
}

async function sourceFile(path, DiskFile) {
    const info = await stat(path);
    if (!info.isFile()) throw new Error(`Not a file: ${path}`);
    return new DiskFile(path, info.size, info.mtimeMs);
}

async function loadSpec(module, source, spec, DiskFile) {
    const match = /^(.*\.img):([^/\\:]+)$/i.exec(spec);
    const path = match ? match[1] : spec;
    const absolute = isAbsolute(path) ? path : resolve(source, path);
    const file = await sourceFile(absolute, DiskFile);
    if (!match) {
        if (extname(path).toLowerCase() === '.img')
            return {name: basename(path), file};
        return {name: basename(path), data: new Uint8Array(await file.arrayBuffer())};
    }
    const entry = (await imgEntries(module, file)).find(item =>
        item.name.toLowerCase() === match[2].toLowerCase());
    if (!entry) throw new Error(`${match[2]} not found in ${path}`);
    return {name: entry.name, data: new Uint8Array(await file
        .slice(entry.offset, entry.offset + entry.size).arrayBuffer())};
}

export function inspectTextures(module, data) {
    const input = module._malloc(Math.max(1, data.length));
    const pointers = module._malloc(8);
    if (!input || !pointers) throw new Error('Out of memory inspecting TXD.');
    let items = 0;
    try {
        module.HEAPU8.set(data, input);
        if (!module._texture_inspect(input, data.length,
            pointers, pointers + 4)) throw new Error('Invalid TXD.');
        const view = new DataView(module.HEAPU8.buffer);
        items = view.getUint32(pointers, true);
        const count = view.getUint32(pointers + 4, true);
        return Array.from({length: count}, (_, i) => {
            const p = items + i * 38;
            return {name: string(module.HEAPU8.subarray(p, p + 33)),
                width: view.getUint16(p + 34, true),
                height: view.getUint16(p + 36, true)};
        });
    } finally {
        if (items) module._free(items);
        module._free(input);
        module._free(pointers);
    }
}

export function decodeDTEX(bytes) {
    const data = Buffer.from(bytes);
    if (data.length < 16 || data.toString('latin1', 0, 4) !== 'DTEX')
        throw new Error('Invalid DTEX.');
    const width = data.readUInt16LE(4), height = data.readUInt16LE(6);
    const format = data[8], flags = data[9], palettes = data.readUInt16LE(10);
    const textureSize = data.readUInt32LE(12);
    const paletteStart = 16 + Math.ceil(textureSize / 4) * 4;
    if (!width || !height || width * height > 1024 * 1024 ||
        ![1, 2, 3, 4, 6, 7].includes(format) ||
        paletteStart + palettes * 2 > data.length)
        throw new Error('Invalid DTEX dimensions or palette.');
    const pixels = Buffer.alloc(width * height * 4);
    const bitCount = format === 2 ? 2 : format === 3 ? 4 :
        format === 4 ? 8 : 0;
    const needed = format === 7 ? width * height * 2 :
        bitCount ? Math.ceil(width * height * bitCount / 8) : width * height;
    if (textureSize < needed) throw new Error('Truncated DTEX pixels.');
    for (let i = 0; i < width * height; i++) {
        let color, alpha = 255;
        if (format === 7) {
            color = data.readUInt16LE(16 + i * 2);
            alpha = color & 0x8000 ? 255 : 0;
        } else {
            const sample = bitCount ?
                (data[16 + (i * bitCount >> 3)] >> (i * bitCount & 7)) &
                    ((1 << bitCount) - 1) : data[16 + i];
            const indexBits = format === 1 ? 5 : format === 6 ? 3 : bitCount;
            const index = bitCount ? sample : sample & ((1 << indexBits) - 1);
            if (index >= palettes) throw new Error('Invalid DTEX palette index.');
            color = data.readUInt16LE(paletteStart + index * 2);
            if (!bitCount) alpha = Math.trunc(
                (sample >> indexBits) * 255 / ((1 << (8 - indexBits)) - 1));
            else if (index === 0 && flags & 1) alpha = 0;
        }
        const p = i * 4;
        pixels[p] = (color & 31) << 3;
        pixels[p + 1] = (color >> 5 & 31) << 3;
        pixels[p + 2] = (color >> 10 & 31) << 3;
        pixels[p + 3] = alpha;
    }
    return {width, height, pixels, format};
}

function inspectModel(module, data, name) {
    const input = module._malloc(data.length);
    const model = module._malloc(32);
    if (!input || !model) throw new Error('Out of memory inspecting DFF.');
    let parsed = false;
    try {
        module.HEAPU8.set(data, input);
        module.HEAPU8.fill(0, model, model + 32);
        parsed = !!module._dff_parse_model(input, data.length, model);
        if (!parsed) throw new Error('Invalid DFF.');
        const view = new DataView(module.HEAPU8.buffer);
        const frames = view.getUint32(model, true);
        const geometries = view.getUint32(model + 4, true);
        const atomics = view.getUint32(model + 8, true);
        const rows = [`${name}: ${frames} frames, ${geometries} geometries, ` +
            `${atomics} atomics`];
        const f = view.getUint32(model + 16, true);
        for (let i = 0; i < frames; i++) {
            const p = f + i * 232;
            rows.push(`  frame ${string(module.HEAPU8.subarray(p, p + 128))} ` +
                `parent=${view.getInt32(p + 224, true)} ` +
                `bone=${view.getInt32(p + 228, true)}`);
        }
        const g = view.getUint32(model + 20, true);
        for (let i = 0; i < geometries; i++) {
            const p = g + i * 80;
            rows.push(`  geom ${i}: ${view.getUint32(p + 4, true)} verts ` +
                `${view.getUint32(p + 8, true)} tris ` +
                `${view.getUint32(p + 12, true)} materials`);
        }
        return rows.join('\n');
    } finally {
        if (parsed) module._dff_free_model(model);
        module._free(input);
        module._free(model);
    }
}

function inspectCollision(module, data) {
    const input = module._malloc(data.length);
    const pointers = module._malloc(8);
    if (!input || !pointers) throw new Error('Out of memory inspecting COL.');
    let entries = 0, count = 0;
    try {
        module.HEAPU8.set(data, input);
        if (!module._worldcol_parse_archive(input, data.length,
            pointers, pointers + 4)) throw new Error('Invalid COL archive.');
        const view = new DataView(module.HEAPU8.buffer);
        entries = view.getUint32(pointers, true);
        count = view.getUint32(pointers + 4, true);
        return Array.from({length: count}, (_, i) => {
            const p = entries + i * 40;
            return `  COL ${string(module.HEAPU8.subarray(p, p + 24))} ` +
                `faces=${view.getUint32(p + 36, true)} ` +
                `radius=${view.getUint16(p + 32, true) / 128}`;
        }).join('\n');
    } finally {
        if (entries) module._worldcol_free_archive(entries, count);
        module._free(input);
        module._free(pointers);
    }
}

function inspectAnimation(data, name) {
    const bytes = Buffer.from(data);
    if (bytes.length < 36 || !['ANP3', 'ANPK'].includes(bytes.toString('latin1', 0, 4)))
        throw new Error('Invalid IFP.');
    const kind = bytes.toString('latin1', 0, 4);
    const count = bytes.readUInt32LE(kind === 'ANP3' ? 32 : 16);
    const block = string(bytes.subarray(kind === 'ANP3' ? 8 : 20, kind === 'ANP3' ? 32 : 44));
    return `${name}: block ${block}, ${count} animations`;
}

async function preview(module, planner, options, name, bytes) {
    if (extname(name).toLowerCase() !== '.txd')
        throw new Error('preview requires a TXD file or IMG texture entry.');
    const {params, overrides} = await planner.params(options.section, 'texture', name);
    const textures = inspectTextures(module, bytes);
    const selected = textures.map((item, index) => ({...item, index}))
        .filter(item => !options.texture ||
            item.name.toLowerCase() === options.texture.toLowerCase());
    if (!selected.length) throw new Error('Texture not found in TXD.');
    const built = spawnSync('make', ['-f', 'tools/portable/Makefile',
        'build/txd_cli'], {cwd: root, encoding: 'utf8'});
    if (built.status !== 0) throw new Error(built.stderr || 'C compiler unavailable.');
    const converted = Buffer.from(convertTexture(module, bytes, params, overrides));
    const temporary = await mkdtemp(join(tmpdir(), 'gtasads-preview-'));
    const input = join(temporary, 'source.txd');
    const created = [];
    try {
        await writeFile(input, bytes);
        await mkdir(options.output, {recursive: true});
        const rows = [];
        for (const {name: texture, index, width, height} of selected) {
            const raw = join(temporary, `${index}.rgba`);
            const run = spawnSync(join(root, 'build/txd_cli'),
                [input, String(index), '0', raw], {encoding: 'utf8'});
            if (run.status !== 0) throw new Error(run.stderr || `Cannot decode ${texture}`);
            created.push(raw);
            const original = await readFile(raw);
            if (original.length !== 4 + original.readUInt16LE(0) *
                original.readUInt16LE(2) * 4) throw new Error('Invalid decoded TXD.');
            const record = 8 + index * 40;
            const at = converted.readUInt32LE(record + 32);
            const size = converted.readUInt32LE(record + 36);
            if (at + size > converted.length) throw new Error('Invalid converted TXD.');
            const ds = decodeDTEX(converted.subarray(at, at + size));
            const safe = texture.replace(/[^a-z0-9_.-]/gi, '_');
            await writePNG(join(options.output, `${safe}_orig.png`),
                original.readUInt16LE(0), original.readUInt16LE(2),
                original.subarray(4));
            await writePNG(join(options.output, `${safe}_ds.png`),
                ds.width, ds.height, ds.pixels);
            rows.push(`${texture}: ${width}x${height} -> ` +
                `${ds.width}x${ds.height} fmt=${ds.format} (${size}B)`);
        }
        return `${rows.join('\n')}\npreviews in ${options.output}`;
    } finally {
        for (const file of created) await unlink(file);
        if (existsSync(input)) await unlink(input);
        await rmdir(temporary);
    }
}

export async function inspectAssets(options, {DiskFile, loadManifest, sourceFiles}) {
    const manifest = await loadManifest();
    const {default: createModule} = await import('../web/assets.mjs');
    const module = await createModule();
    const source = resolve(options.source);
    if (options.command === 'list') {
        const section = manifest.sections[options.spec];
        if (section?.kind !== 'img') throw new Error('list only supports IMG sections.');
        const original = await sourceFiles(source);
        const file = original.get(section.src.toLowerCase());
        if (!file) throw new Error(`Missing original IMG: ${section.src}`);
        const planner = new Parameters(manifest, original);
        const entries = (await imgEntries(module, file))
            .sort((a, b) => b.size - a.size).slice(0, options.count);
        const rows = [];
        for (const {name, size} of entries) {
            const kind = handlers[extname(name).toLowerCase()] || 'copy';
            const {params, removed} = await planner.params(options.spec,
                kind === 'copy' ? 'texture' : kind, name);
            rows.push(`${name.padEnd(26)} ${(size / 1024).toFixed(0)}KB  ` +
                `${kind}  ${removed ? 'REMOVED' :
                    keys[kind]?.map(key => `${key}=${params[key]}`).join(' ') || ''}`);
        }
        return rows.join('\n');
    }
    const {name, data, file} = await loadSpec(module, source, options.spec, DiskFile);
    if (options.command === 'preview') {
        const planner = new Parameters(manifest, await sourceFiles(source));
        return preview(module, planner, options, name, data);
    }
    if (file) {
        const entries = await imgEntries(module, file);
        const groups = new Map();
        for (const entry of entries) {
            const ext = extname(entry.name).toLowerCase();
            const group = groups.get(ext) || {count: 0, bytes: 0};
            group.count++;
            group.bytes += entry.size;
            groups.set(ext, group);
        }
        return `${name}: ${entries.length} entries\n` +
            [...groups].sort((a, b) => b[1].bytes - a[1].bytes)
                .map(([ext, group]) => `  ${ext} ${group.count} ` +
                    `${(group.bytes / 1048576).toFixed(1)}MB`).join('\n');
    }
    const ext = extname(name).toLowerCase();
    if (ext === '.txd') return inspectTextures(module, data)
        .map(item => `  ${item.name} ${item.width}x${item.height}`).join('\n');
    if (ext === '.dff') return inspectModel(module, data, name);
    if (ext === '.col') return inspectCollision(module, data);
    if (ext === '.ifp') return inspectAnimation(data, name);
    return `${name}: ${data.length} bytes`;
}
