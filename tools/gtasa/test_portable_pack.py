import shutil
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from .dsfmt import write_pack
from .test_portable_build import build_library

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which('cc'), 'C compiler unavailable')
class PortablePackTest(unittest.TestCase):
    def test_native_pipeline_packs_match_python(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            library = work / 'libassets.so'
            build_library(library)
            for entries in ({}, {'Z.TXD': b'a', 'lower.dff': b'bb', 'empty.col': b''},
                            {'foo.dat': bytes(range(256)) * 320, 'FOO.DAT': b'upper',
                             'b.ifp': b'\x00\x01\x02'}):
                with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': ''}):
                    write_pack(work / 'python.dspk', entries)
                with patch.dict(os.environ, {'GTASADS_NATIVE_LIB': str(library)}):
                    write_pack(work / 'native.dspk', entries)
                self.assertEqual((work / 'native.dspk').read_bytes(),
                                 (work / 'python.dspk').read_bytes())

    def test_c_pack_matches_python_byte_for_byte(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            exe = work / 'dspk_cli'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2',
                            str(ROOT / 'tools/portable/dspk.c'),
                            str(ROOT / 'tools/portable/dspk_cli.c'), '-o', str(exe)], check=True)
            for entries in ({}, {'Z.TXD': b'a', 'lower.dff': b'bb', 'empty.col': b''},
                            {'foo.dat': bytes(range(256)) * 320, 'FOO.DAT': b'upper',
                             'b.ifp': b'\x00\x01\x02'}):
                args = []
                for i, (name, data) in enumerate(entries.items()):
                    path = work / f'entry{i}'
                    path.write_bytes(data)
                    args.extend([name, str(path)])
                c_file, py_file = work / 'c.dspk', work / 'py.dspk'
                subprocess.run([str(exe), str(c_file), *args], check=True)
                write_pack(py_file, entries)
                self.assertEqual(c_file.read_bytes(), py_file.read_bytes())


if __name__ == '__main__':
    unittest.main()
