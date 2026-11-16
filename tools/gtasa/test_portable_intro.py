import os
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from . import intro
from .test_portable_build import build_library


ROOT = Path(__file__).resolve().parents[2]


def expected_video(palette, frames, audio, fps, rate):
    pal = [(palette[i] >> 3) | ((palette[i + 1] >> 3) << 5) |
           ((palette[i + 2] >> 3) << 10) for i in range(0, 768, 3)]
    pixels = intro.W * intro.H
    stride = pixels + 1024 if len(frames) % pixels and len(frames) % (pixels + 1024) == 0 else pixels
    count = len(frames) // stride
    output = bytearray(struct.pack('<4sHHHHHxxII', b'DVID', 1, intro.W,
                                   intro.H, fps, 0, count, rate))
    output += struct.pack('<256H', *pal)
    consumed = 0
    for i in range(count):
        next_pos = min(len(audio), int((i + 1) * (rate * 2.0 / fps)))
        chunk = audio[consumed:next_pos]
        consumed = next_pos
        if len(chunk) % 2:
            chunk += b'\0'
        output += struct.pack('<I', len(chunk)) + chunk
        output += frames[i * stride:i * stride + pixels]
    if consumed < len(audio):
        output += struct.pack('<I', len(audio) - consumed) + audio[consumed:]
    output += struct.pack('<I', 0)
    return bytes(output)


@unittest.skipUnless(shutil.which('cc'), 'C compiler unavailable')
class PortableIntroTest(unittest.TestCase):
    def test_intro_cache_preserves_outputs_and_size_report(self):
        from .pipeline import Builder, Config
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            movies = root / 'game/movies'
            movies.mkdir(parents=True)
            (movies / 'Logo.mpg').write_bytes(b'local movie fixture')
            cfg = Config(ROOT / 'resources.toml')
            cfg.game, cfg.out = root / 'game', root / 'output'
            section = cfg.sections['movies']
            def state():
                return {'kind': 'intro', 'in': 0, 'out': 0, 'files': 0,
                        'removed': 0, 'by_type': {}, 'stats': {}, 'errors': []}
            def fake_convert(_src, dst, _fps, _rate):
                Path(dst).write_bytes(b'DVID' + bytes(12))
                return 1
            first = Builder(cfg)
            first_state = state()
            with patch.object(intro, 'convert', side_effect=fake_convert):
                first.do_intro('movies', section, first_state)
            self.assertEqual(first_state['out'], 16)
            self.assertEqual(first.outputs, ['movies/logo.dvid'])
            cached = Builder(cfg)
            cached_state = state()
            with patch.object(intro, 'convert',
                              side_effect=AssertionError('cache missed')):
                cached.do_intro('movies', section, cached_state)
            self.assertEqual(cached_state['out'], 16)
            self.assertEqual(cached.outputs, first.outputs)

    def test_synthetic_dvid_matches_python(self):
        with tempfile.TemporaryDirectory() as tmp:
            library = Path(tmp) / 'libassets.so'
            build_library(library)
            palette = bytes(range(256)) * 3
            frames = b''.join(bytes([i]) * (intro.W * intro.H) + bytes([255 - i]) * 1024
                              for i in range(2))
            exe = Path(tmp) / 'dvid_cli'
            subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra',
                            '-Werror', str(ROOT / 'tools/portable/dvid.c'),
                            str(ROOT / 'tools/portable/dvid_cli.c'),
                            '-o', str(exe)], check=True)
            for frame_data, audio, fps, rate in (
                    (frames, b'\x01\x02\x03\x04\x05', 7, 3),
                    (frames[:intro.W * intro.H], bytes(range(100)), 10, 22050),
                    (b'', b'\x01\x02\x03', 11, 32000)):
                with self.subTest(frames=len(frame_data), audio=len(audio)):
                    with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                        from .portable import pack_video
                        actual, count = pack_video(palette, frame_data, audio, fps, rate)
                    self.assertEqual(actual, expected_video(palette, frame_data, audio, fps, rate))
                    self.assertEqual(struct.unpack_from('<I', actual, 16)[0], count)
                    for name, data in (('palette.rgb', palette),
                                       ('frames.pal', frame_data),
                                       ('audio.pcm', audio)):
                        (Path(tmp) / name).write_bytes(data)
                    output = Path(tmp) / f'video-{len(frame_data)}-{len(audio)}.dvid'
                    subprocess.run([str(exe), str(Path(tmp) / 'palette.rgb'),
                                    str(Path(tmp) / 'frames.pal'),
                                    str(Path(tmp) / 'audio.pcm'), str(output),
                                    str(fps), str(rate)], check=True)
                    self.assertEqual(output.read_bytes(), actual)

    @unittest.skipUnless(shutil.which('ffmpeg'), 'ffmpeg unavailable')
    def test_real_intro_native_conversion_is_playable_format(self):
        source = ROOT / 'game/movies/Logo.mpg'
        if not source.is_file():
            self.skipTest('local movie unavailable')
        with tempfile.TemporaryDirectory() as tmp:
            library = Path(tmp) / 'libassets.so'
            build_library(library)
            baseline = Path(tmp) / 'python.dvid'
            native = Path(tmp) / 'native.dvid'
            with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                count = intro.convert(source, baseline)
            with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                native_count = intro.convert(source, native)
            raw = native.read_bytes()
            self.assertLessEqual(abs(native_count - count), 2)
            self.assertEqual(struct.unpack_from('<4sHHHHHxxII', raw),
                             (b'DVID', 1, 256, 256, 10, 0, native_count, 22050))
            self.assertGreater(len(raw), native_count * intro.W * intro.H)
            self.assertGreater(sum(byte != 0 for byte in raw[24:536]), 100)
            exe = Path(tmp) / 'mpeg_movie_cli'
            sources = ('mpeg_video.c', 'mpeg_audio.c', 'mpeg_movie.c',
                       'movie_palette.c', 'dvid.c', 'resample.c',
                       'mpeg_movie_cli.c')
            subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                            *(str(ROOT / 'tools/portable' / s) for s in sources),
                            '-lm', '-o', str(exe)], check=True)
            cli_output = Path(tmp) / 'cli.dvid'
            subprocess.run([str(exe), str(source), str(cli_output)], check=True)
            self.assertEqual(cli_output.read_bytes(), raw)

    def test_real_titles_have_complete_dvid_records(self):
        source = ROOT / 'game/movies/GTAtitles.mpg'
        if not source.is_file():
            self.skipTest('local movie unavailable')
        with tempfile.TemporaryDirectory() as tmp:
            library = Path(tmp) / 'libassets.so'
            build_library(library)
            from .portable import convert_movie
            with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                blob, frames = convert_movie(source.read_bytes())
            self.assertGreater(frames, 100)
            self.assertEqual(struct.unpack_from('<4sHHHHHxxII', blob),
                             (b'DVID', 1, 256, 256, 10, 0, frames, 22050))
            offset, audio_bytes = 536, 0
            for _ in range(frames):
                size, = struct.unpack_from('<I', blob, offset)
                self.assertEqual(size % 2, 0)
                audio_bytes += size
                offset += 4 + size + 256 * 256
                self.assertLessEqual(offset, len(blob))
            self.assertGreater(audio_bytes, 0)
            self.assertEqual(blob[offset:], b'\0\0\0\0')
