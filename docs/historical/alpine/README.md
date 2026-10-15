# A1599 Alpine RAM rootfs candidate

This directory contains an offline-built Alpine Linux root filesystem candidate for the experimental A1599/A7 Linux boot path. It is a rootfs only: it is not a full boot bundle, has not been sent to the iPad, and has not been booted on hardware. Do not concatenate it with any m1n1 bundle without coordination with the parent task.

## Verified upstream base

- Base: official Alpine Linux `alpine-minirootfs-3.24.2-aarch64.tar.gz` (4,028,030 bytes compressed).
- Version-pinned source URL: `https://dl-cdn.alpinelinux.org/alpine/v3.24/releases/aarch64/alpine-minirootfs-3.24.2-aarch64.tar.gz`.
- The official `.sha256` matches, and the detached `.asc` signature verified using the Alpine release signing key published by Alpine. Key fingerprint: `0482 D840 22F5 2DF1 C4E7 CD43 293A CD09 07D9 495A`.
- The extracted base is approximately 9.9 MiB. The staged runtime with the display dependency is about 11 MiB; the reproducible rootfs archive is `out/a1599-alpine-rootfs.cpio.gz` (4,259,930 bytes; SHA256 `c993681ee89fecfce1907cde254f992dc510be1e4f8dadb8deeecfd391dee816`).
- Build-only inputs include Alpine `musl-dev` and `libjpeg-turbo-dev`; runtime adds the Alpine `libjpeg-turbo` package. Each downloaded APK passed `apk verify` against keys shipped in the verified base.
- Alpine package metadata reports BusyBox and apk-tools as GPL-2.0, musl as MIT, and libjpeg-turbo as BSD-3-Clause/IJG/Zlib. Consult each package's metadata and license files for redistribution obligations.

## Target and system layout

The existing candidate kernel is AArch64 Linux with ELF support and 4 KiB pages (`CONFIG_ARM64_4K_PAGES=y`, `CONFIG_BINFMT_ELF=y`). The Alpine BusyBox/APK binaries are AArch64 PIE programs using `/lib/ld-musl-aarch64.so.1`; the rootfs contains that musl loader. The kernel's known framebuffer mode is 1536x2048, 32 bpp. These are compatibility checks from existing kernel/device evidence, not a hardware boot test of this rootfs.

The initramfs contains Alpine base utilities, BusyBox PID 1, apk-tools, musl, CA certificates, and libjpeg-turbo. It starts three separate USB CDC ACM functions when the kernel exposes a USB device controller and configfs:

1. `ttyGS0`: an unauthenticated root shell (`getty -n -l /bin/sh`). Anyone who can attach to this physical gadget gets root-equivalent access. Keep the device physically controlled. No network gadget, SSH daemon, or saved credentials are configured.
2. `ttyGS1`: bounded file receiver. It accepts safe leaf filenames only, files up to 32 MiB, verifies CRC32, uses exclusive/no-follow creation, and writes into `/run/incoming` on a 128 MiB tmpfs with an 8 MiB free-space reserve. It accepts APK files as data; it does not install them automatically.
3. `ttyGS2`: JPEG framebuffer display service. It accepts JPEG images up to 8 MiB and 8 million decoded pixels, for the expected 1536x2048 RGB framebuffer. The framebuffer mapping is capped at 32 MiB; the display process has a 128 MiB virtual-address-space limit to bound decoder working memory. Each `START` begins a fresh session; `STOP` restores the framebuffer snapshot. Invalid or interrupted frames close that session and the same service process can accept a later `START`; there is no three-frame-per-boot counter.

The gadget script creates three separate ACM functions in creation order and binds only if exactly one UDC is present. Each ACM function requires a control interrupt endpoint and a bulk IN/OUT pair; whether the A1599 DWC2 instance can allocate all three functions is not verified until the target enumerates. USB ACM numbering/host port mapping, framebuffer color order, touch, and throughput also remain unverified on hardware. The setup code intentionally exposes no USB storage or USB networking. `/etc/apk/repositories` contains no active network repository; there is no network interface setup or network daemon in the init path. This is the default configuration, not a sandbox: the passwordless root shell can manually configure any network device the kernel exposes.

## Application updates and persistence

Files and package changes are RAM-only. The receiver writes to a hidden temporary file, verifies CRC32, then hard-links it to the requested name atomically without overwriting an existing file. Failed or incomplete transfers remove the temporary file; a receiver restart also removes stale `.part-*` files. CRC32 detects accidental corruption but does not authenticate the sender or file. Transfer a locally obtained APK to `/run/incoming`, verify its source and package metadata, then install from the root shell with Alpine `apk` against that local file (for example, `apk --network=no add /run/incoming/<package>.apk`). APK signature checking remains enabled; only install packages signed by an accepted Alpine key or a separately trusted key. The 128 MiB `/run` tmpfs caps upload accumulation, and each upload is at most 32 MiB. The passwordless root shell can still consume other RAM or install a package larger than the transfer limit by another route. There are no persistent credentials or configured network update channel. All changes disappear at reboot, so a durable update requires rebuilding and separately reviewing a new rootfs image.

## Build and verification evidence

- The AArch64 programs were cross-compiled with local Clang/LLD 22 and the Alpine musl sysroot; the resulting target ELF interpreter is `/lib/ld-musl-aarch64.so.1`.
- The archive includes `/dev/console` as character device 5:1, and its `/init` and `/sbin/init` symlink chain resolves to Alpine BusyBox PID 1.
- `tests/test_services.py` passes three offline tests: AArch64 file receiver traversal/CRC/atomic-commit behavior under QEMU, AArch64 rejection of malformed JPEG, and display session recovery/frame rejection/restore behavior using a framebuffer test backend.
- QEMU smoke checks ran BusyBox and APK from this rootfs; the APK reports `apk-tools 3.0.8-r0`. QEMU user-mode needs `LD_LIBRARY_PATH=/usr/lib` for APK because it is not booting/chrooting the target environment.
- `gzip -t out/a1599-alpine-rootfs.cpio.gz` and all entries in `out/SHA256SUMS` pass. The cpio archive is generated with root ownership and reproducible newc/gzip metadata.
- Runtime ELF dependencies were checked against the packaged loader and libraries, including `libjpeg.so.8`, musl, and APK's libssl/libcrypto/libz dependencies.
- This executor has QEMU user-mode but no `qemu-system-aarch64`; the complete PID 1 + mount + configfs + UDC startup sequence has not been simulated. The shell/getty and service processes were tested separately. No test verifies physical USB enumeration or a successful iPad boot. The iPad remains in DFU and has not been modified by this work.

## Upstream references

- [Alpine downloads and signing key fingerprint](https://alpinelinux.org/downloads/)
- [Version-pinned Alpine 3.24 AArch64 release directory](https://dl-cdn.alpinelinux.org/alpine/v3.24/releases/aarch64/)
- [Alpine release policy](https://alpinelinux.org/releases/)
- [Linux initramfs documentation](https://docs.kernel.org/filesystems/ramfs-rootfs-initramfs.html)
- [Linux USB gadget configfs documentation](https://docs.kernel.org/usb/gadget_configfs.html)
- [Linux USB ACM testing documentation](https://docs.kernel.org/usb/gadget-testing.html)
