import ctypes
import os
import random
import shutil
import struct
import subprocess
import tempfile
import unittest
from array import array
from pathlib import Path
from unittest.mock import patch

from . import audio
from .test_portable_build import build_library

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which('cc'), 'C compiler unavailable')
class PortableAudioTest(unittest.TestCase):
    def test_flat_wasm_sound_entry_matches_python(self):
        with tempfile.TemporaryDirectory() as tmp:
            library = Path(tmp) / 'libassets.so'
            build_library(library)
            native = ctypes.CDLL(str(library))
            native.dsnd_encode_simple.argtypes = (
                ctypes.POINTER(ctypes.c_int16), ctypes.c_size_t,
                ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint8, ctypes.c_int,
                ctypes.c_double, ctypes.c_double, ctypes.c_int32,
                ctypes.c_int16, ctypes.POINTER(ctypes.c_void_p),
                ctypes.POINTER(ctypes.c_size_t))
            native.dsnd_encode_simple.restype = ctypes.c_int
            native.dtex_free.argtypes = (ctypes.c_void_p,)
            samples = array('h', (((i * 109) % 30000) - 15000
                                  for i in range(32000)))
            params = {**audio.DEFAULTS, 'sample_rate': 11025,
                      'checkpoints': bytearray(), 'volume': 0.8}
            with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                expected = audio.make_sound(samples, 32000, params, -1, 3)
            source = (ctypes.c_int16 * len(samples)).from_buffer_copy(samples)
            output = ctypes.c_void_p()
            length = ctypes.c_size_t()
            self.assertTrue(native.dsnd_encode_simple(
                source, len(samples), 32000, 11025, 2, 1, 0.8, 0,
                -1, 3, ctypes.byref(output), ctypes.byref(length)))
            try:
                self.assertEqual(ctypes.string_at(output, length.value), expected)
            finally:
                native.dtex_free(output)

    def test_sound_bank_packages_match_python(self):
        with tempfile.TemporaryDirectory() as tmp:
            library = Path(tmp) / 'libassets.so'
            build_library(library)
            names = ['GENRL', 'SCRIPT', 'ñame']
            items = [(19, b'abc'), (1, b'x' * 4099), (7, b'')]
            lookup = [(255, 0), (0, 1), (2, 65535)]
            with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                expected = (audio.pack_sound_banks(items),
                            audio.pack_sound_lookup(names, lookup))
            with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                self.assertEqual(
                    (audio.pack_sound_banks(items),
                     audio.pack_sound_lookup(names, lookup)), expected)
            exe = Path(tmp) / 'audio_pack_cli'
            subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra',
                            '-Werror', str(ROOT / 'tools/portable/audio_pack.c'),
                            str(ROOT / 'tools/portable/audio_pack_cli.c'),
                            '-o', str(exe)], check=True)
            args = []
            for bank, data in items:
                path = Path(tmp) / f'{bank}.dsbk'
                path.write_bytes(data)
                args.extend((str(bank), str(path)))
            result = Path(tmp) / 'test.dsfx'
            subprocess.run([str(exe), str(result), *args], check=True)
            self.assertEqual(result.read_bytes(), expected[0])
            root = ROOT / 'game/audio/CONFIG'
            if (root / 'BankLkup.dat').is_file():
                with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                    packs, banks = audio.sfx_tables(root)
                    expected = audio.pack_sound_lookup(
                        packs, [(255, 0)] * len(banks))
                with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                    self.assertEqual(audio.pack_sound_lookup(
                        packs, [(255, 0)] * len(banks)), expected)

    @unittest.skipUnless(shutil.which('ffmpeg'), 'ffmpeg unavailable')
    def test_ogg_metadata_and_truncation(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            sample = root / 'synthetic.ogg'
            subprocess.run(['ffmpeg', '-nostdin', '-loglevel', 'error',
                            '-f', 'lavfi', '-i',
                            'sine=frequency=440:duration=1:sample_rate=22050',
                            '-c:a', 'libvorbis', str(sample)], check=True)
            library = root / 'libassets.so'
            build_library(library)
            from .portable import decode_ogg_vorbis, parse_ogg_vorbis_info
            with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                rate, samples, channels, small, large = parse_ogg_vorbis_info(
                    sample.read_bytes())
                self.assertEqual((rate, channels), (22050, 1))
                self.assertGreater(samples, 0)
                self.assertLessEqual(small, large)
                with self.assertRaises(ValueError):
                    parse_ogg_vorbis_info(sample.read_bytes()[:20])
                pcm, decoded_rate, decoded_channels = decode_ogg_vorbis(
                    sample.read_bytes())
                self.assertEqual((decoded_rate, decoded_channels), (22050, 1))
                with self.assertRaises(ValueError):
                    decode_ogg_vorbis(sample.read_bytes()[:20])
            exe = root / 'ogg_cli'
            subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra',
                            '-Werror', str(ROOT / 'tools/portable/ogg_decode.c'),
                            str(ROOT / 'tools/portable/ogg_cli.c'),
                            '-lm', '-o', str(exe)], check=True)
            cli_output = root / 'decoded.pcm'
            subprocess.run([str(exe), str(sample), str(cli_output)], check=True)
            self.assertEqual(cli_output.read_bytes(), pcm)
            reference = subprocess.run(
                ['ffmpeg', '-nostdin', '-loglevel', 'error', '-i', str(sample),
                 '-f', 's16le', '-ac', '1', '-ar', '22050', 'pipe:1'],
                capture_output=True, check=True).stdout
            self.assertEqual(len(pcm), len(reference))
            actual_samples = struct.unpack(f'<{len(pcm) // 2}h', pcm)
            expected_samples = struct.unpack(f'<{len(reference) // 2}h', reference)
            self.assertLessEqual(max(abs(a - b) for a, b in
                                     zip(actual_samples, expected_samples)), 1)

    @unittest.skipUnless(shutil.which('ffmpeg'), 'ffmpeg unavailable')
    def test_real_ogg_decodes_at_source_rate(self):
        root = ROOT / 'game/audio'
        if not (root / 'CONFIG/TrakLkup.dat').is_file():
            self.skipTest('local GTA stream lookup unavailable')
        packs, tracks = audio.stream_tables(root / 'CONFIG')
        pack, offset, size = tracks[0]
        source = root / 'streams' / packs[pack]
        if not source.is_file():
            self.skipTest('local GTA stream unavailable')
        with source.open('rb') as file:
            file.seek(offset)
            encrypted = file.read(size + audio.TRACK_HEADER)
        ogg = audio.decrypt(encrypted, offset)[audio.TRACK_HEADER:]
        with tempfile.TemporaryDirectory() as tmp:
            library = Path(tmp) / 'libassets.so'
            build_library(library)
            from .portable import decode_ogg_vorbis, parse_ogg_vorbis_info
            with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                rate, count, channels, _, _ = parse_ogg_vorbis_info(ogg)
                decoded, actual_rate, actual_channels = decode_ogg_vorbis(ogg)
            self.assertEqual((actual_rate, actual_channels), (rate, channels))
            self.assertEqual(len(decoded), count * channels * 2)
            reference = subprocess.run(
                ['ffmpeg', '-nostdin', '-loglevel', 'error', '-i', 'pipe:0',
                 '-f', 's16le', '-ac', str(channels), '-ar', str(rate), 'pipe:1'],
                input=ogg, capture_output=True, check=True).stdout
            self.assertEqual(len(decoded), len(reference))
            actual_samples = struct.unpack(f'<{len(decoded) // 2}h', decoded)
            expected_samples = struct.unpack(f'<{len(reference) // 2}h', reference)
            self.assertLessEqual(max(abs(a - b) for a, b in
                                     zip(actual_samples, expected_samples)), 1)

    @unittest.skipUnless(shutil.which('ffmpeg'), 'ffmpeg unavailable')
    def test_real_stream_encoding_native_and_cli(self):
        root = ROOT / 'game/audio'
        if not (root / 'CONFIG/TrakLkup.dat').is_file():
            self.skipTest('local GTA stream lookup unavailable')
        packs, tracks = audio.stream_tables(root / 'CONFIG')
        pack, offset, size = tracks[0]
        source = root / 'streams' / packs[pack]
        if not source.is_file():
            self.skipTest('local GTA stream unavailable')
        with source.open('rb') as file:
            file.seek(offset)
            encrypted = file.read(size + audio.TRACK_HEADER)
        params = {**audio.DEFAULTS, 'sample_rate': 11025}
        with tempfile.TemporaryDirectory() as tmp:
            library = Path(tmp) / 'libassets.so'
            build_library(library)
            with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                expected, _ = audio.convert_track(encrypted, offset, params)
            with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                actual, _ = audio.convert_track(encrypted, offset, params)
            self.assertEqual(actual[:24], expected[:24])
            self.assertEqual(len(actual), len(expected))
            exe = Path(tmp) / 'audio_stream_cli'
            files = ('dsnd.c', 'resample.c', 'adpcm.c', 'ogg_decode.c',
                     'ogg_info.c', 'audio_stream.c', 'audio_stream_cli.c')
            subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                            *(str(ROOT / 'tools/portable' / name) for name in files),
                            '-lm', '-o', str(exe)], check=True)
            input_file = Path(tmp) / 'track.enc'
            output_file = Path(tmp) / 'track.dsnd'
            input_file.write_bytes(encrypted)
            subprocess.run([str(exe), str(input_file), str(output_file),
                            str(offset)], check=True)
            self.assertEqual(output_file.read_bytes(), actual)

    def test_sound_cli_matches_python(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            exe = root / 'audio_cli'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2',
                            str(ROOT / 'tools/portable/dsnd.c'),
                            str(ROOT / 'tools/portable/resample.c'),
                            str(ROOT / 'tools/portable/adpcm.c'),
                            str(ROOT / 'tools/portable/audio_cli.c'),
                            '-lm', '-o', str(exe)], check=True)
            samples = array('h', (((i * 761) % 60000) - 30000 for i in range(8195)))
            pcm = root / 'sound.pcm'
            pcm.write_bytes(samples.tobytes())
            result = root / 'sound.dsnd'
            subprocess.run([str(exe), 'sound', str(pcm), str(result), '22050',
                            '11025', 'adpcm', '0.8', '0.25', '3'], check=True)
            with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                expected = audio.make_sound(samples, 22050,
                                            {**audio.DEFAULTS, 'volume': 0.8,
                                             'max_seconds': 0.25}, 3)
            self.assertEqual(result.read_bytes(), expected)

    def test_stream_decryption_and_offsets_match_python(self):
        with tempfile.TemporaryDirectory() as tmp:
            library = Path(tmp) / 'libassets.so'
            build_library(library)
            for pos in (0, 7, 8191):
                source = bytes(range(256))
                with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                    expected = audio.decrypt(source, pos)
                with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                    self.assertEqual(audio.decrypt(source, pos), expected)
            root = ROOT / 'game/audio'
            if (root / 'CONFIG/TrakLkup.dat').is_file():
                with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                    reference_sfx = audio.sfx_tables(root / 'CONFIG')
                    reference_streams = audio.stream_tables(root / 'CONFIG')
                with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                    self.assertEqual(audio.sfx_tables(root / 'CONFIG'), reference_sfx)
                    self.assertEqual(audio.stream_tables(root / 'CONFIG'),
                                     reference_streams)
                names, entries = audio.stream_tables(root / 'CONFIG')
                compared = 0
                for pack, offset, _ in entries:
                    if pack >= len(names):
                        continue
                    path = root / 'streams' / names[pack]
                    if not path.is_file():
                        continue
                    with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                        try:
                            expected = audio.aligned_track_offset(path, offset)
                        except ValueError:
                            continue
                    with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                        self.assertEqual(audio.aligned_track_offset(path, offset),
                                         expected, f'{path.name}:{offset}')
                    compared += 1
                    if compared == 5:
                        break
                self.assertEqual(compared, 5)

    def test_stream_package_matches_python(self):
        with tempfile.TemporaryDirectory() as tmp:
            library = Path(tmp) / 'libassets.so'
            build_library(library)
            pcm = array('h', (i * 51 - 25500 for i in range(1000)))
            for seekable in (False, True):
                sounds = []
                with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                    for track, length in ((12, 750), (2, 100), (12, 100)):
                        params = {**audio.DEFAULTS,
                                  'checkpoints': bytearray() if seekable else None}
                        sounds.append((track, audio.make_sound(pcm[:length], 11025, params)))
                    expected = audio.pack_streams(sounds, seekable)
                with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                    self.assertEqual(audio.pack_streams(sounds, seekable), expected)

    def test_real_sound_banks_match_python(self):
        root = ROOT / 'game/audio'
        if not (root / 'CONFIG/BankLkup.dat').is_file():
            self.skipTest('local GTA installation unavailable')
        names, entries = audio.sfx_tables(root / 'CONFIG')
        with tempfile.TemporaryDirectory() as tmp:
            library = Path(tmp) / 'libassets.so'
            build_library(library)
            compared = 0
            for pack, off, size in entries:
                if size < audio.BANK_HEADER or size > 150000 or pack >= len(names):
                    continue
                path = root / 'sfx' / names[pack]
                if not path.is_file():
                    continue
                with path.open('rb') as source:
                    source.seek(off)
                    raw = source.read(size)
                if len(raw) != size:
                    continue
                params = {**audio.DEFAULTS, 'sample_rate': 11025}
                with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                    try:
                        expected = audio.convert_bank(raw, params)
                    except (ValueError, struct.error, ZeroDivisionError):
                        continue
                with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                    self.assertEqual(audio.convert_bank(raw, params), expected,
                                     f'{names[pack]} offset {off}')
                compared += 1
                if compared == 20:
                    break
            self.assertEqual(compared, 20)

    def test_dsnd_resampling_volume_and_formats_match_python(self):
        with tempfile.TemporaryDirectory() as tmp:
            library = Path(tmp) / 'libassets.so'
            build_library(library)
            samples = array('h', (((i * 1471) % 60000) - 30000 for i in range(8195)))
            for fmt in ('pcm8', 'pcm16', 'adpcm'):
                for source, target in ((22050, 11025), (8000, 4000), (11025, 22050)):
                    for seekable in (False, True):
                        with self.subTest(fmt=fmt, source=source, seekable=seekable):
                            params = {**audio.DEFAULTS, 'sample_rate': target,
                                      'format': fmt, 'volume': 0.8, 'max_seconds': 0.5}
                            if seekable:
                                params['checkpoints'] = bytearray(b'\x01\x02\x03\x04')
                            with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                                expected = audio.make_sound(samples, source, params, 3, 5)
                                saved = bytes(params.get('checkpoints', b''))
                            if seekable:
                                params['checkpoints'] = bytearray(b'\x01\x02\x03\x04')
                            with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                                self.assertEqual(audio.make_sound(samples, source, params, 3, 5),
                                                 expected)
                            self.assertEqual(bytes(params.get('checkpoints', b'')), saved)

    def test_cli_matches_python(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            exe = work / 'adpcm_cli'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2',
                            str(ROOT / 'tools/portable/adpcm.c'),
                            str(ROOT / 'tools/portable/adpcm_cli.c'), '-o', str(exe)], check=True)
            samples = array('h', (int((i * 173) % 50000 - 25000) for i in range(25000)))
            source = work / 'samples.pcm'
            source.write_bytes(samples.tobytes())
            output, checkpoints = work / 'samples.adpcm', work / 'seek.bin'
            subprocess.run([str(exe), str(source), str(output), str(checkpoints)], check=True)
            expected_checkpoints = bytearray()
            expected = audio.adpcm_encode(samples, expected_checkpoints)
            self.assertEqual(output.read_bytes(), expected)
            self.assertEqual(checkpoints.read_bytes(), expected_checkpoints)

    def test_adpcm_and_seek_checkpoints_match_python(self):
        with tempfile.TemporaryDirectory() as tmp:
            library = Path(tmp) / 'libassets.so'
            build_library(library)
            randomizer = random.Random(42)
            for n in (0, 1, 2, 19, 8192, 8193, 25000):
                samples = array('h', (randomizer.randrange(-32768, 32768) for _ in range(n)))
                for seekable in (False, True):
                    params = {**audio.DEFAULTS, 'checkpoints': bytearray() if seekable else None}
                    with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                        expected = audio.make_sound(samples, 11025, params)
                    params['checkpoints'] = bytearray() if seekable else None
                    with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                        actual = audio.make_sound(samples, 11025, params)
                    self.assertEqual(actual, expected, f'n={n} seekable={seekable}')


if __name__ == '__main__':
    unittest.main()
