import createModule from './assets.mjs';
import {convertVehicles} from './vehicles.mjs';

let modulePromise;

self.onmessage = async event => {
    try {
        modulePromise ??= createModule();
        const module = await modulePromise;
        const result = convertVehicles(module, event.data.files);
        self.postMessage({bytes: result.data.buffer, models: result.models},
                         [result.data.buffer]);
    } catch (error) {
        self.postMessage({error: error.message});
    }
};
