import {directoryFiles, indexFiles, missingConversionFiles, selectedFiles} from './folder.mjs';
import {convertOriginalIMGs} from './local_imgs.mjs';
import {convertOriginalEntrances} from './local_entrances.mjs';
import {convertOriginalLoose} from './local_loose.mjs';
import {convertOriginalRadar} from './local_radar.mjs';
import {convertOriginalSound} from './local_sound.mjs';
import {convertOriginalMovies} from './local_movies.mjs';
import {convertOriginalPeds} from './local_peds.mjs';
import {convertOriginalVehicles} from './local_vehicles.mjs';
import {convertOriginalWorld} from './local_world.mjs';
import {packageConverted} from './package.mjs';
import {downloadROM, latestRelease} from './releases.mjs';

const releaseStatus = document.querySelector('#release');
const releaseROM = document.querySelector('#release-rom');
const changelog = document.querySelector('#changelog');
const folderStatus = document.querySelector('#folder-status');
const folderInput = document.querySelector('#folder');
const romInput = document.querySelector('#rom');
const variant = document.querySelector('#variant');
const convertedInput = document.querySelector('#converted');
const packageStatus = document.querySelector('#package-status');
const exportButton = document.querySelector('#export');
const progressPanel = document.querySelector('#progress-panel');
const progressBar = document.querySelector('#progress');
let files = null;
let release = null;
let converted = [];
let originalMissing = [];

function canExport() {
    return converted.length > 0 || !!(files && !originalMissing.length);
}

function report(message) {
    packageStatus.textContent = message;
    if (!progressPanel || !progressBar) return;
    progressPanel.hidden = false;
    const count = /(\d+)\/(\d+)\s+files/.exec(message);
    if (count) {
        progressBar.max = Number(count[2]);
        progressBar.value = Number(count[1]);
    } else {
        progressBar.removeAttribute('value');
    }
}

function updateReleaseLink() {
    const asset = release?.assets[variant.value];
    releaseROM.hidden = !asset;
    if (asset) {
        releaseROM.href = asset.url;
        releaseROM.textContent = `Download ${variant.value.toUpperCase()} ROM from GitHub`;
    }
}

latestRelease().then(value => {
    release = value;
    releaseStatus.textContent = value.tag ?
        `${value.tag} — ${Object.keys(value.assets).join(' / ') || 'no SD ROMs'}` :
        'No release published yet; you can select a local SD ROM.';
    changelog.textContent = value.notes;
    updateReleaseLink();
}).catch(error => {
    releaseStatus.textContent = error.message;
});
variant.addEventListener('change', updateReleaseLink);

async function select(entries) {
    if (progressPanel) progressPanel.hidden = true;
    folderStatus.textContent = 'Checking local files…';
    try {
        const scanned = [];
        for await (const entry of entries) scanned.push(entry);
        files = indexFiles(scanned);
        originalMissing = await missingConversionFiles(files);
        const missing = originalMissing;
        folderStatus.textContent = missing.length ?
            `Missing files: ${missing.join(', ')}` :
            `Installation found: ${files.size} files, initial checks passed.`;
        exportButton.disabled = !canExport();
        if (!missing.length && !converted.length)
            packageStatus.textContent = 'Ready to convert from the original installation.';
    } catch (error) {
        files = null;
        originalMissing = [];
        exportButton.disabled = !canExport();
        folderStatus.textContent = error.message;
    }
}

document.querySelector('#directory').addEventListener('click', async () => {
    if (!('showDirectoryPicker' in window)) {
        folderInput.click();
        return;
    }
    try {
        const directory = await window.showDirectoryPicker({mode: 'read'});
        await select(directoryFiles(directory));
    } catch (error) {
        if (error.name !== 'AbortError') folderStatus.textContent = error.message;
    }
});

folderInput.addEventListener('change', async () => {
    await select(selectedFiles(folderInput.files));
});

romInput.addEventListener('change', () => {
    const rom = romInput.files[0];
    if (!rom) return;
    const valid = rom.name.toLowerCase().endsWith(`.${variant.value}`) &&
        rom.size > 0 && rom.size <= 16 * 1024 * 1024;
    if (!valid) packageStatus.textContent = 'The local ROM must be an SD build for the selected version (16 MiB maximum).';
});

async function selectConverted(entries) {
    if (progressPanel) progressPanel.hidden = true;
    try {
        converted = [];
        for await (const entry of entries) converted.push(entry);
        exportButton.disabled = !canExport();
        packageStatus.textContent = converted.length ?
            `${converted.length} converted files selected.` :
            files && !originalMissing.length ?
                'Ready to convert from the original installation.' :
                'No converted assets selected.';
    } catch (error) {
        converted = [];
        exportButton.disabled = !canExport();
        packageStatus.textContent = error.message;
    }
}

document.querySelector('#converted-directory').addEventListener('click', async () => {
    if (!('showDirectoryPicker' in window)) {
        convertedInput.click();
        return;
    }
    try {
        const directory = await window.showDirectoryPicker({mode: 'read'});
        await selectConverted(directoryFiles(directory));
    } catch (error) {
        if (error.name !== 'AbortError') packageStatus.textContent = error.message;
    }
});

convertedInput.addEventListener('change', async () => {
    await selectConverted(selectedFiles(convertedInput.files));
});

exportButton.addEventListener('click', async () => {
    exportButton.disabled = true;
    report('Preparing download…');
    let destination;
    try {
        const mode = variant.value;
        const local = romInput.files[0];
        const original = files && !originalMissing.length ? files : null;
        const previous = converted;
        if (local && (!local.name.toLowerCase().endsWith(`.${mode}`) ||
                      !local.size || local.size > 16 * 1024 * 1024))
            throw new Error('The local ROM does not match the selected SD version.');
        if (!local && !release?.assets[mode]) {
            const manualOptions = document.querySelector('#manual-options');
            if (manualOptions) manualOptions.open = true;
            throw new Error('No ROM available in this release. Select a local SD ROM.');
        }
        if (files && originalMissing.length && !previous.length)
            throw new Error(`Missing original files: ${originalMissing.join(', ')}`);
        if (original && !('showSaveFilePicker' in window))
            throw new Error('To create a full ZIP from the original installation, use a browser that supports saving large files.');
        if ('showSaveFilePicker' in window) {
            const handle = await window.showSaveFilePicker({
                suggestedName: `gtasads-${mode}.zip`,
                types: [{description: 'ZIP archive', accept: {'application/zip': ['.zip']}}]
            });
            destination = await handle.createWritable();
        }
        const rom = local || await downloadROM(release.assets[mode]);
        const progress = report;
        const manifest = original ?
            await (await fetch('./resources.json')).json() : null;
        const sections = manifest?.sections;
        const generated = original ? [
            ...await convertOriginalIMGs(original, previous, manifest, progress),
            ...await convertOriginalWorld(original, previous, sections.world, progress),
            ...await convertOriginalPeds(original, previous, {
                bounds: sections.peds.bounds ?? sections.world.bounds
            }, progress),
            ...await convertOriginalVehicles(original, previous, progress),
            ...await convertOriginalEntrances(original, previous, progress),
            ...await convertOriginalLoose(original, previous, manifest, progress),
            ...await convertOriginalRadar(original, previous, sections.radar, progress),
            ...await convertOriginalSound(original, previous, manifest, progress),
            ...await convertOriginalMovies(original, previous, progress)
        ] : [];
        const selected = [...previous, ...generated];
        if (original) {
            const paths = new Set(selected.map(entry => entry.path.toLowerCase()));
            const required = [
                'models/gta3.dspk', 'models/gta_int.dspk',
                'models/player.dspk', 'anim/anim.dspk',
                'models/cutscene.dspk', 'models/radar.dsrt',
                'data/world.dwld', 'data/peds.dped',
                'data/vehicles.dveh', 'data/entry_exits.denx',
                'data/map_points.dpoi', 'anim/ped.ifp',
                'audio/sfx/banks.dsbl', 'text/american.gxt',
                'movies/logo.dvid', 'movies/gtatitles.dvid'
            ];
            for (const [source, target] of [
                ['audio/sfx/feet', 'audio/sfx/feet.dsfx'],
                ['audio/streams/aa', 'audio/streams/aa.dsst'],
                ['audio/streams/ambience', 'audio/streams/ambience.dsst'],
                ['audio/streams/cutscene', 'audio/streams/cutscene.dsst']
            ])
                if (original.has(source)) required.push(target);
            const missing = required.filter(path => !paths.has(path));
            if (missing.length)
                throw new Error(`Conversion did not produce: ${missing.join(', ')}`);
        }
        report('Preparing ROM and local ZIP…');
        if (!destination) {
            const size = selected.reduce((sum, entry) => sum + entry.file.size, 0);
            if (size > 128 * 1024 * 1024)
                throw new Error('This browser cannot write large ZIP files incrementally. Use a browser that supports saving large files.');
            const chunks = [];
            destination = {
                write: async bytes => chunks.push(bytes.slice()),
                close: async () => {
                    const url = URL.createObjectURL(new Blob(chunks, {type: 'application/zip'}));
                    const link = document.createElement('a');
                    link.href = url;
                    link.download = `gtasads-${mode}.zip`;
                    link.click();
                    setTimeout(() => URL.revokeObjectURL(url), 60000);
                }
            };
        }
        report('Writing ROM and converted assets…');
        await packageConverted({variant: mode, rom, files: selected, destination});
        destination = null;
        report('SD ZIP created locally.');
        if (progressBar) {
            progressBar.max = 1;
            progressBar.value = 1;
        }
    } catch (error) {
        if (destination?.abort) await destination.abort().catch(() => {});
        if (error.name !== 'AbortError') packageStatus.textContent = error.message;
        if (progressPanel) progressPanel.hidden = true;
    } finally {
        exportButton.disabled = !canExport();
    }
});
