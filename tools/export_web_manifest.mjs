import {readFile, writeFile} from 'node:fs/promises';
import {resolve} from 'node:path';
import {pathToFileURL} from 'node:url';

const root = resolve(import.meta.dirname, '..');
const defaults = {
    texture: {scale: 1, max_size: 256, min_size: 8, format: 'auto',
        auto_colors: 16, alpha_threshold: 24, opaque: false, additive: false},
    model: {ratio: 1, min_tris: 64, max_tris: 0, decimate: 'cluster',
        min_area: 0, max_shift: 0, ground_qem: false, uvs: true,
        normals: false, vertex_colors: true, skin: true, drop_damage: false,
        drop_vlo: true, drop_frames: [], rigid_skin: false},
    collision: {mode: 'full', face_ratio: 1, min_faces: 16},
    anim: {keep_every: 1, max_frames: 0, drop_translation: false,
        remove: [], rigid_clips: []},
    audio: {sample_rate: 11025, format: 'adpcm', max_seconds: 0, volume: 1}
};

function uncomment(line) {
    let quote = null, escaped = false;
    for (let i = 0; i < line.length; i++) {
        const char = line[i];
        if (quote) {
            if (quote === '"' && char === '\\' && !escaped) escaped = true;
            else if (char === quote && !escaped) quote = null;
            else escaped = false;
        } else if (char === '"' || char === "'") quote = char;
        else if (char === '#') return line.slice(0, i).trim();
    }
    return line.trim();
}

function split(value) {
    let quote = null, escaped = false, depth = 0, start = 0;
    const parts = [];
    for (let i = 0; i < value.length; i++) {
        const char = value[i];
        if (quote) {
            if (quote === '"' && char === '\\' && !escaped) escaped = true;
            else if (char === quote && !escaped) quote = null;
            else escaped = false;
        } else if (char === '"' || char === "'") quote = char;
        else if (char === '[' || char === '{') depth++;
        else if (char === ']' || char === '}') depth--;
        else if (char === ',' && depth === 0) {
            parts.push(value.slice(start, i).trim());
            start = i + 1;
        }
        if (depth < 0) throw new Error('Unbalanced TOML value.');
    }
    if (depth || quote) throw new Error('Unbalanced TOML value.');
    const last = value.slice(start).trim();
    if (last) parts.push(last);
    else if (value.length && value.at(-1) !== ',')
        throw new Error('Empty TOML value.');
    return parts;
}

function parseValue(value) {
    if (value.startsWith('"')) return JSON.parse(value);
    if (value.startsWith("'") && value.endsWith("'"))
        return value.slice(1, -1);
    if (value === 'true') return true;
    if (value === 'false') return false;
    if (/^[+-]?[0-9][0-9_.]*(?:[eE][+-]?[0-9]+)?$/.test(value)) {
        const number = Number(value.replaceAll('_', ''));
        if (Number.isFinite(number)) return number;
    }
    if (value.startsWith('[') && value.endsWith(']'))
        return split(value.slice(1, -1)).map(parseValue);
    if (value.startsWith('{') && value.endsWith('}')) {
        const result = {};
        for (const entry of split(value.slice(1, -1))) {
            const pair = /^([a-zA-Z][a-zA-Z0-9_-]*)\s*=\s*(.+)$/.exec(entry);
            if (!pair || Object.hasOwn(result, pair[1]))
                throw new Error('Invalid TOML inline table.');
            result[pair[1]] = parseValue(pair[2]);
        }
        return result;
    }
    throw new Error(`Unsupported TOML value: ${value}`);
}

export function parseResources(source) {
    const result = {};
    let table = result;
    for (const [index, raw] of source.split(/\r?\n/).entries()) {
        const line = uncomment(raw);
        if (!line) continue;
        try {
            const header = /^(\[\[|\[)([a-zA-Z][a-zA-Z0-9_.-]*)(\]\]|\])$/.exec(line);
            if (header) {
                if (header[1].length !== header[3].length)
                    throw new Error('Invalid TOML table header.');
                const path = header[2].split('.');
                table = result;
                for (const name of path.slice(0, -1)) {
                    table[name] ??= {};
                    if (!table[name] || typeof table[name] !== 'object' ||
                        Array.isArray(table[name]))
                        throw new Error('Invalid TOML table path.');
                    table = table[name];
                }
                const name = path.at(-1);
                if (header[1] === '[[') {
                    table[name] ??= [];
                    if (!Array.isArray(table[name])) throw new Error('Invalid TOML array table.');
                    table[name].push({});
                    table = table[name].at(-1);
                } else {
                    table[name] ??= {};
                    if (Array.isArray(table[name]) || typeof table[name] !== 'object')
                        throw new Error('Invalid TOML table.');
                    table = table[name];
                }
                continue;
            }
            const pair = /^([a-zA-Z][a-zA-Z0-9_-]*)\s*=\s*(.+)$/.exec(line);
            if (!pair || Object.hasOwn(table, pair[1]))
                throw new Error('Invalid TOML assignment.');
            table[pair[1]] = parseValue(pair[2].trim());
        } catch (error) {
            throw new Error(`resources.toml:${index + 1}: ${error.message}`);
        }
    }
    return result;
}

export function exportManifest(config) {
    const raw = parseResources(config);
    return {
        version: 1,
        global: Object.fromEntries(['texture_scale', 'model_ratio', 'audio_rate_scale']
            .map(name => [name, raw.global?.[name] ?? 1])),
        defaults: Object.fromEntries(Object.entries(defaults)
            .map(([name, value]) => [name, {...value, ...raw.defaults?.[name]}])),
        sections: Object.fromEntries(Object.entries(raw)
            .filter(([, value]) => value && typeof value === 'object' &&
                !Array.isArray(value) && Object.hasOwn(value, 'kind')))
    };
}

function sorted(value) {
    if (Array.isArray(value)) return value.map(sorted);
    if (value && typeof value === 'object')
        return Object.fromEntries(Object.keys(value).sort()
            .map(key => [key, sorted(value[key])]));
    return value;
}

export function serializeManifest(config) {
    return JSON.stringify(sorted(exportManifest(config)), null, 2)
        .replace(/("(?:volume|max_shift|min_area|audio_rate_scale|model_ratio|texture_scale|scale)": )(-?[0-9]+)(?=,?\n)/g,
            '$1$2.0') + '\n';
}

export async function exportFile(config = resolve(root, 'resources.toml'),
                                 output = resolve(root, 'web/resources.json')) {
    const generated = serializeManifest(await readFile(config, 'utf8'));
    await writeFile(output, generated);
    return generated;
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
    const argumentsList = process.argv.slice(2);
    let config = resolve(root, 'resources.toml');
    let output = resolve(root, 'web/resources.json');
    for (let i = 0; i < argumentsList.length; i++) {
        if (argumentsList[i] === '--config' && argumentsList[i + 1])
            config = resolve(argumentsList[++i]);
        else if (argumentsList[i] === '--output' && argumentsList[i + 1])
            output = resolve(argumentsList[++i]);
        else throw new Error(`Unknown option: ${argumentsList[i]}`);
    }
    await exportFile(config, output);
}
