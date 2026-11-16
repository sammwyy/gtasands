import json
import shutil
import subprocess
import tempfile
import unittest
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which('node'), 'Node.js unavailable')
class WebZipTest(unittest.TestCase):
    def test_converted_assets_package_has_sd_layout(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / 'sd.zip'
            script = f"""
                import {{packageConverted}} from
                    {json.dumps((ROOT / 'web/package.mjs').as_uri())};
                import {{writeFileSync}} from 'node:fs';
                const chunks = [];
                const destination = {{
                    write: async data => chunks.push(Buffer.from(data)),
                    close: async () => writeFileSync({json.dumps(str(output))},
                                                     Buffer.concat(chunks))
                }};
                const files = [
                    'models/gta3.dspk', 'models/gta_int.dspk',
                    'data/world.dwld', 'data/vehicles.dveh', 'data/peds.dped',
                    'data/map_points.dpoi'
                ].map(path => ({{path, file: new Blob([path])}}));
                files.push({{path: '.stamp', file: new Blob(['ignored'])}});
                await packageConverted({{
                    variant: 'nds', rom: Uint8Array.of(1, 2, 3),
                    files, destination
                }});
            """
            subprocess.run(['node', '--input-type=module', '-e', script], check=True)
            with zipfile.ZipFile(output) as archive:
                self.assertIsNone(archive.testzip())
                self.assertEqual(archive.read('gtasads.nds'), bytes([1, 2, 3]))
                self.assertEqual(archive.read('gtasads/data/world.dwld'),
                                 b'data/world.dwld')
                self.assertNotIn('gtasads/.stamp', archive.namelist())
                self.assertEqual(len(archive.namelist()), 7)

    def test_streamed_package_is_valid_and_rejects_unsafe_paths(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / 'package.zip'
            script = f"""
                import {{ZipWriter}} from {json.dumps((ROOT / 'web/zip.mjs').as_uri())};
                import {{writeFileSync}} from 'node:fs';
                const pieces = [];
                const writer = new ZipWriter({{
                    write: async bytes => pieces.push(Buffer.from(bytes)),
                    close: async () => writeFileSync({json.dumps(str(output))},
                                                     Buffer.concat(pieces))
                }});
                await writer.add('gtasads.dsi', new TextEncoder().encode('DSI'));
                await writer.add('gtasads/data/assets.dspk', (async function* () {{
                    yield Uint8Array.from([0, 1, 2]);
                    yield Uint8Array.from([3, 4, 255]);
                }})());
                await writer.add('gtasads/data/á.dat', new Uint8Array());
                for (const name of ['../source.img', '/absolute', 'gtasads/../bad',
                                    'gtasads/data/assets.dspk']) {{
                    try {{ await writer.add(name, new Uint8Array()); throw Error(name); }}
                    catch (error) {{
                        if (error.message === name) throw error;
                    }}
                }}
                await writer.close();
            """
            subprocess.run(['node', '--input-type=module', '-e', script], check=True)
            with zipfile.ZipFile(output) as archive:
                self.assertIsNone(archive.testzip())
                self.assertEqual(archive.namelist(),
                                 ['gtasads.dsi', 'gtasads/data/assets.dspk',
                                  'gtasads/data/á.dat'])
                self.assertEqual(archive.read('gtasads.dsi'), b'DSI')
                self.assertEqual(archive.read('gtasads/data/assets.dspk'),
                                 bytes([0, 1, 2, 3, 4, 255]))
                self.assertEqual(archive.read('gtasads/data/á.dat'), b'')
                self.assertEqual(archive.getinfo('gtasads.dsi').date_time,
                                 (1980, 1, 1, 0, 0, 0))
