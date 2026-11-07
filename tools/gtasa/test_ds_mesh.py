import unittest
from unittest.mock import patch

from . import dff
from .ds_mesh import DEFAULTS, convert_dff, decimate


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

    def test_rigid_cutscene_atomic_follows_animated_bone(self):
        model = dff.Model()
        model.frames = []
        for parent, bone, pos in [(-1, -1, (0, 0, 0)), (0, 0, (10, 0, 0)), (1, 1, (2, 0, 0))]:
            frame = dff.Frame()
            frame.parent, frame.bone_id, frame.pos = parent, bone, pos
            frame.rot = (1, 0, 0, 0, 1, 0, 0, 0, 1)
            frame.name = f'node{bone}'
            model.frames.append(frame)
        geometry = dff.Geometry()
        geometry.verts = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
        geometry.tris = [(0, 1, 2, 0)]
        geometry.uvs = geometry.colors = geometry.normals = geometry.skin = None
        geometry.sphere = (0, 0, 0, 1)
        material = dff.Material()
        material.color, material.texture = (255, 255, 255, 255), ''
        geometry.materials = [material]
        model.geometries = [geometry]
        model.atomics = [(2, 0)]
        model.hanim = [(0, 1, 0), (1, 2, 1)]
        with patch.object(dff, 'parse', return_value=model):
            binary, _ = convert_dff(b'', {**DEFAULTS, 'rigid_skin': True})
        self.assertEqual(binary[6:8], b'\x01\x00')
        self.assertEqual(binary[14:16], b'\x02\x00')
        self.assertEqual(geometry.verts[0], (12, 0, 0))
        self.assertEqual(geometry.skin.indices[:4], b'\x01\x00\x00\x00')
        self.assertEqual(geometry.skin.inv_matrices[1][12:15], (-12, 0, 0))


if __name__ == '__main__':
    unittest.main()
