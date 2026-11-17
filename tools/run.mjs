import {spawn} from 'node:child_process';
import {copyFile, lstat, mkdir, readFile, readdir, realpath,
        stat, symlink, utimes, writeFile} from 'node:fs/promises';
import {homedir} from 'node:os';
import {join, resolve} from 'node:path';
import {pathToFileURL} from 'node:url';

const root = resolve(import.meta.dirname, '..');
const firmware = join(root, 'build/emulator/firmware');

function setValue(source, section, key, value) {
    const lines = source.split('\n');
    const start = lines.findIndex(line => line.trim() === `[${section}]`);
    if (start < 0) return `${source}\n[${section}]\n${key} = ${value}\n`;
    let end = start + 1;
    while (end < lines.length && !lines[end].trim().startsWith('[')) end++;
    const existing = lines.findIndex((line, i) =>
        i > start && i < end && new RegExp(`^\\s*${key}\\s*=`).test(line));
    if (existing >= 0) lines[existing] = `${key} = ${value}`;
    else lines.splice(end, 0, `${key} = ${value}`);
    return lines.join('\n');
}

function configValue(source, section, key) {
    const start = source.indexOf(`[${section}]`);
    if (start < 0) return null;
    const next = source.slice(start + section.length + 2).search(/\n\s*\[/);
    const body = source.slice(start + section.length + 2,
        next < 0 ? undefined : start + section.length + 2 + next);
    const value = new RegExp(`^\\s*${key}\\s*=\\s*("[^"]*"|'[^']*')`, 'm').exec(body)?.[1];
    return value ? value.slice(1, -1) : null;
}

async function fileExists(path) {
    return stat(path).then(info => info.isFile(), () => false);
}

async function copyIfNewer(source, target) {
    const current = await stat(target).catch(error => {
        if (error.code !== 'ENOENT') throw error;
        return null;
    });
    const incoming = await stat(source);
    if (!current || current.size !== incoming.size || incoming.mtimeMs > current.mtimeMs) {
        await mkdir(resolve(target, '..'), {recursive: true});
        await copyFile(source, target);
        await utimes(target, incoming.atime, incoming.mtime);
    }
}

async function stageAssets(source, destination) {
    for (const entry of await readdir(source, {withFileTypes: true})) {
        const from = join(source, entry.name), to = join(destination, entry.name);
        if (entry.isDirectory()) {
            await mkdir(to, {recursive: true});
            await stageAssets(from, to);
        } else if (entry.isFile()) await copyIfNewer(from, to);
    }
}

export async function prepare(mode, storage, env = process.env) {
    if (!['ds', 'dsi'].includes(mode) || !['sd', 'rom'].includes(storage))
        throw new Error('Usage: node tools/run.mjs ds|dsi rom|sd [EMULATOR]');
    const extension = mode === 'dsi' ? 'dsi' : 'nds';
    let rom = resolve(env.GTASADS_ROM || join(root, 'build',
        `${storage === 'rom' ? 'fat-' : ''}gtasads.${extension}`));
    const config = resolve(env.MELONDS_CONFIG || join(homedir(), '.config/melonDS/melonDS.toml'));
    let settings = await readFile(config, 'utf8').catch(error => {
        if (error.code !== 'ENOENT') throw error;
        return '';
    });
    let nandSource, firmwareSource, dsFirmwareSource;
    if (mode === 'dsi') {
        const required = {
            BIOS9Path: 'biosdsi9.bin', BIOS7Path: 'biosdsi7.bin',
            FirmwarePath: 'dsifirmware.bin', NANDPath: 'dsinand.bin'
        };
        const missing = [];
        for (const [key, name] of Object.entries(required)) {
            const configured = configValue(settings, 'DSi', key);
            let path = configured && await fileExists(configured) ? configured : join(firmware, name);
            if (!await fileExists(path)) {
                missing.push(key);
                continue;
            }
            path = resolve(path);
            settings = setValue(settings, 'DSi', key, JSON.stringify(path));
            if (key === 'NANDPath') nandSource = path;
            if (key === 'FirmwarePath') firmwareSource = path;
        }
        if (missing.length)
            throw new Error(`DSi mode requires your own BIOS, firmware and NAND: ${missing.join(', ')}`);
    }
    const dsFiles = {BIOS9Path: 'biosnds9.bin', BIOS7Path: 'biosnds7.bin',
        FirmwarePath: 'dsfirmware.bin'};
    const externalDS = await Promise.all(Object.values(dsFiles)
        .map(name => fileExists(join(firmware, name))));
    if (externalDS.every(Boolean)) {
        for (const [key, name] of Object.entries(dsFiles))
            settings = setValue(settings, 'DS', key, JSON.stringify(join(firmware, name)));
        dsFirmwareSource = join(firmware, 'dsfirmware.bin');
    }
    const profile = join(root, 'build/emulator',
        `${mode}-${storage}${env.GTASADS_PROFILE || ''}`);
    await mkdir(join(profile, 'melonDS'), {recursive: true});
    if (nandSource) {
        const nand = join(profile, 'nand.bin');
        if (!await fileExists(nand)) await copyFile(nandSource, nand);
        settings = setValue(settings, 'DSi', 'NANDPath', JSON.stringify(nand));
    }
    if (firmwareSource) {
        const localFirmware = join(profile, 'firmware.bin');
        if (!await fileExists(localFirmware)) await copyFile(firmwareSource, localFirmware);
        settings = setValue(settings, 'DSi', 'FirmwarePath', JSON.stringify(localFirmware));
    }
    if (dsFirmwareSource) {
        const localFirmware = join(profile, 'dsfirmware.bin');
        if (!await fileExists(localFirmware)) await copyFile(dsFirmwareSource, localFirmware);
        settings = setValue(settings, 'DS', 'FirmwarePath', JSON.stringify(localFirmware));
    }
    const sdRoot = join(profile, 'sd-root');
    if (mode === 'dsi') await mkdir(sdRoot, {recursive: true});
    if (storage === 'sd')
        await stageAssets(resolve(env.GTASADS_ASSET_SOURCE ||
            join(root, 'build/gtasads')), join(sdRoot, 'gtasads'));
    settings = setValue(settings, 'Emu', 'ConsoleType', mode === 'dsi' ? '1' : '0');
    settings = setValue(settings, 'Emu', 'ExternalBIOSEnable',
        mode === 'dsi' || externalDS.every(Boolean) ? 'true' : 'false');
    settings = setValue(settings, 'DLDI', 'Enable', storage === 'sd' ? 'true' : 'false');
    settings = setValue(settings, 'DLDI', 'FolderSync', storage === 'sd' ? 'true' : 'false');
    settings = setValue(settings, 'DLDI', 'FolderPath', JSON.stringify(sdRoot));
    settings = setValue(settings, 'DLDI', 'ImagePath', JSON.stringify(join(profile, 'dldi.bin')));
    settings = setValue(settings, 'DSi.SD', 'Enable', mode === 'dsi' ? 'true' : 'false');
    settings = setValue(settings, 'DSi.SD', 'FolderSync', mode === 'dsi' ? 'true' : 'false');
    settings = setValue(settings, 'DSi.SD', 'FolderPath', JSON.stringify(sdRoot));
    settings = setValue(settings, 'DSi.SD', 'ImagePath', JSON.stringify(join(profile, 'dsisd.bin')));
    await writeFile(join(profile, 'melonDS/melonDS.toml'), settings);
    if (mode === 'dsi') {
        const alias = join(profile, 'gtasads.nds');
        const existing = await lstat(alias).catch(error => {
            if (error.code !== 'ENOENT') throw error;
            return null;
        });
        if (existing && (!existing.isSymbolicLink() ||
            await realpath(alias).catch(() => '') !== rom))
            throw new Error(`Existing DSi ROM alias points elsewhere: ${alias}`);
        if (!existing) await symlink(rom, alias);
        rom = alias;
    }
    return {profile, rom};
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
    try {
        const [mode, storage, emulator = 'melonds'] = process.argv.slice(2);
        const {profile, rom} = await prepare(mode, storage);
        const child = spawn(emulator, [rom], {
            env: {...process.env, XDG_CONFIG_HOME: profile},
            stdio: 'inherit', detached: true
        });
        const interrupt = () => {
            try { process.kill(-child.pid, 'SIGINT'); }
            catch (error) { if (error.code !== 'ESRCH') throw error; }
        };
        process.on('SIGINT', interrupt);
        process.on('SIGTERM', interrupt);
        const exit = await new Promise((done, reject) => {
            child.on('close', code => done(code));
            child.on('error', reject);
        });
        process.exitCode = exit || 0;
    } catch (error) {
        console.error(error.message);
        process.exitCode = 1;
    }
}
