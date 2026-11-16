import json
import shutil
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which('node'), 'Node unavailable')
class WebMoviesTest(unittest.TestCase):
    def test_original_movie_fills_missing_converted_asset(self):
        script = f"""
            import {{convertOriginalMovies}} from
                {json.dumps((ROOT / 'web/local_movies.mjs').as_uri())};
            import {{strict as assert}} from 'node:assert';
            const original = new Map([
                ['movies/gtatitles.mpg', new Blob([Uint8Array.of(2, 5, 7)])]
            ]);
            let terminated = false;
            const workerFactory = () => ({{
                postMessage({{file}}) {{
                    assert.equal(file.size, 3);
                    queueMicrotask(() => this.onmessage({{
                        data: {{frames: 10, bytes: Uint8Array.of(68, 86, 73, 68).buffer}}
                    }}));
                }},
                terminate() {{terminated = true;}}
            }});
            const converted = [{{path: 'movies/logo.dvid', file: new Blob([])}}];
            const messages = [];
            const generated = await convertOriginalMovies(original, converted,
                message => messages.push(message), workerFactory);
            assert.deepEqual(generated.map(entry => entry.path),
                             ['movies/gtatitles.dvid']);
            assert.equal(generated[0].file.size, 4);
            assert.equal(terminated, true);
            assert.equal(messages.length, 2);
            assert.deepEqual(await convertOriginalMovies(original, [
                ...converted, ...generated
            ], () => assert.fail('no conversion needed'), () =>
                assert.fail('no worker needed')), []);
            await assert.rejects(convertOriginalMovies(new Map(), converted));
        """
        subprocess.run(['node', '--input-type=module', '-e', script], check=True)

    def test_mpeg_conversion_uses_wasm_and_frees_buffers(self):
        script = f"""
            import {{convertMovie}} from
                {json.dumps((ROOT / 'web/movies.mjs').as_uri())};
            import {{strict as assert}} from 'node:assert';
            const buffer = new ArrayBuffer(1024);
            const freed = [];
            let next = 8;
            const module = {{
                HEAPU8: new Uint8Array(buffer),
                HEAPU32: new Uint32Array(buffer),
                _malloc(length) {{
                    const pointer = next;
                    next += (length + 7) & ~7;
                    return pointer;
                }},
                _free(pointer) {{freed.push(pointer);}},
                _mpeg_movie_convert(input, length, fps, rate, output, size,
                                    frames) {{
                    assert.deepEqual([fps, rate], [10, 22050]);
                    assert.deepEqual([...this.HEAPU8.slice(input, input + length)],
                                     [0, 4, 7]);
                    const blob = this._malloc(4);
                    this.HEAPU8.set([68, 86, 73, 68], blob);
                    this.HEAPU32[output >>> 2] = blob;
                    this.HEAPU32[size >>> 2] = 4;
                    this.HEAPU32[frames >>> 2] = 2;
                    return 1;
                }}
            }};
            const result = convertMovie(module, new Uint8Array([0, 4, 7]));
            assert.deepEqual([...result.data], [68, 86, 73, 68]);
            assert.equal(result.frames, 2);
            assert.equal(freed.length, 3);
            assert.throws(() => convertMovie(module, new Uint8Array(), 10, 22050));
        """
        subprocess.run(['node', '--input-type=module', '-e', script], check=True)
