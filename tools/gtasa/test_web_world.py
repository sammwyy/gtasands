import json
import shutil
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which('node'), 'Node unavailable')
class WebWorldTest(unittest.TestCase):
    def test_missing_world_is_built_from_original_folder(self):
        script = f"""
            import {{convertOriginalWorld}} from
                {json.dumps((ROOT / 'web/local_world.mjs').as_uri())};
            import {{strict as assert}} from 'node:assert';
            const source = new Map([
                ['data/gta.dat', new Blob([])],
                ['models/gta3.img', new Blob([])],
                ['models/gta_int.img', new Blob([])]
            ]);
            let terminated = false;
            const workerFactory = () => ({{
                postMessage({{files, options}}) {{
                    assert.equal(files, source);
                    assert.equal(options.sector, 50);
                    queueMicrotask(() => this.onmessage({{
                        data: {{models: 11, instances: 20,
                                bytes: Uint8Array.of(68, 87, 76, 68).buffer}}
                    }}));
                }},
                terminate() {{terminated = true;}}
            }});
            const messages = [];
            const generated = await convertOriginalWorld(source, [],
                {{sector: 50}}, message => messages.push(message), workerFactory);
            assert.equal(terminated, true);
            assert.equal(generated[0].path, 'data/world.dwld');
            assert.equal(generated[0].file.size, 4);
            assert.equal(messages.length, 2);
            assert.deepEqual(await convertOriginalWorld(source, generated,
                {{}}, () => assert.fail('already converted'),
                () => assert.fail('worker not needed')), []);
            await assert.rejects(convertOriginalWorld(new Map(), []));
        """
        subprocess.run(['node', '--input-type=module', '-e', script], check=True)
