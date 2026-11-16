export async function convertOriginalRadar(original, converted, options,
                                           progress = () => {},
                                           workerFactory = () => new Worker(
                                               new URL('./radar_worker.mjs', import.meta.url),
                                               {type: 'module'})) {
    const destination = options.out.toLowerCase();
    if (converted.some(entry => entry.path.toLowerCase() === destination))
        return [];
    const source = original.get(options.src.toLowerCase());
    if (!source) throw new Error(`Falta el IMG original para radar: ${options.src}`);
    progress('Convirtiendo tiles del radar en este navegador…');
    const worker = workerFactory();
    try {
        const result = await new Promise((resolve, reject) => {
            worker.onmessage = event => {
                if (event.data.progress) progress(event.data.progress);
                else if (event.data.error) reject(new Error(event.data.error));
                else resolve(event.data);
            };
            worker.onerror = event => reject(new Error(event.message));
            worker.postMessage({file: source, grid: options.grid || 12});
        });
        progress(`Radar: ${result.tiles} tiles convertidos.`);
        return [{
            path: destination,
            file: new Blob([result.bytes], {type: 'application/octet-stream'})
        }];
    } finally {
        worker.terminate();
    }
}
