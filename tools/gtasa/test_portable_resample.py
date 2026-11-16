import ctypes
import math
import random
import shutil
import subprocess
import sys
import tempfile
import unittest
from array import array
from pathlib import Path

from .resample import downsample


ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which('cc'), 'C compiler unavailable')
class PortableResampleTest(unittest.TestCase):
    def test_generated_lookup_table_is_current(self):
        with tempfile.TemporaryDirectory() as tmp:
            generated = Path(tmp) / 'resample_coeffs.h'
            subprocess.run([sys.executable,
                            str(ROOT / 'tools/generate_resample_table.py'),
                            '--output', str(generated)], check=True)
            self.assertEqual(generated.read_bytes(),
                             (ROOT / 'tools/portable/resample_coeffs.h').read_bytes())

    def test_native_matches_python_across_rates_and_boundaries(self):
        with tempfile.TemporaryDirectory() as tmp:
            library = Path(tmp) / 'resample.so'
            subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra',
                            '-Werror', '-shared', '-fPIC',
                            str(ROOT / 'tools/portable/resample.c'),
                            '-o', str(library)], check=True)
            native = ctypes.CDLL(str(library))
            native.resample_pcm16.argtypes = (
                ctypes.POINTER(ctypes.c_int16), ctypes.c_size_t,
                ctypes.c_uint32, ctypes.c_uint32,
                ctypes.POINTER(ctypes.POINTER(ctypes.c_int16)),
                ctypes.POINTER(ctypes.c_size_t))
            native.resample_pcm16.restype = ctypes.c_int
            libc = ctypes.CDLL(None)
            libc.free.argtypes = (ctypes.c_void_p,)
            generator = random.Random(1234)
            for source_rate, target_rate in (
                    (22050, 11025), (32000, 11025), (44100, 16000),
                    (48000, 22050), (96000, 11025), (32000, 8000)):
                for count in (0, 1, 17, 257, 4096):
                    with self.subTest(source_rate=source_rate,
                                      target_rate=target_rate, count=count):
                        samples = array('h', (generator.randint(-32768, 32767)
                                              for _ in range(count)))
                        inputs = (ctypes.c_int16 * count).from_buffer_copy(samples)
                        output = ctypes.POINTER(ctypes.c_int16)()
                        length = ctypes.c_size_t()
                        self.assertTrue(native.resample_pcm16(
                            inputs, count, source_rate, target_rate,
                            ctypes.byref(output), ctypes.byref(length)))
                        try:
                            result = ctypes.string_at(output, length.value * 2)
                        finally:
                            libc.free(output)
                        self.assertEqual(result,
                                         downsample(samples, source_rate,
                                                    target_rate).tobytes())

    def test_downsampling_suppresses_aliasing(self):
        source_rate, target_rate = 32000, 11025
        def rms(data):
            middle = data[300:-300]
            return math.sqrt(sum(x * x for x in middle) / len(middle))
        low = array('h', (round(math.sin(i * 2 * math.pi * 1000 / source_rate) *
                                12000) for i in range(source_rate)))
        high = array('h', (round(math.sin(i * 2 * math.pi * 8000 / source_rate) *
                                 12000) for i in range(source_rate)))
        self.assertAlmostEqual(rms(downsample(low, source_rate, target_rate)) /
                               rms(low), 1.0, delta=0.01)
        self.assertLess(rms(downsample(high, source_rate, target_rate)), 10)
