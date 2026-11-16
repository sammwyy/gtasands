import json
import shutil
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which('node'), 'Node unavailable')
class WebEntrancesTest(unittest.TestCase):
    def test_missing_entrances_and_points_are_built_locally(self):
        script = f"""
            import {{convertOriginalEntrances}} from
                {json.dumps((ROOT / 'web/local_entrances.mjs').as_uri())};
            import {{strict as assert}} from 'node:assert';
            const source = new Map([['data/gta.dat', new Blob([])]]);
            let terminated = false;
            const workerFactory = () => ({{
                postMessage({{files}}) {{
                    assert.equal(files, source);
                    queueMicrotask(() => this.onmessage({{
                        data: {{count: 2,
                                denx: Uint8Array.of(68, 69, 78, 88).buffer,
                                dpoi: Uint8Array.of(68, 80, 79, 73).buffer}}
                    }}));
                }},
                terminate() {{terminated = true;}}
            }});
            const built = await convertOriginalEntrances(source, [],
                () => {{}}, workerFactory);
            assert.equal(terminated, true);
            assert.deepEqual(built.map(entry => entry.path),
                ['data/entry_exits.denx', 'data/map_points.dpoi']);
            assert.deepEqual(await convertOriginalEntrances(source, built,
                () => assert.fail('already converted'),
                () => assert.fail('worker not needed')), []);
        """
        subprocess.run(['node', '--input-type=module', '-e', script], check=True)
