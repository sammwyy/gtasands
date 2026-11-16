const movies = [
    ['movies/logo.mpg', 'movies/logo.dvid'],
    ['movies/gtatitles.mpg', 'movies/gtatitles.dvid']
];

export async function convertOriginalMovies(original, converted, progress = () => {},
                                            workerFactory = () => new Worker(
                                                new URL('./movie_worker.mjs', import.meta.url),
                                                {type: 'module'})) {
    const existing = new Set(converted.map(({path}) => path.toLowerCase()));
    const missing = movies.filter(([, target]) => !existing.has(target));
    if (!missing.length) return [];
    const absent = missing.filter(([source]) => !original.has(source));
    if (absent.length) throw new Error(
        `Missing original videos for the ZIP: ${absent.map(([path]) => path).join(', ')}`);
    const worker = workerFactory();
    const generated = [];
    try {
        for (const [source, target] of missing) {
            progress(`Converting ${source} in your browser…`);
            const result = await new Promise((resolve, reject) => {
                worker.onmessage = event => event.data.error ?
                    reject(new Error(event.data.error)) : resolve(event.data);
                worker.onerror = event => reject(new Error(event.message));
                worker.postMessage({file: original.get(source)});
            });
            generated.push({
                path: target,
                file: new Blob([result.bytes], {type: 'application/octet-stream'})
            });
            progress(`${source}: ${result.frames} frames converted.`);
        }
        return generated;
    } finally {
        worker.terminate();
    }
}
