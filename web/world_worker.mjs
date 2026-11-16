import createModule from './assets.mjs';
import {convertWorld} from './world.mjs';

let modulePromise;

self.onmessage = async event => {
    try {
        modulePromise ??= createModule();
        const module = await modulePromise;
        const result = convertWorld(module, event.data.files,
                                    event.data.options);
        self.postMessage({bytes: result.data.buffer,
                          models: result.models, instances: result.instances},
                         [result.data.buffer]);
    } catch (error) {
        self.postMessage({error: error.message});
    }
};
