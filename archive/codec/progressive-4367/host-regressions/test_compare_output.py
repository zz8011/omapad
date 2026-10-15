#!/usr/bin/env python3
import importlib.util
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("compare_output", ROOT / "tools/compare_output.py")
compare_output = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = compare_output
SPEC.loader.exec_module(compare_output)


def put_planar(backing, layout, y, u, v):
    for row in range(32):
        start = layout.y_offset + row * layout.y_stride
        backing[start:start + 32] = y[row * 32:(row + 1) * 32]
    if layout.semiplanar:
        uv = bytes(value for pair in zip(u, v) for value in pair)
        for row in range(16):
            start = layout.u_offset + row * layout.c_stride
            backing[start:start + 32] = uv[row * 32:(row + 1) * 32]
    else:
        for plane, values in ((layout.u_offset, u), (layout.v_offset, v)):
            for row in range(16):
                start = plane + row * layout.c_stride
                backing[start:start + 16] = values[row * 16:(row + 1) * 16]


class CompareOutputTests(unittest.TestCase):
    def setUp(self):
        self.y = bytes((i * 31 + 7) & 255 for i in range(1024))
        self.u = bytes((i * 17 + 43) & 255 for i in range(256))
        self.v = bytes((i * 29 + 91) & 255 for i in range(256))
        self.reference = self.y + self.u + self.v

    def test_detects_nv12_candidate_without_assuming_it(self):
        layout = compare_output.Layout("NV12", 0, 64, 2048, 2048, 32, True)
        backing = bytearray([0xA5] * 4096)
        backing[:3072] = bytes(3072)
        put_planar(backing, layout, self.y, self.u, self.v)
        matches = [item for item in compare_output.compare(bytes(backing), self.reference)
                   if item["exact"]]
        self.assertTrue(any(item["layout"]["name"] == "NV12" for item in matches))

    def test_detects_planar_candidate_and_rejects_guard_overlap(self):
        layout = compare_output.Layout("YUV420P", 0, 64, 2048, 2304, 16)
        backing = bytearray([0xA5] * 4096)
        backing[:3072] = bytes(3072)
        put_planar(backing, layout, self.y, self.u, self.v)
        matches = [item for item in compare_output.compare(bytes(backing), self.reference)
                   if item["exact"]]
        self.assertTrue(any(item["layout"]["name"] == "YUV420P" for item in matches))
        crossing = compare_output.Layout("NV12", 0, 64, 2560, 2560, 64, True)
        self.assertIsNone(compare_output.extract_planes(bytes(backing), crossing))

    def test_unmatched_output_is_explicit(self):
        backing = bytes([0xA5]) * 4096
        report = compare_output.compare(backing, self.reference)
        self.assertTrue(report)
        self.assertFalse(any(item["exact"] for item in report))


if __name__ == "__main__":
    unittest.main()
