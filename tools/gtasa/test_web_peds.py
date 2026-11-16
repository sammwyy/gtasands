import json
import shutil
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which('node'), 'Node unavailable')
class WebPedsTest(unittest.TestCase):
    def test_missing_peds_are_built_locally(self):
        script = f"""
            import {{convertOriginalPeds}} from
                {json.dumps((ROOT / 'web/local_peds.mjs').as_uri())};
            import {{strict as assert}} from 'node:assert';
            const source = new Map([['data/peds.ide', new Blob([])]]);
            let terminated = false;
            const workerFactory = () => ({{
                postMessage({{files, options}}) {{
                    assert.equal(files, source);
                    assert.deepEqual(options.bounds, [-100, -100, 100, 100]);
                    queueMicrotask(() => this.onmessage({{
                        data: {{models: 2, nodes: 3,
                                bytes: Uint8Array.of(68, 80, 69, 68).buffer}}
                    }}));
                }},
                terminate() {{terminated = true;}}
            }});
            const messages = [];
            const generated = await convertOriginalPeds(source, [],
                {{bounds: [-100, -100, 100, 100]}},
                message => messages.push(message), workerFactory);
            assert.equal(terminated, true);
            assert.equal(generated[0].path, 'data/peds.dped');
            assert.equal(generated[0].file.size, 4);
            assert.equal(messages.length, 2);
            assert.deepEqual(await convertOriginalPeds(source, generated,
                {{}}, () => assert.fail('already converted'),
                () => assert.fail('worker not needed')), []);
        """
        subprocess.run(['node', '--input-type=module', '-e', script], check=True)
