#!/usr/bin/env python3
"""Compose a new Alpine payload after verifying the immutable v2.4 prefix."""
import gzip
import hashlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
BASE = ROOT / ".build/a1599"
V24 = BASE / "v2.4/out"
OLD_BUNDLE = V24 / "a1599-m1n1-linux-acm-v2.4.bundle"
OLD_SUMS = V24 / "SHA256SUMS"
M1N1 = BASE / "display-diag/package/m1n1.bin"
ARGS = BASE / "display-diag/package/bootargs.bin"
DTB = V24 / "kernel/s5l8960x-j85m-v2.4.dtb"
IMAGE = V24 / "kernel/Image-v2.4.gz"
INITRD = Path(__file__).resolve().parents[1] / "out/a1599-alpine-rootfs.cpio.gz"
OUT = Path(__file__).resolve().parents[1] / "out/a1599-alpine-m1n1-linux.bundle"

M1N1_SIZE = 1_196_032
ARGS_SIZE = 38
DTB_SIZE = 19_904
IMAGE_SIZE = 8_337_556
OLD_INITRD_SIZE = 4_049
OLD_BUNDLE_SHA256 = "a9d3673b021da0901aa5f54c1c7825ae29b3a3de5af2926e3df1ae6c1877b133"
EXPECTED_M1N1_SHA256 = "d8bb01afebc5bbdc933e76d130364a14e2a840c32f7cb200f2769dd1e28de5e3"
EXPECTED_ARGS = b"chosen.bootargs=init=/init loglevel=7\n"


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def expected_from_manifest(path: Path, name: str) -> str:
    for row in path.read_text().splitlines():
        digest, filename = row.split(maxsplit=1)
        if filename.strip() == name:
            return digest
    raise ValueError(f"{name} missing from fixed base manifest")


def main() -> None:
    old = OLD_BUNDLE.read_bytes()
    if sha(old) != expected_from_manifest(OLD_SUMS, OLD_BUNDLE.name):
        raise SystemExit("refusing: v2.4 bundle does not match its recorded SHA256")
    if sha(old) != OLD_BUNDLE_SHA256:
        raise SystemExit("refusing: v2.4 bundle differs from the pinned prior candidate")
    m1n1, args, dtb, image, old_initrd = M1N1.read_bytes(), ARGS.read_bytes(), DTB.read_bytes(), IMAGE.read_bytes(), (V24 / "initramfs-a1599-acm-v2.4.cpio.gz").read_bytes()
    lengths = (M1N1_SIZE, ARGS_SIZE, DTB_SIZE, IMAGE_SIZE, OLD_INITRD_SIZE)
    parts = (m1n1, args, dtb, image, old_initrd)
    if tuple(map(len, parts)) != lengths or sum(lengths) != len(old):
        raise SystemExit("refusing: prior payload segment lengths differ from recorded boundaries")
    if sha(m1n1) != EXPECTED_M1N1_SHA256 or args != EXPECTED_ARGS:
        raise SystemExit("refusing: m1n1 binary or bootargs differ from approved baseline")
    offset = 0
    for part in parts:
        if old[offset:offset + len(part)] != part:
            raise SystemExit("refusing: extracted v2.4 component does not match bundle bytes")
        offset += len(part)
    with gzip.open(INITRD, "rb") as stream:
        if not stream.read(6).startswith(b"070701"):
            raise SystemExit("refusing: Alpine initramfs is not a gzip-compressed newc archive")
    prefix = b"".join(parts[:-1])
    bundle = prefix + INITRD.read_bytes()
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_bytes(bundle)
    if OUT.read_bytes() != prefix + INITRD.read_bytes():
        raise SystemExit("bundle readback mismatch")
    print(f"bundle={OUT}")
    print(f"size={len(bundle)} sha256={sha(bundle)}")
    print(f"segments: m1n1={M1N1_SIZE}, bootargs={ARGS_SIZE}, dtb={DTB_SIZE}, image={IMAGE_SIZE}, alpine_initrd={INITRD.stat().st_size}")
    print(f"immutable prefix sha256={sha(prefix)}")


if __name__ == "__main__":
    main()
