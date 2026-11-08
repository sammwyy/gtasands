import struct
import unittest
from array import array
from pathlib import Path

from . import audio
from .pipeline import Config


class RadioAudioTest(unittest.TestCase):
    def test_seek_checkpoints_match_sequential_adpcm_state(self):
        samples = array('h', (int((i * 173) % 50000 - 25000) for i in range(25000)))
        checkpoints = bytearray()
        sound = audio.make_sound(samples, 11025, {
            'volume': 1.0, 'sample_rate': 11025, 'max_seconds': 0,
            'format': 'adpcm', 'checkpoints': checkpoints,
        })
        size, = struct.unpack_from('<I', sound, 20)
        at = 24 + ((size + 3) & ~3)
        self.assertEqual(sound[at:], checkpoints)
        self.assertEqual(len(checkpoints), 4 * 4)
        pred, index = struct.unpack_from('<hB', sound, at + 4)
        codes = sound[28:at]
        replay_pred, replay_index = struct.unpack_from('<hB', sound, 24)
        for i in range(8192):
            code = (codes[i // 2] >> (4 if i & 1 else 0)) & 15
            step = audio.STEP[replay_index]
            diff = (step >> 3) + (step if code & 4 else 0) + (step >> 1 if code & 2 else 0) + (step >> 2 if code & 1 else 0)
            replay_pred = max(-32767, min(32767, replay_pred - diff if code & 8 else replay_pred + diff))
            replay_index = max(0, min(88, replay_index + audio.IDX[code & 7]))
        self.assertEqual((pred, index), (replay_pred, replay_index))
        pack = audio.pack_streams([(231, sound)], seekable=True)
        magic, version, count = struct.unpack_from('<4sHH', pack)
        track, start, length, seek, n = struct.unpack_from('<IIIII', pack, 8)
        self.assertEqual((magic, version, count, track), (b'DSST', 2, 1, 231))
        self.assertEqual((start, length, seek, n), (28, len(sound), 28 + at, 4))
        self.assertEqual(pack[seek:seek + 16], checkpoints)

    def test_radio_is_not_in_rom_or_user_tracks(self):
        cfg = Config(Path(__file__).resolve().parents[2] / 'resources.toml')
        station = cfg.sections['radio_streams']
        self.assertTrue(station['sd_only'])
        for name in ('CH', 'CO', 'CR', 'DS', 'HC', 'MH', 'MR', 'NJ', 'RE', 'RG', 'TK'):
            self.assertFalse(cfg.params(station, 'audio', name)[2])
            self.assertTrue(cfg.params(cfg.sections['audio_streams'], 'audio', name)[2])
        for name in ('AA', 'ADVERTS', 'AMBIENCE', 'BEATS', 'CUTSCENE', 'USER_TRACKS'):
            self.assertTrue(cfg.params(station, 'audio', name)[2])


if __name__ == '__main__':
    unittest.main()
