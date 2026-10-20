"""Regression guard for 2D matrix configuration persistence contract.

Checks that each web control has matching firmware JSON read/write keys.
Device reboot/flash persistence still requires hardware validation.
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CPP = (ROOT / "src/output/OutputPixel.cpp").read_text(encoding="utf-8")
HTML = (ROOT / "html/PixelCommon.html").read_text(encoding="utf-8")
FIELDS = (
    "matrix_enabled", "matrix_width", "matrix_height", "matrix_serpentine",
    "matrix_vertical", "matrix_reverse_x", "matrix_reverse_y", "matrix_rotation",
)

class MatrixConfigContract(unittest.TestCase):
    def test_json_round_trip_fields(self):
        for field in FIELDS:
            with self.subTest(field=field):
                self.assertRegex(CPP, r'JsonWrite\s*\(\s*jsonConfig\s*,\s*"' + field + r'"')
                self.assertRegex(CPP, r'setFromJSON\s*\(\s*\w+\s*,\s*jsonConfig\s*,\s*"' + field + r'"')

    def test_browser_controls_match(self):
        for field in FIELDS:
            with self.subTest(field=field):
                self.assertRegex(HTML, r'id="' + field + r'"')

    def test_rotation_options(self):
        rotation = re.search(r'<select[^>]+id="matrix_rotation"[^>]*>(.*?)</select>', HTML, re.S)
        self.assertIsNotNone(rotation)
        self.assertEqual(set(re.findall(r'<option value="(\d+)"', rotation.group(1))),
                         {"0", "90", "180", "270"})

    def test_validation_guards(self):
        self.assertIn("MatrixPixels != PixelGroups", CPP)
        self.assertIn("pixel_count % PixelGroupSize", CPP)
        self.assertIn("MatrixWidth != MatrixHeight", CPP)
        self.assertIn("GetConfig (jsonConfig);", CPP)

if __name__ == "__main__":
    unittest.main()
