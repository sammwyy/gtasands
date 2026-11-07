import struct
import tempfile
import unittest
from pathlib import Path

from .pipeline import Builder, Config, sync


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
            models = source / 'MODELS/CUTSCENE.IMG'
            models.parent.mkdir(parents=True)
            models.write_bytes((struct.pack('<4sI', b'VER2', 1)
                                + struct.pack('<IHH24s', 1, 1, 1, b'CSSTEW.DFF')).ljust(2048, b'\0')
                               + b'actor'.ljust(2048, b'\0'))

            class Config:
                sections = {'cutscene_files': {
                    'kind': 'files', 'src': ['anim/cuts/*.dat'],
                    'from_img': 'anim/cuts.img', 'names': ['prolog3'],
                }, 'cutscene_models': {
                    'kind': 'files', 'src': ['models/cutscene/*.dff'],
                    'from_img': 'models/cutscene.img', 'names': ['csstew'],
                    'extract_to': 'models/cutscene',
                    'suffixes': ['.dff', '.txd'],
                }}

            cfg = Config()
            cfg.source, cfg.game = source, game
            sync(cfg)
            self.assertEqual((game / 'anim/cuts/prolog3.dat').read_bytes()[:6], b'camera')
            self.assertFalse((game / 'anim/cuts/other.dat').exists())
            self.assertEqual((game / 'models/cutscene/csstew.dff').read_bytes()[:5], b'actor')

    def test_packs_selected_files_without_duplicate_loose_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cfg_path = root / 'resources.toml'
            cfg_path.write_text('[global]\ngame_dir = "game"\nout_dir = "nitrofs"\n'
                                'cache_dir = "build/cache"\njobs = 1\n\n'
                                '[cutscene_models]\nkind = "files"\n'
                                'src = ["models/cutscene/*.dat"]\nkeep_paths = false\n'
                                'pack = "models/cutscene.dspk"\n')
            source = root / 'game/models/cutscene'
            source.mkdir(parents=True)
            (source / 'scene.dat').write_bytes(b'original')
            (root / 'build').mkdir()
            Builder(Config(cfg_path)).build(only=['cutscene_models'])
            packed = (root / 'nitrofs/models/cutscene.dspk').read_bytes()
            self.assertEqual(packed[:4], b'DSPK')
            self.assertIn(b'original', packed)
            self.assertFalse((root / 'nitrofs/models/cutscene/scene.dat').exists())


if __name__ == '__main__':
    unittest.main()
