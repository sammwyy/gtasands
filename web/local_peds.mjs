export async function convertOriginalPeds(original, converted, options = {},
                                          progress = () => {},
                                          workerFactory = () => new Worker(
                                              new URL('./ped_worker.mjs', import.meta.url),
                                              {type: 'module'})) {
    if (converted.some(entry => entry.path.toLowerCase() === 'data/peds.dped'))
        return [];
    progress('Convirtiendo población y caminos DPED en este navegador…');
    const worker = workerFactory();
    try {
        const result = await new Promise((resolve, reject) => {
            worker.onmessage = event => event.data.error ?
                reject(new Error(event.data.error)) : resolve(event.data);
            worker.onerror = event => reject(new Error(event.message));
            worker.postMessage({files: original, options});
        });
        progress(`Población: ${result.models} modelos, ${result.nodes} nodos.`);
        return [{
            path: 'data/peds.dped',
            file: new Blob([result.bytes], {type: 'application/octet-stream'})
        }];
    } finally {
        worker.terminate();
    }
}
