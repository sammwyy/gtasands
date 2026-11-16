const files = document.querySelector('#files');
const button = document.querySelector('#build');
const status = document.querySelector('#status');

button.addEventListener('click', async () => {
    if (!files.files.length) {
        status.textContent = 'Select at least one file.';
        return;
    }
    button.disabled = true;
    status.textContent = 'Packaging locally…';
    try {
        const {default: createModule} = await import('./dspk.mjs');
        const module = await createModule();
        const args = ['/output.dspk'];
        for (const [index, file] of [...files.files].entries()) {
            if (file.size > 32 * 1024 * 1024) throw new Error('This test limits each file to 32 MiB.');
            const path = `/input${index}`;
            module.FS.writeFile(path, new Uint8Array(await file.arrayBuffer()));
            args.push(file.name, path);
        }
        module.callMain(args);
        const blob = new Blob([module.FS.readFile('/output.dspk')], {type: 'application/octet-stream'});
        const url = URL.createObjectURL(blob);
        const link = document.createElement('a');
        link.href = url;
        link.download = 'test.dspk';
        link.click();
        setTimeout(() => URL.revokeObjectURL(url), 60000);
        status.textContent = 'DSPK created in the browser.';
    } catch (error) {
        status.textContent = `Could not create DSPK: ${error.message}`;
    } finally {
        button.disabled = false;
    }
});
