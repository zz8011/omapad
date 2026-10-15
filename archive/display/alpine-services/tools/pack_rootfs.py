#!/usr/bin/env python3
"""Create a reproducible gzip-compressed newc initramfs, including /dev/console."""
import gzip
import os
import stat
import sys
from pathlib import Path


def pad4(stream):
    stream.write(b"\0" * ((-stream.tell()) & 3))


def emit(stream, ino, name, mode, data=b"", rdev=(0, 0)):
    encoded = name.encode("utf-8") + b"\0"
    fields = (ino, mode, 0, 0, 1, 0, len(data), 0, 0,
              rdev[0], rdev[1], len(encoded), 0)
    stream.write(b"070701" + b"".join(f"{value:08x}".encode("ascii") for value in fields))
    stream.write(encoded)
    pad4(stream)
    stream.write(data)
    pad4(stream)


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: pack_rootfs.py ROOT_DIR OUTPUT.cpio.gz")
    root = Path(sys.argv[1]).resolve()
    out = Path(sys.argv[2])
    if not (root / "init").exists():
        raise SystemExit("ROOT_DIR/init is required")
    paths = [root]
    paths.extend(sorted(root.rglob("*"), key=lambda p: p.relative_to(root).as_posix()))
    with out.open("wb") as raw:
        with gzip.GzipFile(fileobj=raw, mode="wb", compresslevel=9, mtime=0, filename="") as stream:
            ino = 1
            for path in paths:
                name = "." if path == root else path.relative_to(root).as_posix()
                info = path.lstat()
                mode = info.st_mode
                if stat.S_ISLNK(mode):
                    data = os.readlink(path).encode("utf-8")
                elif stat.S_ISREG(mode):
                    data = path.read_bytes()
                elif stat.S_ISDIR(mode):
                    data = b""
                else:
                    raise SystemExit(f"unsupported rootfs object: {name}")
                emit(stream, ino, name, mode, data)
                ino += 1
            emit(stream, ino, "dev/console", stat.S_IFCHR | 0o600, rdev=(5, 1))
            emit(stream, ino + 1, "TRAILER!!!", 0)


if __name__ == "__main__":
    main()
