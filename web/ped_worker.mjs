import createModule from './assets.mjs';
import {convertPeds} from './peds.mjs';

let modulePromise;

self.onmessage = async event => {
    try {
        modulePromise ??= createModule();
        const module = await modulePromise;
        const result = convertPeds(module, event.data.files,
                                   event.data.options);
        self.postMessage({bytes: result.data.buffer,
                          models: result.models, nodes: result.nodes},
                         [result.data.buffer]);
    } catch (error) {
        self.postMessage({error: error.message});
    }
};
