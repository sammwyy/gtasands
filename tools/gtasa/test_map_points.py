import struct
import tempfile
import unittest
from pathlib import Path

from . import map_points


class MapPointsTest(unittest.TestCase):
    def test_extracts_only_real_exterior_entrances(self):
        with tempfile.TemporaryDirectory() as root:
            maps = Path(root) / "data/maps"
            maps.mkdir(parents=True)
            (Path(root) / "data/gta.dat").write_text(
                "IPL DATA\\MAPS\\city.IPL\nIPL DATA\\MAPS\\interior\\room.IPL\n"
            )
            (maps / "city.ipl").write_text(
                'inst\n0, building\nend\nenex\n'
                '100.2, -200.8, 12, 0, 1, 1, 8, 0, 0, 0, 0, 0, 4, "AMMUN1", 0, 2, 0, 24\n'
                '100.2, -200.8, 12, 0, 1, 1, 8, 0, 0, 0, 0, 0, 4, "AMMUN1", 0, 2, 0, 24\n'
                '200, -300, 12, 0, 1, 1, 8, 0, 0, 0, 0, 0, 4, "BAR2", 0, 2, 0, 24\nend\n'
            )
            (maps / "interior").mkdir()
            (maps / "interior/room.ipl").write_text(
                'enex\n10, 20, 1000, 0, 1, 1, 8, 0, 0, 0, 0, 5, 0, "GYM1", 0, 2, 0, 24\nend\n'
            )
            data, stats = map_points.build(root, {})
            self.assertEqual(struct.unpack_from("<4sHH", data), (b"DPOI", 2, 2))
            self.assertEqual(struct.unpack_from("<hhBB", data, 8), (100, -201, 0, 0))
            self.assertEqual(struct.unpack_from("<hhBB", data, 14), (200, -300, 12, 0))
            self.assertEqual(stats["entrances"], 2)


if __name__ == "__main__":
    unittest.main()
