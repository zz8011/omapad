"""Execute the real setup-gadget control flow against a fake configfs tree."""
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "runtime/sbin/setup-gadget"
INITTAB = ROOT / "runtime/etc/inittab"
WAIT = ROOT / "runtime/sbin/wait-for-device"


class GadgetFallbackTests(unittest.TestCase):
    def make_fixture(self, failed_binds):
        tmp = tempfile.TemporaryDirectory(prefix="a1599-gadget-")
        root = Path(tmp.name)
        sysroot = root / "sys"
        devroot = root / "dev"
        console = root / "console"
        fakebin = root / "bin"
        (sysroot / "class/udc/udc-test").mkdir(parents=True)
        devroot.mkdir()
        fakebin.mkdir()
        console.touch()
        state = root / "bind-count"
        state.write_text("0")

        mkdir_shim = fakebin / "mkdir"
        mkdir_shim.write_text("""#!/bin/sh
/usr/bin/mkdir "$@" || exit $?
cfg="$A1599_SYSROOT/kernel/config/usb_gadget/a1599"
if [ -d "$cfg/configs/c.1/strings/0x409" ]; then
  /usr/bin/mkdir -p "$cfg/functions"
  for attr in idVendor idProduct bcdUSB bcdDevice UDC; do : >"$cfg/$attr"; done
  : >"$cfg/strings/0x409/manufacturer"
  : >"$cfg/strings/0x409/product"
  : >"$cfg/configs/c.1/strings/0x409/configuration"
  : >"$cfg/configs/c.1/MaxPower"
fi
""")
        mkdir_shim.chmod(0o755)

        source = SCRIPT.read_text()
        original = 'bind_udc() { printf \'%s\\n\' "$1" >"$cfg/UDC" 2>>"$CONSOLE"; }'
        simulated = f'''bind_udc() {{
    count=$(cat "$A1599_TEST_STATE")
    count=$((count + 1))
    printf '%s\\n' "$count" >"$A1599_TEST_STATE"
    if [ "$count" -le "$A1599_TEST_FAIL_BINDS" ]; then
        return 16
    fi
    printf '%s\\n' "$1" >"$cfg/UDC"
    ln -s /dev/null "$DEVROOT/ttyGS0"
    if [ -L "$cfg/configs/c.1/acm.usb1" ]; then
        ln -s /dev/null "$DEVROOT/ttyGS1"
        ln -s /dev/null "$DEVROOT/ttyGS2"
    fi
    return 0
}}'''
        if original not in source:
            tmp.cleanup()
            raise AssertionError("bind_udc injection point changed")
        test_script = root / "setup-gadget-test.sh"
        test_script.write_text(source.replace(original, simulated))
        env = {
            **os.environ,
            "PATH": str(fakebin) + ":/usr/bin:/bin",
            "A1599_SYSROOT": str(sysroot),
            "A1599_DEVROOT": str(devroot),
            "A1599_CONSOLE": str(console),
            "A1599_TEST_STATE": str(state),
            "A1599_TEST_FAIL_BINDS": str(failed_binds),
        }
        return tmp, root, env, console, state

    def test_three_function_bind_failure_cleans_and_falls_back_to_shell(self):
        tmp, root, env, console, state = self.make_fixture(1)
        self.addCleanup(tmp.cleanup)
        result = subprocess.run(["/bin/sh", str(root / "setup-gadget-test.sh")], env=env,
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr + console.read_text())
        cfg = Path(env["A1599_SYSROOT"]) / "kernel/config/usb_gadget/a1599"
        self.assertEqual(state.read_text().strip(), "2")
        self.assertEqual(sorted(p.name for p in (cfg / "functions").iterdir()), ["acm.usb0"])
        self.assertTrue((cfg / "configs/c.1/acm.usb0").is_symlink())
        self.assertFalse((cfg / "configs/c.1/acm.usb1").exists())
        self.assertTrue((Path(env["A1599_DEVROOT"]) / "ttyGS0").is_symlink())
        self.assertIn("trying shell-only ACM fallback", console.read_text())

    def test_three_and_single_bind_failures_clean_shell_function_and_stop(self):
        tmp, root, env, console, state = self.make_fixture(2)
        self.addCleanup(tmp.cleanup)
        result = subprocess.run(["/bin/sh", str(root / "setup-gadget-test.sh")], env=env,
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 1)
        cfg = Path(env["A1599_SYSROOT"]) / "kernel/config/usb_gadget/a1599"
        self.assertEqual(state.read_text().strip(), "2")
        self.assertEqual(list((cfg / "functions").iterdir()), [])
        self.assertFalse((cfg / "configs/c.1/acm.usb0").exists())
        self.assertEqual((cfg / "UDC").read_text(), "\n")
        self.assertIn("single ACM shell bind failed", console.read_text())

    def test_wait_wrapper_sleeps_until_missing_device_appears(self):
        tmp, root, env, _, _ = self.make_fixture(0)
        self.addCleanup(tmp.cleanup)
        node = Path(env["A1599_DEVROOT"]) / "ttyGS1"
        marker = root / "started"
        proc = subprocess.Popen(["/bin/sh", str(WAIT), str(node), "/usr/bin/touch", str(marker)], env=env)
        time.sleep(0.2)
        self.assertIsNone(proc.poll(), "wrapper exited while endpoint was absent")
        self.assertFalse(marker.exists())
        node.symlink_to("/dev/null")
        self.assertEqual(proc.wait(timeout=3), 0)
        self.assertTrue(marker.exists())

    def test_inittab_never_attaches_init_respawn_to_optional_endpoint(self):
        lines = [line for line in INITTAB.read_text().splitlines() if "ttyGS" in line]
        self.assertEqual(len(lines), 3)
        for line in lines:
            self.assertTrue(line.startswith("::respawn:"), line)
            self.assertIn("/sbin/wait-for-device /dev/", line)
            self.assertNotRegex(line, r"^ttyGS[012]:")


if __name__ == "__main__":
    unittest.main()
