import {createReadStream, openSync, readSync, closeSync} from 'node:fs';
import {createHash} from 'node:crypto';
import {copyFile, mkdir, open, readFile, readdir, stat, unlink, utimes, writeFile} from 'node:fs/promises';
import {join, relative, resolve, sep} from 'node:path';
import {pathToFileURL} from 'node:url';
import {convertEntrances} from '../web/entrances.mjs';
import {indexFiles} from '../web/folder.mjs';
import {convertIMG, imgEntries} from '../web/img_assets.mjs';
import {convertLooseAssets} from '../web/loose_assets.mjs';
import {convertMovie} from '../web/movies.mjs';
import {packageConverted} from '../web/package.mjs';
import {matchPath, Parameters} from '../web/params.mjs';
import {convertPeds} from '../web/peds.mjs';
import {convertRadar} from '../web/radar_assets.mjs';
import {convertSFX, convertStreams} from '../web/sound_assets.mjs';
import {convertVehicles} from '../web/vehicles.mjs';
import {convertWorld} from '../web/world.mjs';
import {serializeManifest} from './export_web_manifest.mjs';

const root = resolve(import.meta.dirname, '..');
const archives = ['gta3_img', 'gta_int_img', 'player_img', 'anim_img'];
const loose = ['models_loose', 'ped_ifp', 'cutscene_models',
    'cutscene_files', 'data', 'text', 'audio_config'];
const sections = [...archives, 'world', 'peds', 'vehicles', 'entry_exits',
    'map_points', 'radar', ...loose, 'audio_sfx', 'audio_streams', 'movies',
    'radio_streams'];

export async function loadManifest() {
    const generated = serializeManifest(await readFile(join(root, 'resources.toml'), 'utf8'));
    const path = join(root, 'web/resources.json');
    const current = await readFile(path, 'utf8').catch(error => {
        if (error.code !== 'ENOENT') throw error;
        return null;
    });
    if (current !== generated) await writeFile(path, generated);
    return JSON.parse(generated);
}

function validOutputPath(path) {
    return typeof path === 'string' && !path.startsWith('/') &&
        path.split('/').every(part => part && part !== '.' && part !== '..' &&
            !part.includes('\\') && !part.includes(':'));
}

function signature(section, manifest, files, wasm) {
    const rule = manifest.sections[section];
    const relevant = path => {
        if (archives.includes(section)) return path === rule.src.toLowerCase() ||
            path.startsWith('data/') && path.endsWith('.ide');
        if (['world', 'peds', 'vehicles', 'entry_exits', 'map_points'].includes(section))
            return path.startsWith('data/') ||
                ['world', 'peds'].includes(section) && path === 'models/gta3.img' ||
                section === 'world' && path === 'models/gta_int.img';
        if (section === 'radar') return path === rule.src.toLowerCase();
        if (loose.includes(section)) return matchPath(path, rule.src) ||
            path === rule.from_img?.toLowerCase();
        if (section === 'movies') return path.startsWith('movies/') &&
            path.endsWith('.mpg');
        if (section === 'audio_sfx') return path.startsWith('audio/config/') ||
            path.startsWith('audio/sfx/');
        return path.startsWith('audio/config/') ||
            path.startsWith('audio/radio_config/') ||
            path.startsWith('audio/streams/');
    };
    const inputs = [...files].filter(([path]) => relevant(path)).map(([path, file]) =>
        [path, file.size, file.lastModified]).sort(([a], [b]) => a.localeCompare(b));
    return createHash('sha256').update(JSON.stringify({
        rule, defaults: manifest.defaults, global: manifest.global,
        wasm, inputs
    })).digest('hex');
}

export class DiskFile {
    constructor(path, size, lastModified, offset = 0) {
        this.path = path;
        this.size = size;
        this.lastModified = lastModified;
        this.offset = offset;
    }

    slice(start = 0, end = this.size) {
        const first = Math.min(this.size, Math.max(0, start));
        const last = Math.min(this.size, Math.max(first, end));
        return new DiskFile(this.path, last - first, this.lastModified, this.offset + first);
    }

    async arrayBuffer() {
        const file = await open(this.path, 'r');
        try {
            const bytes = Buffer.allocUnsafe(this.size);
            let offset = 0;
            while (offset < bytes.length) {
                const {bytesRead} = await file.read(bytes, offset, bytes.length - offset,
                                                    this.offset + offset);
                if (!bytesRead) throw new Error(`Unexpected end of file: ${this.path}`);
                offset += bytesRead;
            }
            return bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
        } finally {
            await file.close();
        }
    }

    async text() {
        return new TextDecoder().decode(await this.arrayBuffer());
    }

    stream() {
        return this.size ?
            createReadStream(this.path, {start: this.offset,
                end: this.offset + this.size - 1}) :
            (async function* () {})();
    }
}

export async function sourceFiles(directory) {
    const entries = [];
    async function visit(folder) {
        for (const entry of await readdir(folder, {withFileTypes: true})) {
            const path = join(folder, entry.name);
            if (entry.isDirectory()) await visit(path);
            else if (entry.isFile() || entry.isSymbolicLink()) {
                const info = await stat(path).catch(error => {
                    if (error.code !== 'ENOENT') throw error;
                    return null;
                });
                if (info?.isFile())
                    entries.push({path: relative(directory, path).split(sep).join('/'),
                        file: new DiskFile(path, info.size, info.mtimeMs)});
            }
        }
    }
    await visit(directory);
    return indexFiles(entries);
}

const descriptors = new Map();
function readRange(file, start, length) {
    if (!Number.isSafeInteger(start) || start < 0 || length < 0 ||
        start + length > file.size) throw new Error(`Invalid file range: ${file.path}`);
    let fd = descriptors.get(file.path);
    if (fd === undefined) {
        fd = openSync(file.path, 'r');
        descriptors.set(file.path, fd);
    }
    const bytes = Buffer.allocUnsafe(length);
    let offset = 0;
    while (offset < length) {
        const count = readSync(fd, bytes, offset, length - offset,
                               file.offset + start + offset);
        if (!count) throw new Error(`Unexpected end of file: ${file.path}`);
        offset += count;
    }
    return bytes;
}

function parseOptions(args) {
    const command = ['build', 'sync', 'report', 'list', 'info', 'preview']
        .includes(args[0]) ? args.shift() : 'build';
    if (['list', 'info', 'preview'].includes(command)) {
        const options = {command, source: join(root, 'game'),
            output: join(root, 'build/preview'), spec: '', texture: '',
            section: 'gta3_img', count: 30};
        for (let i = 0; i < args.length; i++) {
            if (['--source', '--output', '--section', '-n'].includes(args[i]) &&
                args[i + 1]) {
                const key = args[i++];
                if (key === '-n') options.count = Number(args[i]);
                else if (key === '--section') options.section = args[i];
                else options[key.slice(2)] = resolve(args[i]);
            } else if (!args[i].startsWith('-') && !options.spec)
                options.spec = args[i];
            else if (command === 'preview' && !options.texture &&
                !args[i].startsWith('-')) options.texture = args[i];
            else throw new Error(`Unsupported ${command} argument: ${args[i]}`);
        }
        if (!options.spec || command === 'list' &&
            (!Number.isSafeInteger(options.count) || options.count < 1))
            throw new Error(`Usage: node tools/assets.mjs ${command} SPEC`);
        return options;
    }
    const options = {command, source: join(root, 'game'),
                     output: join(root, command === 'sync' ? 'game' : 'nitrofs'),
                     sections: []};
    let sourceProvided = false;
    for (let i = 0; i < args.length; i++) {
        if (['--source', '--output', '--section', '--rom', '--zip', '--budget-mb'].includes(args[i]) &&
            args[i + 1]) {
            const key = args[i++].slice(2);
            if (key === 'section') options.sections.push(args[i]);
            else if (key === 'budget-mb') options.budgetMB = Number(args[i]);
            else {
                options[key] = resolve(args[i]);
                if (key === 'source') sourceProvided = true;
            }
        } else if (['--package-only', '--force', '--prune'].includes(args[i])) {
            if (args[i] === '--force') options.force = true;
            else if (args[i] === '--prune') options.prune = true;
            else options.packageOnly = true;
        } else throw new Error('Usage: node tools/assets.mjs [--source GTA_FOLDER] [--output NITROFS_DIR] [--section NAME ...] [--rom SD_ROM --zip OUTPUT_ZIP]');
    }
    const unknown = options.sections.filter(name => !sections.includes(name));
    if (unknown.length) throw new Error(`Unknown asset section: ${unknown.join(', ')}`);
    if (!!options.rom !== !!options.zip)
        throw new Error('--rom and --zip must be supplied together.');
    if (options.budgetMB !== undefined &&
        (!Number.isFinite(options.budgetMB) || options.budgetMB <= 0))
        throw new Error('--budget-mb must be a positive number.');
    if (command === 'sync' && (!sourceProvided || options.rom))
        throw new Error('sync requires --source GTA_FOLDER and cannot package a ROM.');
    if (options.packageOnly && (!options.rom || options.sections.length))
        throw new Error('--package-only requires --rom and --zip, and cannot select sections.');
    return options;
}

async function printReport() {
    const report = JSON.parse(await readFile(join(root, 'build/assets_report.json'), 'utf8'));
    for (const [name, entry] of Object.entries(report.sections))
        console.log(`${name.padEnd(20)} ${String(entry.files).padStart(5)} files  ` +
            `${(entry.bytes / 1048576).toFixed(2)} MiB`);
    console.log(`Total: ${(report.total_bytes / 1048576).toFixed(1)} / ` +
        `${(report.budget_bytes / 1048576).toFixed(0)} MiB`);
}

export async function sync(options) {
    const original = await sourceFiles(resolve(options.source));
    const manifest = await loadManifest();
    const planner = new Parameters(manifest, original);
    const selected = options.sections?.length ?
        options.sections : Object.keys(manifest.sections).filter(name =>
            manifest.sections[name].enabled !== false &&
            !manifest.sections[name].sd_only);
    const want = new Set();
    const aliases = new Map();
    for (const name of selected) {
        const section = manifest.sections[name];
        if (['img', 'radar'].includes(section.kind)) want.add(section.src.toLowerCase());
        if (['world', 'peds', 'vehicles', 'entry_exits', 'points', 'copy', 'files', 'intro'].includes(section.kind)) {
            if (section.from_img && original.has(section.from_img.toLowerCase()))
                want.add(section.from_img.toLowerCase());
            const patterns = section.src;
            if (patterns)
                for (const path of original.keys())
                    if (matchPath(path, patterns) &&
                        !matchPath(path, section.remove || [])) want.add(path);
            if (['world', 'peds', 'vehicles', 'entry_exits', 'points'].includes(section.kind))
                for (const path of original.keys())
                    if (path.startsWith('data/') && /\.(dat|ide|ipl|zon|scm|cfg)$/.test(path))
                        want.add(path);
        }
        if (section.kind === 'world') {
            want.add('models/gta3.img');
            want.add('models/gta_int.img');
        }
        if (['sfx', 'streams'].includes(section.kind)) {
            for (const path of original.keys()) {
                if (path.startsWith('audio/config/') && path.endsWith('.dat')) {
                    want.add(path);
                    if (section.lookup_dir) {
                        const alternate = `audio/${section.lookup_dir.toLowerCase()}/` +
                            path.slice('audio/config/'.length);
                        if (!original.has(alternate)) aliases.set(alternate, path);
                    }
                }
                if (section.lookup_dir && path.startsWith(
                    `audio/${section.lookup_dir.toLowerCase()}/`) && path.endsWith('.dat'))
                    want.add(path);
                if (!path.startsWith(`audio/${section.kind === 'sfx' ? 'sfx' : 'streams'}/`))
                    continue;
                const filename = path.split('/').at(-1);
                const {removed} = await planner.params(name, 'audio', filename);
                if (!removed) want.add(path);
            }
        }
    }
    let total = 0;
    const staged = [...[...want].map(name => [name, name]), ...aliases]
        .sort(([a], [b]) => a.localeCompare(b));
    for (const [name, sourceName] of staged) {
        const source = original.get(sourceName);
        if (!source) throw new Error(`Missing original file: ${sourceName}`);
        const destination = resolve(options.output, name);
        const info = await stat(destination).catch(error => {
            if (error.code !== 'ENOENT') throw error;
            return null;
        });
        if (sourceName !== name && info) continue;
        if (info && info.size === source.size && info.mtimeMs >= source.lastModified)
            continue;
        await mkdir(resolve(destination, '..'), {recursive: true});
        await copyFile(source.path, destination);
        const date = new Date(source.lastModified);
        await utimes(destination, date, date);
        total += source.size;
        console.log(`copy ${name}`);
    }
    const extracted = selected.map(name => manifest.sections[name])
        .filter(section => section.from_img &&
            original.has(section.from_img.toLowerCase()));
    if (extracted.length) {
        const {default: createModule} = await import('../web/assets.mjs');
        const module = await createModule();
        for (const section of extracted) {
            const archive = original.get(section.from_img.toLowerCase());
            const suffixes = section.suffixes || ['.ifp', '.dat', '.cut'];
            for (const {name, offset, size} of await imgEntries(module, archive)) {
                const dot = name.lastIndexOf('.');
                if (dot < 0 || name.includes('/') || name.includes('\\') ||
                    name.includes(':') ||
                    !section.names.includes(name.slice(0, dot).toLowerCase()) ||
                    !suffixes.includes(name.slice(dot).toLowerCase())) continue;
                const path = `${section.extract_to || 'anim/cuts'}/${name.toLowerCase()}`;
                if (!validOutputPath(path) ||
                    (original.get(path)?.lastModified ?? 0) >= archive.lastModified)
                    continue;
                const destination = resolve(options.output, path);
                const info = await stat(destination).catch(error => {
                    if (error.code !== 'ENOENT') throw error;
                    return null;
                });
                if (info && info.mtimeMs >= archive.lastModified) continue;
                await mkdir(resolve(destination, '..'), {recursive: true});
                await writeFile(destination, Buffer.from(
                    await archive.slice(offset, offset + size).arrayBuffer()));
                const date = new Date(archive.lastModified);
                await utimes(destination, date, date);
                console.log(`extract ${name} from ${section.from_img}`);
            }
        }
    }
    console.log(`sync: ${staged.length} files, ${total} bytes copied`);
}

export async function build(options) {
    if (options.packageOnly) {
        const rootDir = resolve(options.output);
        const files = await sourceFiles(rootDir);
        const paths = [...files.keys()].filter(path =>
            path !== '.stamp' && !path.endsWith('.key'));
        await packageAssets(options.rom, options.zip, rootDir, paths);
        return;
    }
    const manifest = await loadManifest();
    const selected = options.sections?.length ? options.sections :
        sections.filter(name => manifest.sections[name]?.enabled !== false &&
            !manifest.sections[name]?.sd_only);
    const files = await sourceFiles(resolve(options.source));
    const planner = new Parameters(manifest, files);
    const wasmHash = createHash('sha256')
        .update(await readFile(join(root, 'web/assets.wasm'))).digest('hex');
    const {default: createModule} = await import('../web/assets.mjs');
    const module = await createModule();
    const progress = message => console.log(message);
    const output = resolve(options.output);
    const generated = [];
    const report = {};
    let currentSection;
    const cache = join(root, 'build/assets_cache_node',
        createHash('sha256').update(output).digest('hex').slice(0, 16));
    const emit = async (path, file) => {
        if (!validOutputPath(path))
            throw new Error(`Invalid output filename: ${path}`);
        const target = resolve(output, path);
        await mkdir(resolve(target, '..'), {recursive: true});
        await writeFile(target, file instanceof Uint8Array ? file :
            new Uint8Array(await file.arrayBuffer()));
        generated.push(path);
        const bytes = file.size ?? file.byteLength;
        const stats = report[currentSection] ||= {files: 0, bytes: 0};
        stats.files++;
        stats.bytes += bytes;
        progress(`${path}: ${bytes} bytes`);
    };
    try {
        for (const section of selected) {
            currentSection = section;
            const key = signature(section, manifest, files, wasmHash);
            const recordPath = join(cache, `${section}.json`);
            const previous = await readFile(recordPath, 'utf8')
                .then(value => JSON.parse(value), error => {
                    if (error.code !== 'ENOENT') throw error;
                    return null;
                }).catch(error => {
                    if (error instanceof SyntaxError) return null;
                    throw error;
                });
            if (!options.force && previous?.key === key &&
                Array.isArray(previous.files) && previous.files.every(({path, size}) =>
                    validOutputPath(path) && Number.isSafeInteger(size) && size >= 0) &&
                (await Promise.all(previous.files.map(async ({path, size}) =>
                    stat(join(output, path)).then(info =>
                        info.isFile() && info.size === size, () => false)))).every(Boolean)) {
                report[section] = {files: previous.files.length,
                    bytes: previous.files.reduce((sum, file) => sum + file.size, 0)};
                generated.push(...previous.files.map(file => file.path));
                progress(`${section}: cached (${report[section].files} files)`);
                continue;
            }
            const start = generated.length;
            if (archives.includes(section)) {
                const {src, out} = manifest.sections[section];
                const source = files.get(src.toLowerCase());
                if (!source) throw new Error(`Missing original IMG: ${src}`);
                const result = await convertIMG(module, planner, section, source, progress);
                if (result.skipped) throw new Error(`${section}: ${result.skipped} entries could not be converted`);
                await emit(out, result.blob);
            } else if (section === 'world') {
                const options = manifest.sections.world;
                await emit(options.out, convertWorld(module, files, options, readRange).data);
            } else if (section === 'peds') {
                const options = manifest.sections.peds;
                await emit(options.out, convertPeds(module, files,
                    {bounds: options.bounds ?? manifest.sections.world.bounds}, readRange).data);
            } else if (section === 'vehicles') {
                await emit(manifest.sections.vehicles.out,
                           convertVehicles(module, files, readRange).data);
            } else if (section === 'entry_exits' || section === 'map_points') {
                const result = convertEntrances(module, files, readRange);
                if (section === 'entry_exits')
                    await emit(manifest.sections.entry_exits.out, result.enex);
                else await emit(manifest.sections.map_points.out, result.points);
            } else if (section === 'radar') {
                const {src, out, grid} = manifest.sections.radar;
                const source = files.get(src.toLowerCase());
                if (!source) throw new Error(`Missing original radar IMG: ${src}`);
                await emit(out, (await convertRadar(module, source, grid, progress)).data);
            } else if (loose.includes(section)) {
                const view = {...manifest, sections: {[section]: manifest.sections[section]}};
                const generated = await convertLooseAssets(module,
                    new Parameters(view, files), files, [], progress);
                for (const entry of generated) await emit(entry.path, entry.file);
            } else if (section === 'audio_sfx' || section === 'audio_streams' ||
                       section === 'radio_streams') {
                const result = section === 'audio_sfx' ?
                    await convertSFX(module, planner, files, new Set(), progress) :
                    await convertStreams(module, planner, files, new Set(), progress,
                                         Infinity, section);
                for (const entry of result.files) await emit(entry.path, entry.file);
            } else if (section === 'movies') {
                for (const [source, target] of [
                    ['movies/logo.mpg', 'movies/logo.dvid'],
                    ['movies/gtatitles.mpg', 'movies/gtatitles.dvid']
                ]) {
                    const file = files.get(source);
                    if (!file) throw new Error(`Missing original movie: ${source}`);
                    const movie = convertMovie(module, new Uint8Array(await file.arrayBuffer()));
                    await emit(target, movie.data);
                }
            }
            const outputs = await Promise.all(generated.slice(start).map(async path => ({
                path, size: (await stat(join(output, path))).size
            })));
            const obsolete = (Array.isArray(previous?.files) ? previous.files : [])
                .filter(file => file && validOutputPath(file.path) &&
                    !outputs.some(output => output.path === file.path));
            if (obsolete.length) {
                if (options.prune)
                    for (const file of obsolete) await unlink(join(output, file.path));
                else progress(`${section}: ${obsolete.length} old outputs remain; use --prune to remove them.`);
            }
            await mkdir(cache, {recursive: true});
            await writeFile(recordPath, JSON.stringify({key, files: outputs}));
        }
        const total = Object.values(report).reduce((sum, value) => sum + value.bytes, 0);
        const budget = (options.budgetMB ?? 448) * 1024 * 1024;
        if (!options.sections?.length && total > budget)
            throw new Error(`Assets exceed the ${options.budgetMB ?? 448} MiB budget.`);
        if (options.zip) await packageAssets(options.rom, options.zip, output, generated);
        if (!options.sections?.length) {
            await mkdir(join(root, 'build'), {recursive: true});
            await writeFile(join(root, 'build/assets_report.json'),
                            JSON.stringify({sections: report, total_bytes: total,
                                            budget_bytes: budget}, null, 2));
            await writeFile(join(output, '.stamp'), '');
        }
        console.log(`Total: ${(total / 1048576).toFixed(1)} MiB` +
            (!options.sections?.length ? ` / ${(budget / 1048576).toFixed(0)} MiB budget` : ''));
    } finally {
        for (const fd of descriptors.values()) closeSync(fd);
        descriptors.clear();
    }
}

export async function packageAssets(romPath, zipPath, output, paths) {
    const extension = romPath.toLowerCase().endsWith('.dsi') ? 'dsi' : 'nds';
    if (!romPath.toLowerCase().endsWith(`.${extension}`))
        throw new Error('The SD ROM must end in .nds or .dsi.');
    const romStat = await stat(romPath);
    const rom = new DiskFile(romPath, romStat.size, romStat.mtimeMs);
    const files = await Promise.all(paths.map(async path => {
        const filePath = join(output, path);
        const info = await stat(filePath);
        return {path, file: new DiskFile(filePath, info.size, info.mtimeMs)};
    }));
    const destination = await open(zipPath, 'wx');
    const write = async bytes => {
        let offset = 0;
        while (offset < bytes.byteLength) {
            const {bytesWritten} = await destination.write(bytes, offset,
                bytes.byteLength - offset);
            if (!bytesWritten) throw new Error('Could not write ZIP.');
            offset += bytesWritten;
        }
    };
    let closed = false;
    const close = async () => {
        if (!closed) {
            closed = true;
            await destination.close();
        }
    };
    try {
        await packageConverted({
            variant: extension, rom, files,
            destination: {write, close, abort: close}
        });
        console.log(`Wrote ${zipPath}`);
    } catch (error) {
        await close();
        await unlink(zipPath);
        throw error;
    }
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
    try {
        const options = parseOptions(process.argv.slice(2));
        if (['list', 'info', 'preview'].includes(options.command)) {
            const {inspectAssets} = await import('./asset_inspect.mjs');
            console.log(await inspectAssets(options,
                {DiskFile, loadManifest, sourceFiles}));
        } else await (options.command === 'sync' ? sync(options) :
            options.command === 'report' ? printReport() : build(options));
    } catch (error) {
        console.error(error.message);
        process.exitCode = 1;
    }
}
