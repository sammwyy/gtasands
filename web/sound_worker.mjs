import createModule from './assets.mjs';
import {Parameters} from './params.mjs';
import {convertSFX, convertStreams} from './sound_assets.mjs';

self.onmessage = async event => {
    try {
        const module = await createModule();
        const {files, converted, manifest} = event.data;
        const planner = new Parameters(manifest, files);
        const existing = new Set(converted.map(entry => entry.path.toLowerCase()));
        const progress = message => self.postMessage({progress: message});
        const sfx = await convertSFX(module, planner, files, existing, progress);
        const streams = await convertStreams(module, planner, files,
                                            existing, progress);
        self.postMessage({generated: [...sfx.files, ...streams.files],
                          banks: sfx.banks, tracks: streams.tracks});
    } catch (error) {
        self.postMessage({error: error.message});
    }
};
