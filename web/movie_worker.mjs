import createModule from './assets.mjs';
import {convertMovie} from './movies.mjs';

let modulePromise;

self.onmessage = async event => {
    try {
        modulePromise ??= createModule();
        const module = await modulePromise;
        const result = convertMovie(module, new Uint8Array(
            await event.data.file.arrayBuffer()));
        self.postMessage({frames: result.frames, bytes: result.data.buffer},
                         [result.data.buffer]);
    } catch (error) {
        self.postMessage({error: error.message});
    }
};
