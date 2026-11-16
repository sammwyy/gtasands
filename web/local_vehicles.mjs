export async function convertOriginalVehicles(original, converted,
                                              progress = () => {},
                                              workerFactory = () => new Worker(
                                                  new URL('./vehicle_worker.mjs', import.meta.url),
                                                  {type: 'module'})) {
    if (converted.some(entry => entry.path.toLowerCase() === 'data/vehicles.dveh'))
        return [];
    progress('Convirtiendo vehículos DVEH en este navegador…');
    const worker = workerFactory();
    try {
        const result = await new Promise((resolve, reject) => {
            worker.onmessage = event => event.data.error ?
                reject(new Error(event.data.error)) : resolve(event.data);
            worker.onerror = event => reject(new Error(event.message));
            worker.postMessage({files: original});
        });
        progress(`Vehículos: ${result.models} modelos convertidos.`);
        return [{
            path: 'data/vehicles.dveh',
            file: new Blob([result.bytes], {type: 'application/octet-stream'})
        }];
    } finally {
        worker.terminate();
    }
}
