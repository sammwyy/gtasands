import functools
import http.server
import json
import shutil
import socket
import subprocess
import tempfile
import threading
import time
import unittest
import urllib.request
from pathlib import Path

from .test_portable_build import ROOT


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, format, *args):
        pass


@unittest.skipUnless(
    shutil.which('chromium-browser') and shutil.which('node') and
    shutil.which('ffmpeg') and shutil.which('emcc'),
    'Chromium, Node, ffmpeg or Emscripten unavailable'
)
class WebOriginalInstallTest(unittest.TestCase):
    def test_original_folder_creates_local_rom_and_assets_zip(self):
        subprocess.run(['make', '-f', 'tools/portable/Makefile',
                        'web/assets.mjs'], cwd=ROOT, check=True,
                       stdout=subprocess.DEVNULL)
        (ROOT / 'build').mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=ROOT / 'build',
                                         prefix='test-web-original-') as tmp:
            directory = Path(tmp)
            page = directory / 'index.html'
            movie = directory / 'web_original_smoke.mpg'
            page.write_bytes((ROOT / 'tools/gtasa/web_original_smoke.html').read_bytes())
            subprocess.run([
                'ffmpeg', '-nostdin', '-loglevel', 'error',
                '-f', 'lavfi', '-i', 'testsrc=size=64x48:rate=30:duration=1',
                '-f', 'lavfi', '-i',
                'sine=frequency=440:sample_rate=44100:duration=1',
                '-c:v', 'mpeg1video', '-q:v', '5', '-c:a', 'mp2',
                '-b:a', '128k', '-shortest', str(movie)
            ], check=True)
            handler = functools.partial(QuietHandler,
                                        directory=str(ROOT))
            server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), handler)
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                with socket.socket() as address:
                    address.bind(('127.0.0.1', 0))
                    port = address.getsockname()[1]
                url = (f'http://127.0.0.1:{server.server_port}/'
                       f'build/{directory.name}/index.html')
                with tempfile.TemporaryDirectory() as profile:
                    browser = subprocess.Popen([
                        shutil.which('chromium-browser'), '--headless',
                        '--no-sandbox', '--disable-gpu',
                        '--disable-dev-shm-usage', '--remote-allow-origins=*',
                        f'--remote-debugging-port={port}',
                        f'--user-data-dir={profile}', url
                    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                    try:
                        for _ in range(100):
                            try:
                                with urllib.request.urlopen(
                                        f'http://127.0.0.1:{port}/json/list',
                                        timeout=1) as response:
                                    pages = json.load(response)
                                target = next(page for page in pages
                                              if page.get('type') == 'page' and
                                              page.get('url') == url)
                                break
                            except (OSError, StopIteration):
                                time.sleep(0.1)
                        else:
                            self.fail('Chromium did not open the installer')
                        script = """
                            const ws = new WebSocket(process.argv[1]);
                            await new Promise((resolve, reject) => {
                                ws.onopen = resolve; ws.onerror = reject;
                            });
                            let result = '';
                            for (let i = 0; i < 240; i++) {
                                const id = i + 1;
                                ws.send(JSON.stringify({id, method: 'Runtime.evaluate',
                                    params: {expression:
                                        "document.querySelector('#result')?.textContent",
                                        returnByValue: true}}));
                                const response = await new Promise((resolve, reject) => {
                                    const timer = setTimeout(() =>
                                        reject(new Error('CDP timeout')), 15000);
                                    ws.onmessage = event => {
                                        const value = JSON.parse(event.data);
                                        if (value.id === id) {
                                            clearTimeout(timer); resolve(value);
                                        }
                                    };
                                });
                                result = response.result?.result?.value;
                                if (result && result !== 'running') break;
                                await new Promise(resolve => setTimeout(resolve, 500));
                            }
                            ws.close();
                            if (!result || result === 'running' ||
                                result.startsWith('ERROR'))
                                throw new Error(result || 'Installer did not finish');
                            console.log(result);
                        """
                        process = subprocess.run(
                            ['node', '--input-type=module', '-e', script,
                             target['webSocketDebuggerUrl']],
                            cwd=ROOT, capture_output=True, text=True, timeout=160)
                        self.assertEqual(process.returncode, 0,
                                         process.stdout + process.stderr)
                        result = json.loads(process.stdout.strip())
                        self.assertEqual(result['report'], 'ZIP SD creado localmente.')
                        self.assertGreaterEqual(result['entries'], 20)
                        self.assertGreater(result['size'], 1_000_000)
                    finally:
                        browser.terminate()
                        try:
                            browser.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            browser.kill()
                            browser.wait()
            finally:
                server.shutdown()
                server.server_close()
