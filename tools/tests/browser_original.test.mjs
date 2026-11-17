import assert from 'node:assert/strict';
import {spawn, spawnSync} from 'node:child_process';
import {createReadStream, existsSync} from 'node:fs';
import {copyFile, mkdtemp, readFile} from 'node:fs/promises';
import {createServer} from 'node:http';
import {tmpdir} from 'node:os';
import {extname, join, resolve, sep} from 'node:path';
import test from 'node:test';
import {setTimeout as delay} from 'node:timers/promises';

const root = resolve(import.meta.dirname, '../..');

function server() {
    return createServer((request, response) => {
        try {
            const requested = decodeURIComponent(new URL(request.url,
                'http://127.0.0.1').pathname);
            const path = resolve(root, '.' + requested);
            if (!path.startsWith(root + sep)) {
                response.writeHead(403).end();
                return;
            }
            response.setHeader('content-type', ({
                '.html': 'text/html', '.json': 'application/json',
                '.mjs': 'text/javascript', '.wasm': 'application/wasm'
            })[extname(path)] || 'application/octet-stream');
            const stream = createReadStream(path);
            stream.on('error', () => {
                if (!response.headersSent) response.writeHead(404).end();
                else response.destroy();
            });
            stream.pipe(response);
        } catch {
            response.writeHead(400).end();
        }
    });
}

async function debuggerPage(profile, url, child) {
    for (let i = 0; i < 100; i++) {
        if (child.exitCode !== null) throw new Error('Chromium exited before opening the installer.');
        try {
            const [port] = (await readFile(join(profile, 'DevToolsActivePort'),
                'utf8')).split('\n');
            const response = await fetch(`http://127.0.0.1:${port}/json/list`,
                {signal: AbortSignal.timeout(1000)});
            const pages = await response.json();
            const page = pages.find(item => item.type === 'page' && item.url === url);
            if (page) return page.webSocketDebuggerUrl;
        } catch {}
        await delay(100);
    }
    throw new Error('Chromium did not open the installer.');
}

async function resultFromPage(debuggerURL) {
    const ws = new WebSocket(debuggerURL);
    await new Promise((resolveOpen, reject) => {
        ws.addEventListener('open', resolveOpen, {once: true});
        ws.addEventListener('error', reject, {once: true});
    });
    try {
        for (let i = 0; i < 240; i++) {
            const result = await new Promise((resolveResult, reject) => {
                const id = i + 1;
                const timer = setTimeout(() => {
                    ws.removeEventListener('message', onMessage);
                    reject(new Error('Browser debugging request timed out.'));
                }, 15000);
                const onMessage = event => {
                    const message = JSON.parse(event.data);
                    if (message.id !== id) return;
                    clearTimeout(timer);
                    ws.removeEventListener('message', onMessage);
                    resolveResult(message.result?.result?.value);
                };
                ws.addEventListener('message', onMessage);
                ws.send(JSON.stringify({id, method: 'Runtime.evaluate', params: {
                    expression: "document.querySelector('#result')?.textContent",
                    returnByValue: true
                }}));
            });
            if (result && result !== 'running') {
                if (result.startsWith('ERROR')) throw new Error(result);
                return JSON.parse(result);
            }
            await delay(500);
        }
        throw new Error('The browser installer did not finish.');
    } finally {
        ws.close();
    }
}

test('original folder creates a local ROM and assets ZIP in Chromium', {
    timeout: 180000
}, async t => {
    if (!existsSync(resolve(root, 'web/assets.wasm')) ||
        spawnSync('chromium-browser', ['--version']).status !== 0 ||
        spawnSync('ffmpeg', ['-version']).status !== 0)
        return t.skip('Chromium, ffmpeg or WASM unavailable');
    const work = await mkdtemp(join(root, 'build/test-browser-original-'));
    const profile = await mkdtemp(join(tmpdir(), 'gtasads-browser-profile-'));
    await copyFile(resolve(root, 'tools/tests/web_original_smoke.html'),
        join(work, 'index.html'));
    const movie = join(work, 'web_original_smoke.mpg');
    const video = spawnSync('ffmpeg', ['-nostdin', '-loglevel', 'error',
        '-f', 'lavfi', '-i', 'testsrc=size=64x48:rate=30:duration=1',
        '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=44100:duration=1',
        '-c:v', 'mpeg1video', '-q:v', '5', '-c:a', 'mp2', '-b:a', '128k',
        '-shortest', movie], {encoding: 'utf8'});
    assert.equal(video.status, 0, video.stderr);
    const site = server();
    await new Promise(resolveListen => site.listen(0, '127.0.0.1', resolveListen));
    const url = `http://127.0.0.1:${site.address().port}/build/` +
        `${work.split(sep).at(-1)}/index.html`;
    const browser = spawn('chromium-browser', [
        '--headless', '--no-sandbox', '--disable-gpu', '--disable-dev-shm-usage',
        '--remote-allow-origins=*', '--remote-debugging-port=0',
        `--user-data-dir=${profile}`, url
    ], {stdio: 'ignore'});
    try {
        const debuggerURL = await debuggerPage(profile, url, browser);
        const result = await resultFromPage(debuggerURL);
        assert.equal(result.report, 'SD ZIP created locally.');
        assert.ok(result.entries >= 20);
        assert.ok(result.size > 1_000_000);
    } finally {
        browser.kill('SIGTERM');
        await Promise.race([new Promise(resolveExit => browser.once('exit', resolveExit)),
            delay(5000)]);
        if (browser.exitCode === null) browser.kill('SIGKILL');
        await new Promise(resolveClose => site.close(resolveClose));
    }
});
