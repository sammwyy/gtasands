import json
import shutil
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which('node'), 'Node unavailable')
class WebAudioTest(unittest.TestCase):
    def test_source_rate_pcm_rounds_ties_even_and_trims_padding(self):
        script = f"""
            import {{pcm16FromAudioBuffer}} from
                {json.dumps((ROOT / 'web/audio_pcm.mjs').as_uri())};
            import {{strict as assert}} from 'node:assert';
            const mono = new Float32Array([0, 0.5 / 32768, 1.5 / 32768,
                -0.5 / 32768, -1.5 / 32768, 1, -1]);
            const buffer = channels => ({{
                length: channels[0].length,
                numberOfChannels: channels.length,
                getChannelData: index => channels[index]
            }});
            assert.deepEqual([...pcm16FromAudioBuffer(buffer([mono]), mono.length)],
                [0, 0, 2, 0, -2, 32767, -32768]);
            const left = new Float32Array([1, 2, -3, 4, 999].map(x => x / 32768));
            const right = new Float32Array([0, 1, 0, 3, 999].map(x => x / 32768));
            assert.deepEqual([...pcm16FromAudioBuffer(buffer([left, right]), 4)],
                [0, 2, -2, 4]);
            assert.throws(() => pcm16FromAudioBuffer(buffer([mono]), 8));
            assert.throws(() => pcm16FromAudioBuffer(buffer([mono, mono, mono]), 1));
        """
        subprocess.run(['node', '--input-type=module', '-e', script], check=True)
