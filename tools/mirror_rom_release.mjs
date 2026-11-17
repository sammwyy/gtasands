import {createHash} from 'node:crypto';
import {mkdir, readFile, writeFile} from 'node:fs/promises';
import {dirname, join, resolve} from 'node:path';
import {pathToFileURL} from 'node:url';

const root = resolve(import.meta.dirname, '..');
const endpoint = 'https://api.github.com/repos/sammwyy/gtasands/releases/latest';
const maximumROMSize = 16 * 1024 * 1024;

export async function mirrorRelease(release, directory, request = fetch) {
    const output = [];
    for (const asset of release.assets || []) {
        const {name, id, size, digest, browser_download_url: address} = asset;
        if (!['gtasads.nds', 'gtasads.dsi'].includes(name) ||
            !Number.isSafeInteger(id) || id <= 0 ||
            !Number.isSafeInteger(size) || size <= 0 || size > maximumROMSize ||
            !/^sha256:[0-9a-f]{64}$/.test(digest || '')) continue;
        let url;
        try {
            url = new URL(address);
        } catch {
            continue;
        }
        if (url.protocol !== 'https:' || url.hostname !== 'github.com' ||
            !url.pathname.startsWith('/sammwyy/gtasands/releases/download/'))
            continue;
        const path = join(directory, String(id), name);
        const expected = digest.slice(7);
        const existing = await readFile(path).catch(error => {
            if (error.code !== 'ENOENT') throw error;
            return null;
        });
        if (existing) {
            if (existing.length !== size ||
                createHash('sha256').update(existing).digest('hex') !== expected)
                throw new Error(`Existing ROM differs from the release: ${path}`);
        } else {
            const response = await request(url.href);
            if (!response.ok || !response.body)
                throw new Error(`Could not download release ROM: ${name}`);
            const chunks = [];
            let length = 0;
            for await (const chunk of response.body) {
                length += chunk.byteLength;
                if (length > maximumROMSize)
                    throw new Error(`Release ROM exceeds the size limit: ${name}`);
                chunks.push(chunk);
            }
            const bytes = Buffer.concat(chunks, length);
            if (length !== size ||
                createHash('sha256').update(bytes).digest('hex') !== expected)
                throw new Error(`Release ROM hash or size mismatch: ${name}`);
            await mkdir(dirname(path), {recursive: true});
            await writeFile(path, bytes, {flag: 'wx'});
        }
        output.push(path);
    }
    if (!output.length)
        throw new Error('Latest release has no verified code-only SD ROM assets.');
    return output;
}

export async function main(request = fetch) {
    const response = await request(endpoint, {
        headers: {Accept: 'application/vnd.github+json'}
    });
    if (response.status === 404) {
        console.log('No release yet; users can select a local code-only SD ROM.');
        return;
    }
    if (!response.ok) throw new Error(`Could not check the latest release (${response.status}).`);
    for (const path of await mirrorRelease(await response.json(), join(root, 'web/roms'), request))
        console.log(`Verified release ROM: ${path}`);
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href)
    main().catch(error => {
        console.error(error.message);
        process.exitCode = 1;
    });
