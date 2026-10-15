import sys
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))

from boot_markers import FRAME_HEIGHT, FRAME_WIDTH, STAGE_COLORS, build_stage_frame


class BootMarkerTests(unittest.TestCase):
    def test_frame_has_known_a1599_dimensions_and_pixel_order(self):
        frame = build_stage_frame("pid1")
        self.assertEqual(len(frame), FRAME_WIDTH * FRAME_HEIGHT * 4)
        # Pixel storage follows the existing displayd B,G,R,unused-byte layout.
        self.assertEqual(frame[0:4], bytes((255, 128, 0, 0)))
        first_bar = 32 * FRAME_WIDTH * 4
        self.assertEqual(frame[first_bar:first_bar + 4], bytes((0, 0, 255, 0)))
        green_pixel = first_bar + 500 * 4
        self.assertEqual(frame[green_pixel:green_pixel + 4], bytes((0, 255, 0, 0)))

    def test_each_boot_stage_has_a_distinct_top_band(self):
        signatures = set()
        for stage, rgb in STAGE_COLORS.items():
            frame = build_stage_frame(stage)
            expected = bytes((rgb[2], rgb[1], rgb[0], 0))
            signatures.add(frame[:4])
            self.assertEqual(frame[:4], expected)
        self.assertEqual(len(signatures), len(STAGE_COLORS))

    def test_unknown_stage_is_rejected(self):
        with self.assertRaises(ValueError):
            build_stage_frame("made-up")


if __name__ == "__main__":
    unittest.main()
