"""Offline static and branch checks for the A1599 init scripts.

These tests use temporary fake mount/console/sysfs paths and never touch USB or
the target device. The configfs gadget success path requires a real kernel and
is deliberately not simulated here.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
RC_S = ROOT / "runtime/etc/init.d/rcS"
GADGET = ROOT / "runtime/sbin/setup-gadget"


class InitScriptTests(unittest.TestCase):
    def test_overlay_and_runtime_scripts_are_identical(self):
        self.assertEqual(RC_S.read_bytes(), (ROOT / "overlay/etc/init.d/rcS").read_bytes())
        self.assertEqual(GADGET.read_bytes(), (ROOT / "overlay/sbin/setup-gadget").read_bytes())

    def test_scripts_parse_with_host_shell(self):
        for script in (RC_S, GADGET):
            result = subprocess.run(["/bin/sh", "-n", str(script)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_init_fails_with_stage_log_when_mount_fails(self):
        with tempfile.TemporaryDirectory(prefix="a1599-init-test-") as tmp:
            temp = Path(tmp)
            console = temp / "console"
            console.touch()
            fakebin = temp / "bin"
            fakebin.mkdir()
            for name in ("grep", "mkdir", "awk"):
                path = shutil.which(name)
                if path:
                    (fakebin / name).symlink_to(path)
            # Present an already-mounted proc and force the first needed mount
            # (devtmpfs) to fail deterministically.
            fakebin.joinpath("mount").write_text("#!/bin/sh\nexit 37\n")
            fakebin.joinpath("mount").chmod(0o755)
            shim = temp / "rcS"
            content = RC_S.read_text().replace("/dev/console", str(console))
            content = content.replace("PATH=/sbin:/bin:/usr/sbin:/usr/bin", f"PATH={fakebin}")
            content = content.replace("mount -t proc proc /proc", "true # proc already mounted by test")
            content = content.replace("[ -r /proc/mounts ] || fail proc_mount_table 1", "true")
            start = content.index("is_mounted() {")
            end = content.index("\nmount_once()", start)
            content = content[:start] + 'is_mounted() { [ "$1" = /proc ] && return 0 || return 1; }' + content[end:]
            shim.write_text(content)
            result = subprocess.run(["/bin/sh", str(shim)], env={**os.environ, "PATH": str(fakebin)}, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            logs = console.read_text()
            self.assertIn("stage=devtmpfs", logs)
            self.assertIn("mount failed type=devtmpfs", logs)
            self.assertIn("FATAL stage=devtmpfs status=37", logs)

    def test_gadget_has_explicit_endpoint_failure_and_unbind_rollback(self):
        source = GADGET.read_text()
        self.assertIn("stage=function-create", source)
        self.assertIn("check ACM function support/endpoint resources", source)
        self.assertIn("bind-udc", source)
        self.assertIn("rollback-unbind", source)
        self.assertIn("ttyGS0/ttyGS1/ttyGS2", source)


if __name__ == "__main__":
    unittest.main()
