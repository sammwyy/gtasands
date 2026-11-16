import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PORTABLE = ROOT / 'tools/portable'


@unittest.skipUnless(shutil.which('ffmpeg') and shutil.which('cc'),
                     'ffmpeg or C compiler unavailable')
class PortableMPEGTest(unittest.TestCase):
    def test_synthetic_mpeg_converts_video_audio_and_movie(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / 'sample.mpg'
            subprocess.run(['ffmpeg', '-nostdin', '-loglevel', 'error',
                            '-f', 'lavfi', '-i',
                            'testsrc=size=64x48:rate=30:duration=1',
                            '-f', 'lavfi', '-i',
                            'sine=frequency=440:sample_rate=44100:duration=1',
                            '-c:v', 'mpeg1video', '-q:v', '5',
                            '-c:a', 'mp2', '-b:a', '128k', '-shortest',
                            str(source)], check=True)
            def compile_cli(name, sources):
                exe = root / name
                subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra',
                                '-Werror', *(str(PORTABLE / file)
                                             for file in sources),
                                '-lm', '-o', str(exe)], check=True)
                return exe
            video_cli = compile_cli('mpeg_video_cli',
                                    ('mpeg_video.c', 'mpeg_video_cli.c'))
            video = root / 'frames.yuv'
            subprocess.run([str(video_cli), str(source), str(video), '10'],
                           check=True)
            reference = subprocess.run(
                ['ffmpeg', '-nostdin', '-loglevel', 'error', '-i', str(source),
                 '-frames:v', '10', '-pix_fmt', 'yuv420p', '-f', 'rawvideo',
                 'pipe:1'], capture_output=True, check=True).stdout
            decoded = video.read_bytes()
            self.assertEqual(len(decoded), len(reference))
            self.assertLessEqual(max(abs(a - b) for a, b in
                                     zip(decoded, reference)), 8)
            audio_cli = compile_cli('mpeg_audio_cli',
                                    ('mpeg_video.c', 'mpeg_audio.c',
                                     'resample.c', 'mpeg_audio_cli.c'))
            audio = root / 'sound.pcm'
            subprocess.run([str(audio_cli), str(source), str(audio),
                            '22050'], check=True)
            self.assertGreater(len(audio.read_bytes()), 22050)
            movie_cli = compile_cli('mpeg_movie_cli',
                                    ('mpeg_video.c', 'mpeg_audio.c',
                                     'movie_palette.c', 'dvid.c',
                                     'resample.c', 'mpeg_movie.c',
                                     'mpeg_movie_cli.c'))
            movie = root / 'movie.dvid'
            subprocess.run([str(movie_cli), str(source), str(movie)],
                           check=True)
            converted = movie.read_bytes()
            self.assertEqual(struct.unpack_from('<4sHHHHHxxII', converted),
                             (b'DVID', 1, 256, 256, 10, 0, 10, 22050))
            self.assertGreater(len(converted), 10 * 65536)
            self.assertGreater(len(set(converted[24:536])), 10)

    def test_real_video_is_close_to_ffmpeg(self):
        source = ROOT / 'game/movies/GTAtitles.mpg'
        if not source.is_file():
            self.skipTest('local GTA movie unavailable')
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / 'mpeg_video_cli'
            subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra',
                            '-Werror', str(PORTABLE / 'mpeg_video.c'),
                            str(PORTABLE / 'mpeg_video_cli.c'),
                            '-lm', '-o', str(exe)], check=True)
            output = Path(tmp) / 'frames.yuv'
            subprocess.run([str(exe), str(source), str(output), '50'],
                           check=True)
            reference = subprocess.run(
                ['ffmpeg', '-nostdin', '-loglevel', 'error', '-i', str(source),
                 '-frames:v', '50', '-pix_fmt', 'yuv420p', '-f', 'rawvideo',
                 'pipe:1'], capture_output=True, check=True).stdout
            decoded = output.read_bytes()
            self.assertEqual(len(decoded), len(reference))
            self.assertLessEqual(max(abs(a - b) for a, b in
                                     zip(decoded, reference)), 8)
