import json
import shutil
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which('node'), 'Node unavailable')
class WebStreamsTest(unittest.TestCase):
    def test_native_vorbis_path_uses_shared_c_core(self):
        script = f"""
            import {{convertStream}} from
                {json.dumps((ROOT / 'web/streams.mjs').as_uri())};
            import {{strict as assert}} from 'node:assert';
            const buffer = new ArrayBuffer(1 << 16);
            let next = 8;
            const freed = [];
            const module = {{
                HEAPU8: new Uint8Array(buffer),
                HEAPU32: new Uint32Array(buffer),
                _malloc(size) {{
                    const pointer = next;
                    next += (size + 7) & ~7;
                    return pointer;
                }},
                _free(pointer) {{freed.push(pointer);}},
                _audio_stream_convert(input, length, low, high, rate, format,
                                      seekable, volume, seconds, output, size) {{
                    assert.deepEqual([low, high, rate, format, seekable, volume,
                                      seconds], [7, 0, 11025, 2, 1, 0.7, 0]);
                    assert.equal(length, 0x1f84 + 4);
                    assert.equal(this.HEAPU8[input + 0x1f84], 12);
                    const result = this._malloc(4);
                    this.HEAPU8.set([68, 83, 78, 68], result);
                    this.HEAPU32[output >>> 2] = result;
                    this.HEAPU32[size >>> 2] = 4;
                    return 1;
                }}
            }};
            const encrypted = new Uint8Array(0x1f84 + 4);
            encrypted[0x1f84] = 12;
            const result = await convertStream(module, encrypted, 7,
                {{sample_rate: 11025, format: 'adpcm', seekable: true,
                  volume: 0.7}});
            assert.deepEqual([...result], [68, 83, 78, 68]);
            assert.equal(freed.length, 3);
        """
        subprocess.run(['node', '--input-type=module', '-e', script], check=True)

    def test_encrypted_ogg_is_decoded_and_encoded_through_wasm_core(self):
        script = f"""
            import {{convertStream}} from
                {json.dumps((ROOT / 'web/streams.mjs').as_uri())};
            import {{strict as assert}} from 'node:assert';
            const buffer = new ArrayBuffer(1 << 16);
            const key = Uint8Array.from([0xEA, 0x3A, 0xC4, 0xA1, 0x9A, 0xA8,
                0x14, 0xF3, 0x48, 0xB0, 0xD7, 0x23, 0x9D, 0xE8, 0xFF, 0xF1]);
            let next = 8;
            const freed = [];
            const module = {{
                HEAPU8: new Uint8Array(buffer),
                HEAP16: new Int16Array(buffer),
                HEAPU32: new Uint32Array(buffer),
                _malloc(size) {{
                    const pointer = next;
                    next += (size + 7) & ~7;
                    return pointer;
                }},
                _free(pointer) {{freed.push(pointer);}},
                _audio_decrypt_parts(pointer, length, low, high, output) {{
                    assert.equal(high, 0);
                    assert.equal(low, 7);
                    for (let i = 0; i < length; i++)
                        this.HEAPU8[output + i] = this.HEAPU8[pointer + i] ^
                            key[(low + i) & 15];
                }},
                _ogg_read_stream_info(pointer, length, rate, samples, channels) {{
                    assert.equal(length, 4);
                    assert.deepEqual([...this.HEAPU8.slice(pointer, pointer + 4)],
                        [...new TextEncoder().encode('OggS')]);
                    this.HEAPU32[rate >>> 2] = 32000;
                    this.HEAPU32[samples >>> 2] = 4;
                    this.HEAPU32[channels >>> 2] = 2;
                    return 1;
                }},
                _dsnd_encode_simple(pointer, count, rate, sampleRate, format,
                                    seekable, volume, maxSeconds, loop, head,
                                    result, length) {{
                    assert.deepEqual([count, rate, sampleRate, format, seekable,
                                      volume, maxSeconds, loop, head],
                                     [4, 32000, 11025, 2, 1, 1, 0, -1, 0]);
                    assert.deepEqual([...this.HEAP16.slice(pointer >>> 1,
                                                          (pointer >>> 1) + count)],
                                     [0, 2, -2, 4]);
                    const output = this._malloc(4);
                    this.HEAPU8.set([68, 83, 78, 68], output);
                    this.HEAPU32[result >>> 2] = output;
                    this.HEAPU32[length >>> 2] = 4;
                    return 1;
                }}
            }};
            const encrypted = new Uint8Array(0x1f84 + 4);
            const plain = new TextEncoder().encode('OggS');
            for (let i = 0; i < plain.length; i++)
                encrypted[0x1f84 + i] = plain[i] ^ key[(7 + 0x1f84 + i) & 15];
            const audio = {{
                length: 6, numberOfChannels: 2,
                getChannelData: channel => channel ?
                    new Float32Array([0, 1, 0, 3, 999, 999].map(x => x / 32768)) :
                    new Float32Array([1, 2, -3, 4, 999, 999].map(x => x / 32768))
            }};
            const output = await convertStream(module, encrypted, 7,
                {{sample_rate: 11025, format: 'adpcm', seekable: true}},
                async (ogg, rate) => {{
                    assert.equal(rate, 32000);
                    assert.deepEqual([...ogg], [...plain]);
                    return audio;
                }});
            assert.deepEqual([...output], [68, 83, 78, 68]);
            assert.equal(freed.length, 5);
        """
        subprocess.run(['node', '--input-type=module', '-e', script], check=True)
