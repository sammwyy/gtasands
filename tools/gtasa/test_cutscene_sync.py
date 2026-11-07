import struct
import tempfile
import unittest
from pathlib import Path

from .pipeline import sync


class CutsceneSyncTest(unittest.TestCase):
    def test_extracts_selected_entries_from_case_insensitive_archive(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / 'source'
            game = root / 'game'
            archive = source / 'ANIM/CUTS.IMG'
            archive.parent.mkdir(parents=True)
            header = struct.pack('<4sI', b'VER2', 2)
            header += struct.pack('<IHH24s', 1, 1, 1, b'PROLOG3.DAT')
            header += struct.pack('<IHH24s', 2, 1, 1, b'OTHER.DAT')
            archive.write_bytes(header.ljust(2048, b'\0')
                                + b'camera'.ljust(2048, b'\0')
                                + b'other'.ljust(2048, b'\0'))

            class Config:
                sections = {'cutscene_files': {
                    'kind': 'files', 'src': ['anim/cuts/*.dat'],
                    'from_img': 'anim/cuts.img', 'names': ['prolog3'],
                }}

            cfg = Config()
            cfg.source, cfg.game = source, game
            sync(cfg)
            self.assertEqual((game / 'anim/cuts/prolog3.dat').read_bytes()[:6], b'camera')
            self.assertFalse((game / 'anim/cuts/other.dat').exists())


if __name__ == '__main__':
    unittest.main()
