#!/usr/bin/env python3
"""Deterministic 2D matrix mapping regression tests.

Run: python3 -m unittest discover -s tests -p 'test_matrix_mapping.py'
The cases mirror c_OutputPixel::CalculateMatrixPixelId; firmware builds are
separate and hardware output must still be validated.
"""
import unittest


def map_pixel(index, width, height, *, vertical=False, serpentine=True,
              reverse_x=False, reverse_y=False, rotation=0):
    if width < 1 or height < 1 or index < 0 or index >= width * height:
        raise ValueError("invalid matrix geometry/index")
    if rotation not in (0, 90, 180, 270):
        raise ValueError("invalid rotation")
    if rotation in (90, 270) and width != height:
        raise ValueError("quarter-turn requires square geometry")
    x, y = index % width, index // width
    if rotation == 90:
        x, y = width - 1 - y, x
    elif rotation == 270:
        x, y = y, height - 1 - x
    elif rotation == 180:
        x, y = width - 1 - x, height - 1 - y
    if reverse_x:
        x = width - 1 - x
    if reverse_y:
        y = height - 1 - y
    if vertical:
        if serpentine and x % 2:
            y = height - 1 - y
        return x * height + y
    if serpentine and y % 2:
        x = width - 1 - x
    return y * width + x


class MatrixMappingTests(unittest.TestCase):
    def test_4x4_serpentine(self):
        self.assertEqual(
            [map_pixel(i, 4, 4) for i in range(16)],
            [0, 1, 2, 3, 7, 6, 5, 4, 8, 9, 10, 11, 15, 14, 13, 12])

    def test_4x4_progressive(self):
        self.assertEqual(
            [map_pixel(i, 4, 4, serpentine=False) for i in range(16)],
            list(range(16)))

    def test_vertical_serpentine(self):
        self.assertEqual(
            [map_pixel(i, 3, 2, vertical=True) for i in range(6)],
            [0, 3, 4, 1, 2, 5])

    def test_rectangular_rot180(self):
        self.assertEqual(
            [map_pixel(i, 3, 2, serpentine=False, rotation=180)
             for i in range(6)], [5, 4, 3, 2, 1, 0])

    def test_all_permutations(self):
        for width, height in ((4, 4), (16, 16), (32, 16), (32, 32)):
            for vertical in (False, True):
                for serpentine in (False, True):
                    for reverse_x in (False, True):
                        for reverse_y in (False, True):
                            rotations = (0, 90, 180, 270) if width == height else (0, 180)
                            for rotation in rotations:
                                with self.subTest(width=width, height=height,
                                                  vertical=vertical, serpentine=serpentine,
                                                  reverse_x=reverse_x, reverse_y=reverse_y,
                                                  rotation=rotation):
                                    result = [map_pixel(i, width, height, vertical=vertical,
                                                        serpentine=serpentine,
                                                        reverse_x=reverse_x,
                                                        reverse_y=reverse_y,
                                                        rotation=rotation)
                                              for i in range(width * height)]
                                    self.assertEqual(sorted(result), list(range(width * height)))

    def test_invalid_geometry(self):
        with self.assertRaises(ValueError):
            map_pixel(0, 0, 4)
        with self.assertRaises(ValueError):
            map_pixel(16, 4, 4)
        with self.assertRaises(ValueError):
            map_pixel(0, 3, 2, rotation=90)


if __name__ == "__main__":
    unittest.main()
