export async function convertOriginalEntrances(original, converted,
                                               progress = () => {},
                                               workerFactory = () => new Worker(
                                                   new URL('./entrance_worker.mjs', import.meta.url),
                                                   {type: 'module'})) {
    const existing = new Set(converted.map(entry => entry.path.toLowerCase()));
    const targets = [
        ['data/entry_exits.denx', 'denx'],
        ['data/map_points.dpoi', 'dpoi']
    ].filter(([path]) => !existing.has(path));
    if (!targets.length) return [];
    progress('Convirtiendo entradas y puntos de mapa en este navegador…');
    const worker = workerFactory();
    try {
        const result = await new Promise((resolve, reject) => {
            worker.onmessage = event => event.data.error ?
                reject(new Error(event.data.error)) : resolve(event.data);
            worker.onerror = event => reject(new Error(event.message));
            worker.postMessage({files: original});
        });
        progress(`${result.count} entradas convertidas.`);
        return targets.map(([path, key]) => ({
            path, file: new Blob([result[key]], {type: 'application/octet-stream'})
        }));
    } finally {
        worker.terminate();
    }
}
