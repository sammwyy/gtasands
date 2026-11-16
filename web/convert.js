const file = document.querySelector('#file');
const button = document.querySelector('#build');
const status = document.querySelector('#status');
const scale = document.querySelector('#scale');
const size = document.querySelector('#size');

button.addEventListener('click', async () => {
    if (!file.files.length) {
        status.textContent = 'Select a TXD file.';
        return;
    }
    button.disabled = true;
    status.textContent = 'Converting locally…';
    try {
        const {default: createModule} = await import('./texture.mjs');
        const module = await createModule();
        module.FS.writeFile('/source.txd', new Uint8Array(await file.files[0].arrayBuffer()));
        module.callMain(['/source.txd', '/converted.dtxd', scale.value, size.value, 'auto']);
        const blob = new Blob([module.FS.readFile('/converted.dtxd')],
                              {type: 'application/octet-stream'});
        const url = URL.createObjectURL(blob);
        const link = document.createElement('a');
        link.href = url;
        link.download = `${file.files[0].name.replace(/\.txd$/i, '')}.dtxd`;
        link.click();
        setTimeout(() => URL.revokeObjectURL(url), 60000);
        status.textContent = 'DTXD converted.';
    } catch (error) {
        status.textContent = `Conversion failed: ${error.message}`;
    } finally {
        button.disabled = false;
    }
});
