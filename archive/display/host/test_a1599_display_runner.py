import importlib.util
from pathlib import Path
import unittest
from unittest import mock

MODULE_PATH = Path(__file__).with_name("a1599_display_runner.py")
spec = importlib.util.spec_from_file_location("a1599_display_runner", MODULE_PATH)
runner = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(runner)


class RunnerRecoveryTests(unittest.TestCase):
    def test_disconnect_retries_with_bounded_backoff_then_stops_cleanly(self):
        active = True
        calls = []
        delays = []

        def session():
            nonlocal active
            calls.append(len(calls) + 1)
            if len(calls) == 1:
                raise runner.LinkFailure("mock tty disconnect")
            active = False

        failures = runner.supervise(session, lambda: active, sleep_fn=delays.append)
        self.assertEqual(failures, 1)
        self.assertEqual(calls, [1, 2])
        self.assertEqual(delays, [1.0])

    def test_identity_mismatch_is_fatal_and_never_retries(self):
        active = True
        calls = []
        delays = []

        def session():
            calls.append(1)
            raise runner.IdentityMismatch("mock wrong USB port")

        with self.assertRaises(runner.IdentityMismatch):
            runner.supervise(session, lambda: active, sleep_fn=delays.append)
        self.assertEqual(calls, [1])
        self.assertEqual(delays, [])

    def test_capture_never_omits_exact_output_selector(self):
        command = ["grim", "-o", runner.OUTPUT, "-t", "jpeg", "-q", "75", "-"]
        with mock.patch.object(runner.subprocess, "Popen") as popen:
            proc = mock.Mock()
            proc.stdout = mock.Mock()
            proc.stdout.fileno.return_value = 99
            proc.poll.return_value = 0
            proc.wait.return_value = 0
            popen.return_value = proc
            with mock.patch.object(runner.select, "select", return_value=([proc.stdout], [], [])), \
                 mock.patch.object(runner.os, "read", side_effect=[b"", b""]):
                with self.assertRaises(runner.LinkFailure):
                    runner.capture()
            actual = popen.call_args.args[0]
            self.assertEqual(actual[:5], ["sudo", "-n", "-u", "zz8011", "env"])
            self.assertIn(f"XDG_RUNTIME_DIR={runner.HYPR_RUNTIME}", actual)
            self.assertIn(f"WAYLAND_DISPLAY={runner.HYPR_WAYLAND}", actual)
            self.assertIn(f"HYPRLAND_INSTANCE_SIGNATURE={runner.HYPR_SIGNATURE}", actual)
            self.assertEqual(actual[-len(command):], command)


if __name__ == "__main__":
    unittest.main()
