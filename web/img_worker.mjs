import createModule from './assets.mjs';
import {convertIMG} from './img_assets.mjs';
import {Parameters} from './params.mjs';

let modulePromise;

self.onmessage = async event => {
    try {
        modulePromise ??= createModule();
        const module = await modulePromise;
        const {files, manifest, section} = event.data;
        const source = files.get(manifest.sections[section].src.toLowerCase());
        if (!source) throw new Error(`Falta el IMG original de ${section}.`);
        const planner = new Parameters(manifest, files);
        const result = await convertIMG(module, planner, section, source,
            message => self.postMessage({progress: message}));
        self.postMessage({path: manifest.sections[section].out,
                          file: result.blob, entries: result.files,
                          skipped: result.skipped});
    } catch (error) {
        self.postMessage({error: error.message});
    }
};
