#!/usr/bin/env python3
"""Compare one fixed A1599 output-buffer dump against an FFmpeg reference.

This is a bounded layout screen for a single 32x32 8-bit 4:2:0 frame. It never
reads a device and never assumes that one candidate layout is authoritative.
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

WIDTH = 32
HEIGHT = 32
BACKING_BYTES = 4096
GUARD_START = 3072


@dataclass(frozen=True)
class Layout:
    name: str
    y_offset: int
    y_stride: int
    u_offset: int
    v_offset: int
    c_stride: int
    semiplanar: bool = False


def candidate_layouts() -> list[Layout]:
    layouts: list[Layout] = []
    for y_stride in (32, 48, 64, 128):
        y_end = y_stride * HEIGHT
        for c_stride in (16, 32, 64, 128):
            c_bytes = c_stride * (HEIGHT // 2)
            for c_base in sorted({y_end, 1024, 1536, 2048}):
                for order in ("YUV420P", "YVU420P"):
                    u, v = ((c_base, c_base + c_bytes) if order == "YUV420P"
                            else (c_base + c_bytes, c_base))
                    layouts.append(Layout(order, 0, y_stride, u, v, c_stride))
                for order in ("NV12", "NV21"):
                    layouts.append(Layout(order, 0, y_stride, c_base,
                                          c_base, c_stride, True))
    return layouts


def extract_planes(backing: bytes, layout: Layout) -> tuple[bytes, bytes, bytes] | None:
    if len(backing) != BACKING_BYTES:
        raise ValueError(f"output dump must be exactly {BACKING_BYTES} bytes")
    y_size = layout.y_stride * HEIGHT
    c_height = HEIGHT // 2
    c_size = layout.c_stride * c_height
    if layout.y_stride < WIDTH or layout.c_stride < (WIDTH if layout.semiplanar else WIDTH // 2):
        return None
    spans = [(layout.y_offset, y_size)]
    if layout.semiplanar:
        spans.append((layout.u_offset, c_size))
    else:
        spans.extend(((layout.u_offset, c_size), (layout.v_offset, c_size)))
    if any(start < 0 or start + size > GUARD_START for start, size in spans):
        return None
    for index, (start, size) in enumerate(spans):
        for other_start, other_size in spans[:index]:
            if start < other_start + other_size and other_start < start + size:
                return None
    y = b"".join(backing[layout.y_offset + row * layout.y_stride:
                         layout.y_offset + row * layout.y_stride + WIDTH]
                 for row in range(HEIGHT))
    if layout.semiplanar:
        uv = b"".join(backing[layout.u_offset + row * layout.c_stride:
                               layout.u_offset + row * layout.c_stride + WIDTH]
                       for row in range(c_height))
        u = bytes(uv[i] for i in range(0, len(uv), 2))
        v = bytes(uv[i] for i in range(1, len(uv), 2))
    else:
        u = b"".join(backing[layout.u_offset + row * layout.c_stride:
                               layout.u_offset + row * layout.c_stride + WIDTH // 2]
                       for row in range(c_height))
        v = b"".join(backing[layout.v_offset + row * layout.c_stride:
                               layout.v_offset + row * layout.c_stride + WIDTH // 2]
                       for row in range(c_height))
    return y, u, v


def score_frame(actual: tuple[bytes, bytes, bytes], reference: bytes) -> dict[str, float | int | bool]:
    raw = b"".join(actual)
    if len(reference) != 1536 or len(raw) != len(reference):
        raise ValueError("reference and candidate must each contain one 32x32 YUV420 frame")
    errors = [abs(a - b) for a, b in zip(raw, reference)]
    return {
        "exact": all(error == 0 for error in errors),
        "equal_bytes": sum(error == 0 for error in errors),
        "bytes": len(errors),
        "mae": sum(errors) / len(errors),
        "max_abs_error": max(errors),
    }


def compare(backing: bytes, reference: bytes) -> list[dict[str, object]]:
    scored = []
    seen = set()
    for layout in candidate_layouts():
        key = layout
        if key in seen:
            continue
        seen.add(key)
        planes = extract_planes(backing, layout)
        if planes is None:
            continue
        scored.append({"layout": layout.__dict__, **score_frame(planes, reference)})
    return sorted(scored, key=lambda item: (not item["exact"], item["mae"], -item["equal_bytes"]))


def ffmpeg_reference(input_path: Path, ffmpeg: str) -> bytes:
    command = [ffmpeg, "-v", "error", "-i", str(input_path), "-frames:v", "1",
               "-pix_fmt", "yuv420p", "-f", "rawvideo", "-"]
    result = subprocess.run(command, check=True, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE)
    if len(result.stdout) != 1536:
        raise ValueError(f"FFmpeg produced {len(result.stdout)} bytes; expected one 32x32 YUV420 frame")
    return result.stdout


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output_dump", type=Path, help="exact 4096-byte decode_output sysfs dump")
    parser.add_argument("reference_bitstream", type=Path, help="same H.264 fixture used by candidate")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--top", type=int, default=8, help="number of best finite layout candidates to print")
    args = parser.parse_args()
    try:
        backing = args.output_dump.read_bytes()
        if len(backing) != BACKING_BYTES:
            raise ValueError(f"dump is {len(backing)} bytes, expected exactly {BACKING_BYTES}")
        if backing[GUARD_START:] != bytes([0xA5]) * (BACKING_BYTES - GUARD_START):
            raise ValueError("output guard [3072,4096) changed; refuse image-layout comparison")
        reference = ffmpeg_reference(args.reference_bitstream, args.ffmpeg)
        results = compare(backing, reference)
        exact = [result for result in results if result["exact"]]
        report = {
            "reference": str(args.reference_bitstream),
            "reference_decoder": args.ffmpeg,
            "geometry": "32x32 progressive 8-bit 4:2:0",
            "guard": "intact",
            "candidate_layouts_compared": len(results),
            "classification": "exact_layout_match" if exact else "UNMATCHED_LAYOUT",
            "exact_matches": exact,
            "best_candidates": results[:max(0, args.top)],
            "warning": "A score ranks only this finite layout set; a match does not prove DMA coherency or future hardware determinism.",
        }
        print(json.dumps(report, indent=2))
        return 0 if exact else 2
    except (OSError, subprocess.CalledProcessError, ValueError) as exc:
        print(f"compare_output: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
