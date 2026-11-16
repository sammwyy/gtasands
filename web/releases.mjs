const endpoint = 'https://api.github.com/repos/sammwyy/gtasands/releases/latest';
const maximumROMSize = 16 * 1024 * 1024;

export async function latestRelease(request = fetch) {
    const response = await request(endpoint, {
        headers: {Accept: 'application/vnd.github+json'}
    });
    if (response.status === 404) return {tag: null, notes: '', assets: {}};
    if (!response.ok) throw new Error(`No se pudo consultar la última release (${response.status}).`);
    const release = await response.json();
    const assets = {};
    for (const asset of release.assets || []) {
        if (!['gtasads.nds', 'gtasads.dsi'].includes(asset.name) ||
            !Number.isSafeInteger(asset.size) || asset.size <= 0 ||
            asset.size > maximumROMSize) continue;
        const link = new URL(asset.browser_download_url);
        if (link.protocol !== 'https:' || link.hostname !== 'github.com' ||
            !link.pathname.startsWith('/sammwyy/gtasands/releases/download/')) continue;
        const digest = /^sha256:[0-9a-f]{64}$/.test(asset.digest) ?
            asset.digest.slice(7) : null;
        const mirror = Number.isSafeInteger(asset.id) && asset.id > 0 && digest ?
            new URL(`./roms/${asset.id}/${asset.name}`, import.meta.url).href : null;
        assets[asset.name.slice(-3)] = {url: link.href, size: asset.size,
                                      digest, mirror};
    }
    return {tag: release.tag_name || null, notes: release.body || '', assets};
}

export async function downloadROM(asset, request = fetch) {
    if (!asset || asset.size > maximumROMSize || !asset.mirror || !asset.digest)
        throw new Error('La release no contiene una ROM SD válida.');
    const response = await request(asset.mirror);
    if (!response.ok || !response.body)
        throw new Error('La ROM aún no está disponible en esta web. Descárgala desde GitHub y selecciónala como ROM local.');
    const chunks = [];
    let size = 0;
    for await (const chunk of response.body) {
        size += chunk.byteLength;
        if (size > maximumROMSize)
            throw new Error('La ROM descargada supera el límite para una build SD.');
        chunks.push(chunk);
    }
    if (size !== asset.size)
        throw new Error('El tamaño de la ROM no coincide con la release.');
    const rom = new Uint8Array(size);
    let offset = 0;
    for (const chunk of chunks) {
        rom.set(chunk, offset);
        offset += chunk.byteLength;
    }
    const hash = [...new Uint8Array(await crypto.subtle.digest('SHA-256', rom))]
        .map(byte => byte.toString(16).padStart(2, '0')).join('');
    if (hash !== asset.digest)
        throw new Error('La ROM no coincide con el SHA-256 publicado en la release.');
    return rom;
}
