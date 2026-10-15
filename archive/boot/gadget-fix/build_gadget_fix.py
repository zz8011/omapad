#!/usr/bin/env python3
"""Patch only setup-gadget in the hash-pinned bootdiag-v2 cpio and bundle."""
from __future__ import annotations

import gzip
import hashlib
import io
from pathlib import Path
import stat

HERE = Path(__file__).resolve().parent
DIAG = HERE.parent
SOURCE_INITRD = DIAG / "out/a1599-alpine-rootfs-bootdiag-v2.cpio.gz"
SOURCE_BUNDLE = DIAG / "out/a1599-alpine-m1n1-linux-bootdiag-v2.bundle"
SCRIPT = DIAG / "runtime/sbin/setup-gadget"
OUT = HERE / "out"
PREFIX_LEN = 9_553_530
ROOTFS_SHA = "118a94a8c3b819eda228ffb6dcdcf22e3789ee09a868ef27125f531139f46d73"
BUNDLE_SHA = "0516c3fe8226c9ac7858d0cc5d8df09941d1a5e86f6c01dd4621b971fa870106"
PREFIX_SHA = "93ae225e1f25c2cdc0de508ef11471b4acb9e3b32b26b0270b7df378e7e7084c"


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def parse(raw: bytes) -> list[tuple[str, tuple[int, ...], bytes]]:
    out, pos = [], 0
    while pos + 110 <= len(raw):
        h = raw[pos:pos + 110]
        if h[:6] != b"070701":
            raise ValueError(f"bad newc magic at {pos}")
        fields = tuple(int(h[i:i + 8], 16) for i in range(6, 110, 8))
        size, namesz = fields[6], fields[11]
        n0, n1 = pos + 110, pos + 110 + namesz
        name = raw[n0:n1 - 1].decode()
        d0 = (n1 + 3) & ~3
        d1 = d0 + size
        end = (d1 + 3) & ~3
        if n1 > len(raw) or d1 > len(raw):
            raise ValueError(f"truncated entry {name}")
        out.append((name, fields, raw[d0:d1]))
        pos = end
        if name == "TRAILER!!!":
            if pos != len(raw):
                raise ValueError("trailing bytes after newc trailer")
            return out
    raise ValueError("missing newc trailer")


def record(name: str, fields: tuple[int, ...], data: bytes) -> bytes:
    name_bytes = name.encode() + b"\0"
    f = list(fields)
    f[6], f[11] = len(data), len(name_bytes)
    out = bytearray(b"070701" + b"".join(f"{v:08x}".encode() for v in f))
    out.extend(name_bytes)
    out.extend(b"\0" * (-len(out) % 4))
    out.extend(data)
    out.extend(b"\0" * (-len(out) % 4))
    return bytes(out)


def patch(raw: bytes, new_script: bytes) -> bytes:
    entries = parse(raw)
    matches = [e for e in entries if e[0] == "sbin/setup-gadget"]
    if len(matches) != 1 or not stat.S_ISREG(matches[0][1][1]):
        raise ValueError("expected one regular sbin/setup-gadget archive entry")
    entry = matches[0]
    if entry[2].startswith(b"\xef\xbb\xbf") or not entry[2].startswith(b"#!/bin/busybox sh\n"):
        raise ValueError("unexpected original setup-gadget format")
    # Rebuild records in order; all non-target metadata and payloads are retained.
    chunks = [record(n, f, new_script if n == "sbin/setup-gadget" else data)
              for n, f, data in entries]
    result = b"".join(chunks)
    after = parse(result)
    for before, current in zip(entries, after):
        if before[0] == "sbin/setup-gadget":
            if current[2] != new_script or current[1][1] != before[1][1]:
                raise AssertionError("setup-gadget entry mismatch")
        elif before != current:
            raise AssertionError(f"unexpected cpio change: {before[0]}")
    return result


def main() -> None:
    initrd, bundle = SOURCE_INITRD.read_bytes(), SOURCE_BUNDLE.read_bytes()
    if sha(initrd) != ROOTFS_SHA or sha(bundle) != BUNDLE_SHA:
        raise SystemExit("pinned bootdiag-v2 inputs do not match recorded hashes")
    prefix = bundle[:PREFIX_LEN]
    if sha(prefix) != PREFIX_SHA or bundle[PREFIX_LEN:] != initrd:
        raise SystemExit("bundle prefix/suffix boundary validation failed")
    raw = gzip.decompress(initrd)
    patched_raw = patch(raw, SCRIPT.read_bytes())
    compressed = io.BytesIO()
    with gzip.GzipFile(fileobj=compressed, mode="wb", compresslevel=9, mtime=0, filename="") as z:
        z.write(patched_raw)
    new_initrd = compressed.getvalue()
    OUT.mkdir(parents=True, exist_ok=True)
    initrd_path = OUT / "a1599-alpine-rootfs-gadget-fix.cpio.gz"
    bundle_path = OUT / "a1599-alpine-m1n1-linux-gadget-fix.bundle"
    initrd_path.write_bytes(new_initrd)
    bundle_path.write_bytes(prefix + new_initrd)
    if bundle_path.read_bytes() != prefix + initrd_path.read_bytes():
        raise SystemExit("bundle readback mismatch")
    print(f"rootfs={initrd_path} bytes={len(new_initrd)} sha256={sha(new_initrd)}")
    print(f"bundle={bundle_path} bytes={bundle_path.stat().st_size} sha256={sha(bundle_path.read_bytes())}")
    print(f"prefix_bytes={len(prefix)} prefix_sha256={sha(prefix)}")


if __name__ == "__main__":
    main()
