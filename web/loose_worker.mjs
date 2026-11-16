import createModule from './assets.mjs';
import {convertLooseAssets} from './loose_assets.mjs';
import {Parameters} from './params.mjs';

self.onmessage = async event => {
    try {
        const module = await createModule();
        const {files, converted, manifest} = event.data;
        const planner = new Parameters(manifest, files);
        const generated = await convertLooseAssets(module, planner, files,
            converted, message => self.postMessage({progress: message}));
        self.postMessage({generated});
    } catch (error) {
        self.postMessage({error: error.message});
    }
};
