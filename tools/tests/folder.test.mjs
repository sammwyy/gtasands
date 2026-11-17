import assert from 'node:assert/strict';
import test from 'node:test';
import {addHandlingFile, directoryFiles, indexFiles,
        missingConversionFiles, selectedFiles} from '../../web/folder.mjs';

const required = [
    'models/gta3.img', 'models/gta_int.img', 'models/player.img',
    'anim/anim.img', 'anim/ped.ifp', 'data/gta.dat',
    'data/peds.ide', 'data/vehicles.ide', 'data/handling.cfg',
    'data/carcols.dat', 'data/surface.dat', 'data/surfinfo.dat',
    'audio/config/pakfiles.dat', 'audio/config/banklkup.dat',
    'audio/config/strmpaks.dat', 'audio/config/traklkup.dat',
    'movies/logo.mpg', 'movies/gtatitles.mpg', 'text/american.gxt'
];

test('Windows-style relative paths keep the game-directory contents', async () => {
    const files = required.map(path => ({
        webkitRelativePath: `GTA San Andreas\\${path.replaceAll('/', '\\')}`,
        name: path.split('/').at(-1),
        text: async () => '',
        arrayBuffer: async () => new ArrayBuffer(0)
    }));
    const indexed = indexFiles(selectedFiles(files));
    assert.equal(indexed.has('data/handling.cfg'), true);
    assert.deepEqual(await missingConversionFiles(indexed), []);
});

test('selecting a parent directory with one game installation works', async () => {
    const indexed = indexFiles(required.map(path => ({
        path: `Steam/GTA San Andreas/${path}`,
        file: {name: path, text: async () => '',
               arrayBuffer: async () => new ArrayBuffer(0)}
    })));
    assert.equal(indexed.has('data/handling.cfg'), true);
    assert.deepEqual(await missingConversionFiles(indexed), []);
});

test('non-game directories are not stripped', () => {
    const indexed = indexFiles([{path: 'other/data/handling.cfg', file: {name: 'handling.cfg'}}]);
    assert.equal(indexed.has('data/handling.cfg'), false);
});

test('directory picker retrieves handling.cfg when Edge omits it from entries', async () => {
    const source = {name: 'HANDLING.CFG', size: 64532, text: async () => ''};
    const data = {
        kind: 'directory',
        async *entries() {
            yield ['gta.dat', {kind: 'file',
                getFile: async () => ({text: async () => ''})}];
        },
        async getFileHandle(name) {
            assert.equal(name, 'handling.cfg');
            return {getFile: async () => source};
        }
    };
    const root = {async *entries() { yield ['DATA', data]; }};
    const indexed = indexFiles(await Array.fromAsync(directoryFiles(root)));
    assert.equal(indexed.get('data/handling.cfg'), source);
});

test('directory picker does not invent a missing handling file or duplicate a visible one', async () => {
    const source = {name: 'HANDLING.CFG', size: 64532};
    const data = {
        kind: 'directory',
        visible: false,
        forbidden: false,
        async *entries() {
            if (this.visible) yield ['HANDLING.CFG',
                {kind: 'file', getFile: async () => source}];
        },
        async getFileHandle() {
            if (this.visible) throw new Error('Duplicate lookup.');
            if (this.forbidden) throw new TypeError('Name is not allowed');
            throw Object.assign(new Error('Missing'), {name: 'NotFoundError'});
        }
    };
    const root = {async *entries() { yield ['data', data]; }};
    assert.equal(indexFiles(await Array.fromAsync(directoryFiles(root)))
        .has('data/handling.cfg'), false);
    data.forbidden = true;
    assert.equal(indexFiles(await Array.fromAsync(directoryFiles(root)))
        .has('data/handling.cfg'), false);
    data.visible = true;
    const indexed = indexFiles(await Array.fromAsync(directoryFiles(root)));
    assert.equal(indexed.size, 1);
    assert.equal(indexed.get('data/handling.cfg'), source);
});

test('manual handling fallback checks the selected filename and missing inputs', async () => {
    const indexed = indexFiles(required.filter(path => path !== 'data/handling.cfg')
        .map(path => ({path, file: {text: async () => '',
            arrayBuffer: async () => new ArrayBuffer(0)}})));
    assert.deepEqual(await missingConversionFiles(indexed), ['data/handling.cfg']);
    assert.throws(() => addHandlingFile(indexed, {name: 'handling.cfg.txt', size: 10}),
        /handling.cfg/);
    assert.deepEqual(await missingConversionFiles(indexed), ['data/handling.cfg']);
    addHandlingFile(indexed, {name: 'HANDLING.CFG', size: 64532});
    assert.deepEqual(await missingConversionFiles(indexed), []);
});
