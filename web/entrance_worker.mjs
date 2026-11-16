import createModule from './assets.mjs';
import {convertEntrances} from './entrances.mjs';

let modulePromise;

self.onmessage = async event => {
    try {
        modulePromise ??= createModule();
        const module = await modulePromise;
        const result = convertEntrances(module, event.data.files);
        self.postMessage({denx: result.enex.buffer,
                          dpoi: result.points.buffer, count: result.count},
                         [result.enex.buffer, result.points.buffer]);
    } catch (error) {
        self.postMessage({error: error.message});
    }
};
