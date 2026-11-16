import json
import shutil
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which('node'), 'Node unavailable')
class WebVehiclesTest(unittest.TestCase):
    def test_missing_vehicles_are_built_locally(self):
        script = f"""
            import {{convertOriginalVehicles}} from
                {json.dumps((ROOT / 'web/local_vehicles.mjs').as_uri())};
            import {{strict as assert}} from 'node:assert';
            const source = new Map([['data/vehicles.ide', new Blob([])]]);
            let terminated = false;
            const workerFactory = () => ({{
                postMessage({{files}}) {{
                    assert.equal(files, source);
                    queueMicrotask(() => this.onmessage({{
                        data: {{models: 2, bytes: Uint8Array.of(68, 86, 69, 72).buffer}}
                    }}));
                }},
                terminate() {{terminated = true;}}
            }});
            const built = await convertOriginalVehicles(source, [],
                () => {{}}, workerFactory);
            assert.equal(terminated, true);
            assert.equal(built[0].path, 'data/vehicles.dveh');
            assert.equal(built[0].file.size, 4);
            assert.deepEqual(await convertOriginalVehicles(source, built,
                () => assert.fail('already converted'),
                () => assert.fail('worker not needed')), []);
        """
        subprocess.run(['node', '--input-type=module', '-e', script], check=True)
