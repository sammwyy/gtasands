import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

from .pipeline import Config, sync
ROOT = Path(__file__).resolve().parents[2]


class PackageSDTest(unittest.TestCase):
    def test_world_sync_links_both_archives(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / 'source'
            models = source / 'models'
            models.mkdir(parents=True)
            (models / 'gta3.img').write_bytes(b'world')
            (models / 'gta_int.img').write_bytes(b'interiors')
            cfg = Config(ROOT / 'resources.toml')
            cfg.source, cfg.game = source, root / 'staged'
            sync(cfg, only=['world'], link=True)
            self.assertEqual((cfg.game / 'models/gta3.img').read_bytes(), b'world')
            self.assertEqual((cfg.game / 'models/gta_int.img').read_bytes(), b'interiors')

    def test_linked_audio_tables_preserve_names_expected_by_converter(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / 'source'
            cfgdir = source / 'AUDIO/CONFIG'
            cfgdir.mkdir(parents=True)
            (source / 'AUDIO/SFX').mkdir()
            (cfgdir / 'BANKLKUP.DAT').write_bytes(b'\0' * 12)
            (cfgdir / 'PAKFILES.DAT').write_bytes(b'FEET'.ljust(52, b'\0'))
            cfg = Config(ROOT / 'resources.toml')
            cfg.source, cfg.game = source, root / 'staged'
            sync(cfg, only=['audio_sfx'], link=True)
            self.assertEqual((cfg.game / 'audio/CONFIG/PakFiles.dat').read_bytes()[:4], b'FEET')
            self.assertEqual((cfg.game / 'audio/CONFIG/BankLkup.dat').read_bytes(), b'\0' * 12)

    def test_builds_from_original_directory_without_copying_to_game(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            source = work / 'SA'
            (source / 'TEXT').mkdir(parents=True)
            (source / 'TEXT/AMERICAN.GXT').write_bytes(b'english')
            (source / 'TEXT/SPANISH.GXT').write_bytes(b'spanish')
            rom = work / 'code.nds'
            rom.write_bytes(b'code')
            output = work / 'package.zip'
            subprocess.run([sys.executable, str(ROOT / 'tools/package_sd.py'), str(source),
                            '--rom', str(rom), '--output', str(output),
                            '--section', 'text', '-j', '1'], check=True, cwd=work)
            with zipfile.ZipFile(output) as archive:
                self.assertEqual(archive.read('gtasads.nds'), b'code')
                self.assertEqual(archive.read('gtasads/text/american.gxt'), b'english')
                self.assertEqual(archive.read('gtasads/text/spanish.gxt'), b'spanish')
            self.assertFalse((work / 'game').exists())
            again = subprocess.run([sys.executable, str(ROOT / 'tools/package_sd.py'), str(source),
                                    '--rom', str(rom), '--output', str(output),
                                    '--section', 'text', '-j', '1'], capture_output=True, text=True)
            self.assertNotEqual(again.returncode, 0)
            self.assertIn('already exists', again.stderr)

    def test_missing_required_img_does_not_leave_partial_package(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            source = work / 'SA'
            source.mkdir()
            rom = work / 'code.dsi'
            rom.write_bytes(b'code')
            output = work / 'package.zip'
            result = subprocess.run([sys.executable, str(ROOT / 'tools/package_sd.py'), str(source),
                                     '--rom', str(rom), '--output', str(output),
                                     '--section', 'anim_img', '-j', '1'], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(output.exists())


if __name__ == '__main__':
    unittest.main()
