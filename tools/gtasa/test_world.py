import struct
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from . import world


class WorldRadiusTest(unittest.TestCase):
    def test_area_instances_and_interior_pack(self):
        with tempfile.TemporaryDirectory() as root:
            data = Path(root) / "data/maps/interior"
            data.mkdir(parents=True)
            models = Path(root) / "models"
            models.mkdir()
            (models / "gta3.img").touch()
            (models / "gta_int.img").touch()
            (Path(root) / "data/gta.dat").write_text(
                "IDE data/maps/objects.ide\nIPL data/maps/ex.ipl\nIPL data/maps/interior/room.ipl\n"
            )
            (Path(root) / "data/maps/objects.ide").write_text(
                "objs\n1, outside, outtex, 100, 0\n2, room, roomtex, 100, 0\nend\n"
            )
            (Path(root) / "data/maps/ex.ipl").write_text(
                "inst\n1, outside, 0, 10, 10, 10, 0, 0, 0, 1, -1\nend\n"
            )
            (data / "room.ipl").write_text(
                "inst\n2, room, 3, 20, 20, 1000, 0, 0, 0, 1, -1\nend\n"
            )
            def toc(path):
                return [(n, 0, 0) for n in (("outside.dff", "outtex.txd") if path.name == "gta3.img"
                                             else ("room.dff", "roomtex.txd"))]
            with patch.object(world.img, "read_toc", side_effect=toc), patch.object(world, "_radius", return_value=16), \
                 patch.object(world, "_col_models", return_value=({}, 0)):
                blob, stats = world.build(root, {"sector": 50})
            header = struct.unpack_from("<4sHHhhHHHHIIIII", blob)
            self.assertEqual((header[0], header[1], header[9]), (b"DWLD", 6, 2))
            instances = list(struct.iter_unpack("<3i9hHHHHH", blob[header[12]:header[13]]))
            self.assertEqual({instance[-2] for instance in instances}, {0, 3})
            self.assertEqual([struct.unpack_from("<I", blob, header[10] + i * 36 + 32)[0] >> 31 for i in range(2)], [0, 1])
            self.assertEqual([struct.unpack_from("<H", blob, header[10] + 36 * 2 + i * 28 + 26)[0] for i in range(2)], [0, 1])
            self.assertEqual(stats["instances"], 2)

    def test_transformed_atomic_and_parent(self):
        identity = (1, 0, 0, 0, 1, 0, 0, 0, 1)
        model = SimpleNamespace(
            frames=[
                SimpleNamespace(rot=identity, pos=(10, 0, 0), parent=-1),
                SimpleNamespace(rot=identity, pos=(0, 5, 0), parent=0),
            ],
            geometries=[SimpleNamespace(sphere=(1, 0, 0, 2))],
            atomics=[(1, 0)],
        )
        with patch.object(world.img, 'read_entry', return_value=b''), patch.object(world.dff, 'parse', return_value=model):
            self.assertEqual(world._radius('', 0, 0), 15)

    def test_reused_geometry_counts_each_atomic(self):
        identity = (1, 0, 0, 0, 1, 0, 0, 0, 1)
        model = SimpleNamespace(
            frames=[
                SimpleNamespace(rot=identity, pos=(0, 0, 0), parent=-1),
                SimpleNamespace(rot=identity, pos=(20, 0, 0), parent=-1),
            ],
            geometries=[SimpleNamespace(sphere=(0, 0, 0, 2))],
            atomics=[(0, 0), (1, 0)],
        )
        with patch.object(world.img, 'read_entry', return_value=b''), patch.object(world.dff, 'parse', return_value=model):
            self.assertEqual(world._radius('', 0, 0), 22)


if __name__ == '__main__':
    unittest.main()
