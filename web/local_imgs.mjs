const sections = ['gta3_img', 'gta_int_img', 'player_img', 'anim_img'];

export async function convertOriginalIMGs(original, converted, manifest,
                                          progress = () => {},
                                          workerFactory = () => new Worker(
                                              new URL('./img_worker.mjs', import.meta.url),
                                              {type: 'module'})) {
    const existing = new Set(converted.map(({path}) => path.toLowerCase()));
    const missing = sections.filter(name =>
        !existing.has(manifest.sections[name].out.toLowerCase()));
    if (!missing.length) return [];
    const absent = missing.filter(name =>
        !original.has(manifest.sections[name].src.toLowerCase()));
    if (absent.length)
        throw new Error(`Faltan IMG originales: ${absent.join(', ')}`);
    const worker = workerFactory();
    const generated = [];
    try {
        for (const section of missing) {
            progress(`Convirtiendo ${section} en este navegador…`);
            const result = await new Promise((resolve, reject) => {
                worker.onmessage = event => {
                    if (event.data.progress) progress(event.data.progress);
                    else if (event.data.error) reject(new Error(event.data.error));
                    else resolve(event.data);
                };
                worker.onerror = event => reject(new Error(event.message));
                worker.postMessage({files: original, manifest, section});
            });
            generated.push({path: result.path, file: result.file});
            progress(`${section}: ${result.entries} archivos convertidos` +
                     (result.skipped ? `, ${result.skipped} errores.` : '.'));
        }
        return generated;
    } finally {
        worker.terminate();
    }
}
