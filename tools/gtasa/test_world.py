import unittest
from types import SimpleNamespace
from unittest.mock import patch

from . import world


class WorldRadiusTest(unittest.TestCase):
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
