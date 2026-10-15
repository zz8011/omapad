"""Offline PTY checks for the AArch64 file service and display session loop."""
import binascii
import os
from pathlib import Path
import pty
import select
import signal
import subprocess
import tempfile
import time
import unittest

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
QEMU = "/usr/bin/qemu-aarch64-static"
FB_BYTES = 1536 * 2048 * 4


def read_line(fd, timeout=8):
    out = bytearray()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        ready, _, _ = select.select([fd], [], [], max(0, deadline-time.monotonic()))
        if not ready:
            break
        out.extend(os.read(fd, 1))
        if out.endswith(b"\n"):
            return out.decode("ascii").rstrip("\r\n")
    raise AssertionError(f"serial line timeout, bytes={bytes(out)!r}")


def spawn_qemu(program, slave, rootfs, *args):
    return subprocess.Popen(
        [QEMU, "-E", f"LD_LIBRARY_PATH={rootfs / 'usr/lib'}", "-L", str(rootfs),
         str(program), *args],
        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
    )


class ServiceTests(unittest.TestCase):
    def test_aarch64_file_receiver_bounds_path_and_checks_crc(self):
        rootfs = ROOT / "runtime"
        program = ROOT / "file-recv"
        with tempfile.TemporaryDirectory(prefix="a1599-incoming-") as tmp:
            incoming = Path(tmp) / "incoming"
            incoming.mkdir()
            (incoming / ".part-stale").write_bytes(b"interrupted upload")
            master, slave_fd = pty.openpty()
            slave = os.ttyname(slave_fd)
            proc = spawn_qemu(program, slave, rootfs, slave, str(incoming))
            try:
                self.assertEqual(read_line(master), "FILE-RECV 1 READY")
                self.assertFalse((incoming / ".part-stale").exists())
                os.write(master, b"HELLO\n")
                self.assertEqual(read_line(master), "FILE-RECV 1 READY")
                os.write(master, b"PUT ../escape.apk 1 00000000\n")
                self.assertEqual(read_line(master), "FILE-ERR request")
                payload = b"apk-like test bytes\x00with binary data"
                crc = binascii.crc32(payload) & 0xffffffff
                os.write(master, f"PUT rejected.apk {len(payload)} {crc ^ 1:08x}\n".encode())
                self.assertEqual(read_line(master), "FILE-READY")
                os.write(master, payload)
                self.assertEqual(read_line(master), "FILE-ERR transfer")
                self.assertFalse((incoming / "rejected.apk").exists())
                self.assertFalse(list(incoming.glob(".part-*")))
                os.write(master, f"PUT update.apk {len(payload)} {crc:08x}\n".encode())
                self.assertEqual(read_line(master), "FILE-READY")
                os.write(master, payload)
                self.assertEqual(read_line(master), f"FILE-OK name=update.apk bytes={len(payload)} crc={crc:08x}")
                self.assertEqual((incoming / "update.apk").read_bytes(), payload)
                self.assertFalse(list(incoming.glob(".part-*")))
                os.write(master, f"PUT update.apk {len(payload)} {crc:08x}\n".encode())
                self.assertEqual(read_line(master), "FILE-READY")
                os.write(master, payload)
                self.assertEqual(read_line(master), "FILE-ERR transfer")
                self.assertEqual((incoming / "update.apk").read_bytes(), payload)
                self.assertFalse(list(incoming.glob(".part-*")))
                self.assertFalse((Path(tmp) / "escape.apk").exists())
            finally:
                os.close(master)
                os.close(slave_fd)
                proc.send_signal(signal.SIGTERM)
                proc.wait(timeout=5)
                proc.stderr.close()

    def test_aarch64_display_rejects_malformed_jpeg(self):
        rootfs = ROOT / "runtime"
        with tempfile.NamedTemporaryFile(prefix="a1599-bad-jpeg-", suffix=".jpg") as source:
            source.write(b"\xff\xd8\xff\xe0malformed")
            source.flush()
            result = subprocess.run(
                [QEMU, "-E", f"LD_LIBRARY_PATH={rootfs / 'usr/lib'}", "-L", str(rootfs),
                 str(rootfs / "sbin/displayd"), "--decode-only", source.name],
                capture_output=True, timeout=10,
            )
        self.assertEqual(result.returncode, 3)

    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="a1599-displayd-test-")
        cls.jpeg = Path(cls.temp.name) / "smoke.jpg"
        Image.new("RGB", (64, 48), (20, 90, 180)).save(cls.jpeg, quality=82)
        cls.native = Path(cls.temp.name) / "displayd-test"
        subprocess.run([
            "cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
            "-DDISPLAYD_TEST_BACKEND", str(ROOT / "source" / "displayd.c"),
            "-ljpeg", "-o", str(cls.native),
        ], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_display_sessions_restart_in_same_process_after_bad_frame(self):
        master, slave_fd = pty.openpty()
        slave = os.ttyname(slave_fd)
        proc = subprocess.Popen([str(self.native), slave], stdin=subprocess.DEVNULL,
                                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        payload = self.jpeg.read_bytes()
        crc = binascii.crc32(payload) & 0xffffffff
        try:
            self.assertEqual(read_line(master), "DISPLAYD 1 READY")
            os.write(master, b"HELLO\n")
            self.assertEqual(read_line(master), "DISPLAYD 1 READY")
            os.write(master, b"START\n")
            self.assertEqual(read_line(master), "SESSION-READY seq=1")
            os.write(master, f"FRAME 1 {len(payload)} {crc ^ 1:08x}\n".encode())
            self.assertEqual(read_line(master), "FRAME-READY seq=1")
            os.write(master, payload)
            self.assertEqual(read_line(master), "FRAME-ERR seq=1 code=crc")
            self.assertEqual(read_line(master), "SESSION-ENDED; send START to begin again")
            os.write(master, b"START\n")
            self.assertEqual(read_line(master), "SESSION-READY seq=1")
            os.write(master, f"FRAME 1 {len(payload)} {crc:08x}\n".encode())
            self.assertEqual(read_line(master), "FRAME-READY seq=1")
            os.write(master, payload)
            self.assertEqual(read_line(master), f"FRAME-OK seq=1 bytes={len(payload)} crc={crc:08x}")
            os.write(master, b"STOP\n")
            self.assertEqual(read_line(master), "SESSION-STOPPED")
        finally:
            os.close(master)
            os.close(slave_fd)
            proc.send_signal(signal.SIGTERM)
            proc.wait(timeout=5)
        diagnostics = proc.stderr.read().decode("ascii")
        proc.stderr.close()
        baseline = binascii.crc32(b"\xa5" * FB_BYTES) & 0xffffffff
        frame_crc = int(diagnostics.split("framebuffer_crc=")[1].splitlines()[0], 16)
        restored_crc = int(diagnostics.split("restored_crc=")[1].splitlines()[0], 16)
        self.assertNotEqual(frame_crc, baseline)
        self.assertEqual(restored_crc, baseline)


if __name__ == "__main__":
    unittest.main()
