import shutil
import os
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from . import img
from .test_portable_build import build_library

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which('cc'), 'C compiler unavailable')
class PortableIMGTest(unittest.TestCase):
    def test_index_matches_python(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            exe = work / 'img_cli'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2',
                            str(ROOT / 'tools/portable/img.c'),
                            str(ROOT / 'tools/portable/img_cli.c'), '-o', str(exe)], check=True)
            fixture = work / 'test.img'
            fixture.write_bytes(b'VER2' + struct.pack('<I', 2) +
                                struct.pack('<IHH24s', 3, 1, 2, b'HELLO.TXD') +
                                struct.pack('<IHH24s', 5, 4, 0, b'other.dff'))
            library = work / 'libgtasads_assets.so'
            build_library(library)
            for path in (fixture, ROOT / 'game/models/gta3.img'):
                if not path.exists():
                    continue
                result = subprocess.run([str(exe), str(path)], capture_output=True, check=True)
                actual = [line.split('\t') for line in result.stdout.decode('latin1').splitlines()]
                self.assertEqual([(name, int(off), int(size)) for name, off, size in actual],
                                 img.read_toc(path))
                with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                    self.assertEqual(img.read_toc(path),
                                     [(name, int(off), int(size)) for name, off, size in actual])
            invalid = work / 'invalid.img'
            invalid.write_bytes(b'VER2' + struct.pack('<I', 999))
            self.assertNotEqual(subprocess.run([str(exe), str(invalid)], capture_output=True).returncode, 0)


if __name__ == '__main__':
    unittest.main()
