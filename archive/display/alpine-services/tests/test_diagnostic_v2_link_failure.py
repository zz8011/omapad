"""Regression coverage for the bootdiag-v2 configfs link failure path."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "diagnostic-v2/runtime/sbin/setup-gadget"


class ConfigfsLinkFailureTests(unittest.TestCase):
    def test_failed_multifunction_link_retries_shell_only(self):
        with tempfile.TemporaryDirectory(prefix="a1599-link-fallback-") as td:
            root = Path(td)
            sysroot, devroot, fakebin = root / "sys", root / "dev", root / "bin"
            (sysroot / "class/udc/udc-test").mkdir(parents=True)
            devroot.mkdir()
            fakebin.mkdir()
            console = root / "console"
            console.touch()
            calls = root / "ln-count"
            calls.write_text("0")
            (fakebin / "mkdir").write_text(f"""#!/bin/sh
/usr/bin/mkdir "$@" || exit $?
cfg="$A1599_SYSROOT/kernel/config/usb_gadget/a1599"
if [ -d "$cfg/configs/c.1/strings/0x409" ]; then
  /usr/bin/mkdir -p "$cfg/functions"
  for attr in idVendor idProduct bcdUSB bcdDevice UDC; do : >"$cfg/$attr"; done
  : >"$cfg/strings/0x409/manufacturer"; : >"$cfg/strings/0x409/product"
  : >"$cfg/configs/c.1/strings/0x409/configuration"; : >"$cfg/configs/c.1/MaxPower"
fi
""")
            (fakebin / "mkdir").chmod(0o755)
            (fakebin / "ln").write_text(f"""#!/bin/sh
n=$(cat '{calls}'); n=$((n + 1)); printf '%s\\n' "$n" >'{calls}'
if [ "$n" -eq 2 ]; then echo 'ln: simulated configfs resource failure' >&2; exit 1; fi
exec /usr/bin/ln "$@"
""")
            (fakebin / "ln").chmod(0o755)
            script = root / "setup-gadget"
            script.write_text(SCRIPT.read_text().replace(
                'bind_udc() { printf \'%s\\n\' "$1" >"$cfg/UDC" 2>>"$CONSOLE"; }',
                '''bind_udc() {
  printf '%s\\n' "$1" >"$cfg/UDC" 2>>"$CONSOLE" || return $?
  /usr/bin/ln -sf /dev/null "$DEVROOT/ttyGS0"
  [ -L "$cfg/configs/c.1/acm.usb1" ] && { /usr/bin/ln -sf /dev/null "$DEVROOT/ttyGS1"; /usr/bin/ln -sf /dev/null "$DEVROOT/ttyGS2"; }
  return 0
}'''))
            env = {**os.environ, "PATH": f"{fakebin}:/usr/bin:/bin",
                   "A1599_SYSROOT": str(sysroot), "A1599_DEVROOT": str(devroot),
                   "A1599_CONSOLE": str(console)}
            result = subprocess.run(["/bin/sh", str(script)], env=env,
                                    capture_output=True, text=True, timeout=10)
            output = console.read_text()
            cfg = sysroot / "kernel/config/usb_gadget/a1599"
            self.assertEqual(result.returncode, 0, result.stderr + output)
            self.assertIn("trying shell-only ACM fallback", output)
            self.assertIn("stage=verify-ttyGS0 ok shell-only fallback", output)
            self.assertEqual(sorted(p.name for p in (cfg / "functions").iterdir()), ["acm.usb0"])
            self.assertTrue((cfg / "configs/c.1/acm.usb0").is_symlink())


if __name__ == "__main__":
    unittest.main()
