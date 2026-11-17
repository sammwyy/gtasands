export async function convertOriginalSound(original, converted, manifest,
                                           progress = () => {},
                                           workerFactory = () => new Worker(
                                               new URL('./sound_worker.mjs', import.meta.url),
                                               {type: 'module'})) {
    const worker = workerFactory();
    try {
        progress('Converting SFX banks and audio tracks…');
        const result = await new Promise((resolve, reject) => {
            worker.onmessage = event => {
                if (event.data.progress) progress(event.data.progress);
                else if (event.data.error) reject(new Error(event.data.error));
                else resolve(event.data);
            };
            worker.onerror = event => reject(new Error(event.message));
            worker.postMessage({files: original, converted, manifest});
        });
        progress(`Audio: ${result.banks} banks and ${result.tracks} tracks converted.`);
        return result.generated;
    } finally {
        worker.terminate();
    }
}
