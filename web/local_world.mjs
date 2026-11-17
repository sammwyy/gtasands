export async function convertOriginalWorld(original, converted, options = {},
                                           progress = () => {},
                                           workerFactory = () => new Worker(
                                               new URL('./world_worker.mjs', import.meta.url),
                                               {type: 'module'})) {
    if (converted.some(entry => entry.path.toLowerCase() === 'data/world.dwld'))
        return [];
    const required = ['data/gta.dat', 'models/gta3.img', 'models/gta_int.img'];
    const missing = required.filter(path => !original.has(path));
    if (missing.length) throw new Error(`Missing original files: ${missing.join(', ')}`);
    progress('Converting the world to DWLD…');
    const worker = workerFactory();
    try {
        const result = await new Promise((resolve, reject) => {
            worker.onmessage = event => event.data.error ?
                reject(new Error(event.data.error)) : resolve(event.data);
            worker.onerror = event => reject(new Error(event.message));
            worker.postMessage({files: original, options});
        });
        progress(`World: ${result.models} models, ${result.instances} instances.`);
        return [{
            path: 'data/world.dwld',
            file: new Blob([result.bytes], {type: 'application/octet-stream'})
        }];
    } finally {
        worker.terminate();
    }
}
