import {imgEntries} from './img_assets.mjs';
import {convertAsset} from './native_convert.mjs';
import {match, matchPath} from './params.mjs';
import {packDSPK} from './native_pack.mjs';

const types = {'.txd': 'texture', '.dff': 'model',
    '.col': 'collision', '.ifp': 'anim'};
const sections = ['models_loose', 'ped_ifp', 'cutscene_models',
    'cutscene_files', 'data', 'text', 'audio_config'];

export async function convertLooseAssets(module, planner, original, converted,
                                         progress = () => {}) {
    const existing = new Set(converted.map(entry => entry.path.toLowerCase()));
    const generated = [];
    for (const name of sections) {
        const section = planner.manifest.sections[name];
        if (!section || section.enabled === false) continue;
        if (section.pack && existing.has(section.pack.toLowerCase()))
            continue;
        const sources = new Map(original);
        const archive = section.from_img && original.get(section.from_img.toLowerCase());
        if (archive) {
            const suffixes = section.suffixes || ['.ifp', '.dat', '.cut'];
            for (const {name: entry, offset, size} of await imgEntries(module, archive)) {
                if (entry.includes('/') || entry.includes('\\') ||
                    !section.names.includes(entry.slice(0, entry.lastIndexOf('.')).toLowerCase()) ||
                    !suffixes.includes(entry.slice(entry.lastIndexOf('.')).toLowerCase()))
                    continue;
                const path = `${section.extract_to || 'anim/cuts'}/${entry.toLowerCase()}`;
                if (!sources.has(path) ||
                    sources.get(path).lastModified < archive.lastModified)
                    sources.set(path, archive.slice(offset, offset + size));
            }
        }
        const pack = section.pack ? new Map() : null;
        let processed = 0;
        for (const [path, file] of [...sources].sort(([a], [b]) =>
            a < b ? -1 : a > b ? 1 : 0)) {
            if (!matchPath(path, section.src)) continue;
            const output = `${section.out ? section.out + '/' : ''}` +
                (section.keep_paths === false ? path.split('/').at(-1) : path);
            if (!pack && existing.has(output)) continue;
            if (section.kind === 'copy') {
                if (match(path, section.remove || []) ||
                    match(path.split('/').at(-1), section.remove || []))
                    continue;
                generated.push({path: output, file});
            } else {
                const kind = types[path.slice(path.lastIndexOf('.'))] || 'copy';
                const {params, overrides, removed} = await planner.params(
                    name, kind === 'copy' ? 'texture' : kind, path);
                if (removed) continue;
                let bytes;
                try {
                    if (kind === 'copy') bytes = file;
                    else bytes = convertAsset(module, kind,
                        new Uint8Array(await file.arrayBuffer()), params, overrides);
                } catch (error) {
                    progress(`${path}: ${error.message}`);
                    continue;
                }
                if (pack) pack.set(output.split('/').at(-1), bytes);
                else generated.push({
                    path: output,
                    file: bytes instanceof Uint8Array ? new Blob([bytes]) : bytes
                });
            }
            if (++processed % 50 === 0)
                progress(`${name}: ${processed} archivos…`);
        }
        if (pack)
            generated.push({path: section.pack, file: packDSPK(module, pack)});
        progress(`${name}: ${processed} archivos procesados.`);
    }
    return generated;
}
