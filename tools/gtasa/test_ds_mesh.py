import unittest

from .ds_mesh import decimate


class ClusterTest(unittest.TestCase):
    def test_max_shift_preserves_large_corner_displacements(self):
        verts = [(float(x), float(y), 0.0) for y in range(3) for x in range(3)]
        tris = []
        for y in range(2):
            for x in range(2):
                a = y * 3 + x
                tris.extend(((a, a + 1, a + 3, 0), (a + 1, a + 4, a + 3, 0)))
        rep, out = decimate(verts, tris, 2, max_shift=0.25)
        self.assertGreater(len(out), 2)
        if rep is not None:
            self.assertLessEqual(max(sum((verts[i][k] - rep[i][k]) ** 2 for k in range(3)) ** 0.5
                                     for i in {v for t in out for v in t[:3]}), 0.25)
        self.assertLessEqual(len(decimate(verts, tris, 2, max_shift=0.25, max_output=3)[1]), 3)


if __name__ == '__main__':
    unittest.main()
