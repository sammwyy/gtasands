export async function convertOriginalLoose(original, converted, manifest,
                                           progress = () => {},
                                           workerFactory = () => new Worker(
                                               new URL('./loose_worker.mjs', import.meta.url),
                                               {type: 'module'})) {
    const worker = workerFactory();
    try {
        const result = await new Promise((resolve, reject) => {
            worker.onmessage = event => {
                if (event.data.progress) progress(event.data.progress);
                else if (event.data.error) reject(new Error(event.data.error));
                else resolve(event.data);
            };
            worker.onerror = event => reject(new Error(event.message));
            worker.postMessage({files: original, converted, manifest});
        });
        return result.generated;
    } finally {
        worker.terminate();
    }
}
