"""Create compact framebuffer bands used by the separate boot-diagnostic rootfs."""

from pathlib import Path
import sys

FRAME_WIDTH = 1536
FRAME_HEIGHT = 192
STATUS_HEIGHT = 32
STAGE_COLORS = {
    "pid1": (0, 128, 255),       # blue: PID1 reached devtmpfs
    "mounts": (255, 220, 0),     # yellow: critical mounts completed
    "gadget-ok": (0, 220, 40),  # green: gadget bind succeeded
    "gadget-fail": (255, 0, 220),  # magenta: userspace reached gadget failure
}
BAR_COLORS = ((255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 255))


def _pixel(rgb: tuple[int, int, int]) -> bytes:
    red, green, blue = rgb
    return bytes((blue, green, red, 0))


def build_stage_frame(stage: str) -> bytes:
    if stage not in STAGE_COLORS:
        raise ValueError(f"unknown boot stage: {stage}")
    status = _pixel(STAGE_COLORS[stage])
    bars = tuple(map(_pixel, BAR_COLORS))
    frame = bytearray(FRAME_WIDTH * FRAME_HEIGHT * 4)
    for y in range(FRAME_HEIGHT):
        if y < STATUS_HEIGHT:
            row = status * FRAME_WIDTH
        else:
            row = b"".join(bars[x * 4 // FRAME_WIDTH] for x in range(FRAME_WIDTH))
        start = y * FRAME_WIDTH * 4
        frame[start:start + len(row)] = row
    return bytes(frame)


def write_stage_assets(root: Path) -> None:
    dest = root / "usr/share/a1599/boot-stage"
    dest.mkdir(parents=True, exist_ok=True)
    for stage in STAGE_COLORS:
        (dest / f"{stage}.raw").write_bytes(build_stage_frame(stage))


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: boot_markers.py ROOTFS")
    write_stage_assets(Path(sys.argv[1]))
