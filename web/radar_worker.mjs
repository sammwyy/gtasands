import createModule from './assets.mjs';
import {convertRadar} from './radar_assets.mjs';

let modulePromise;

self.onmessage = async event => {
    try {
        modulePromise ??= createModule();
        const module = await modulePromise;
        const result = await convertRadar(module, event.data.file,
            event.data.grid, message => self.postMessage({progress: message}));
        self.postMessage({bytes: result.data.buffer, tiles: result.tiles},
                         [result.data.buffer]);
    } catch (error) {
        self.postMessage({error: error.message});
    }
};
