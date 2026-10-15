#!/usr/bin/env python3
"""Pack the reviewed minimal init root into gzip-compressed newc format."""
import gzip
import os
import stat
import sys
from pathlib import Path


def pad4(stream):
    stream.write(b"\0" * ((-stream.tell()) & 3))


def emit(stream, ino, name, mode, data=b"", rdev=(0, 0)):
    encoded = name.encode() + b"\0"
    fields = (ino, mode, 0, 0, 1, 0, len(data), 0, 0,
              rdev[0], rdev[1], len(encoded), 0)
    stream.write(b"070701" + b"".join(f"{v:08x}".encode() for v in fields))
    stream.write(encoded)
    pad4(stream)
    stream.write(data)
    pad4(stream)


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: pack_newc.py ROOT_DIR OUTPUT.cpio.gz")
    root = Path(sys.argv[1]).resolve()
    out = Path(sys.argv[2])
    init = root / "init"
    if not init.is_file():
        raise SystemExit("ROOT_DIR/init is required")
    entries = [(root, ".")]
    entries.extend((p, p.relative_to(root).as_posix())
                   for p in sorted(root.rglob("*")) if p.is_dir())
    entries.extend((p, p.relative_to(root).as_posix())
                   for p in sorted(root.rglob("*")) if p.is_file())
    with out.open("wb") as raw:
        with gzip.GzipFile(fileobj=raw, mode="wb", mtime=0, filename="") as stream:
            ino = 1
            for path, name in entries:
                mode = (stat.S_IFDIR | 0o755) if path.is_dir() else (stat.S_IFREG | (0o755 if name == "init" else 0o644))
                data = b"" if path.is_dir() else path.read_bytes()
                emit(stream, ino, name, mode, data)
                ino += 1
            emit(stream, ino, "dev", stat.S_IFDIR | 0o755)
            ino += 1
            emit(stream, ino, "dev/console", stat.S_IFCHR | 0o600, rdev=(5, 1))
            emit(stream, ino + 1, "TRAILER!!!", 0)


if __name__ == "__main__":
    main()
