import struct
import tempfile
import unittest
from pathlib import Path

from . import entry_exits


class EntryExitsTest(unittest.TestCase):
    def test_records_follow_gta_dat_and_keep_both_ends(self):
        with tempfile.TemporaryDirectory() as root:
            data = Path(root) / "data"
            maps = data / "maps"
            maps.mkdir(parents=True)
            (data / "gta.dat").write_text("IPL DATA\\MAPS\\outside.IPL\nIPL DATA\\MAPS\\inside.IPL\n")
            (maps / "outside.ipl").write_text(
                'enex\n100, 200, 10, 0, 2, 3, 8, 1, 2, 1000, 90, 1, 4, "HOUSE", 0, 2, 0, 24\nend\n'
            )
            (maps / "inside.ipl").write_text(
                'enex\n1, 2, 1000, 90, 2, 3, 8, 100, 200, 10, 180, 0, 0, "HOUSE", 0, 2, 0, 24\nend\n'
            )
            blob, stats = entry_exits.build(root, {})
            self.assertEqual(struct.unpack_from("<4sHH", blob), (b"DENX", 3, 2))
            first = entry_exits.RECORD.unpack_from(blob, 8)
            second = entry_exits.RECORD.unpack_from(blob, 8 + entry_exits.RECORD.size)
            self.assertEqual(first[0:3], (100, 200, 10))
            self.assertEqual(first[11:13], (4, 1))
            self.assertEqual(second[11:13], (0, 0))
            self.assertEqual(first[13].rstrip(b"\0"), b"house")
            self.assertEqual(second[13].rstrip(b"\0"), b"house")
            self.assertEqual(first[18:20], (0, 1))
            self.assertEqual(second[18:20], (1, 0))
            self.assertEqual(stats, {"entries": 2, "exterior": 1, "linked": 2})

    def test_direct_warp_and_interior_at_low_height(self):
        with tempfile.TemporaryDirectory() as root:
            maps = Path(root) / "data/maps"
            maps.mkdir(parents=True)
            (maps / "room.ipl").write_text(
                'inst\n10, room, 1, 681, -456, -24, 0, 0, 0, 1, -1\nend\n'
                'enex\n681, -456, -24, 0, 2, 2, 8, 100, 200, 20, 90, 0, 0, "ROOM", 0, 2, 0, 24\n'
                '100, 200, 20, 0, 2, 2, 8, 110, 210, 25, 90, 0, 0, "", 0, 2, 0, 24\nend\n'
            )
            (Path(root) / "data/gta.dat").write_text("IPL DATA\\MAPS\\room.IPL\n")
            data, _ = entry_exits.build(root, {})
            interior = entry_exits.RECORD.unpack_from(data, 8)
            warp = entry_exits.RECORD.unpack_from(data, 8 + entry_exits.RECORD.size)
            self.assertEqual(interior[18:20], (1, 0xFFFF))
            self.assertEqual(warp[18:20], (0, 0xFFFF))


if __name__ == "__main__":
    unittest.main()
