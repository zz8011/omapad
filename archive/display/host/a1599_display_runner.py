#!/usr/bin/env python3
"""RAM-only Hyprland A1599-RAM -> the pinned A1599 USB display service.

Run via the approved host sudo path with the logged-in Hyprland environment.
Only grim -o A1599-RAM is used. Frames stay in bounded memory; none are saved.
SIGINT/SIGTERM attempts STOP (which restores the iPad framebuffer snapshot).
"""
from __future__ import annotations

import argparse
import binascii
import fcntl
import json
import os
import resource
import select
import signal
import stat
import subprocess
import sys
import termios
import threading
import time
from pathlib import Path

OUTPUT = "A1599-RAM"
TTY = "/dev/ttyACM2"
USB_SYSFS = "/sys/bus/usb/devices/1-12"
TTY_IFACE = "/sys/devices/pci0000:00/0000:00:14.0/usb1/1-12/1-12:1.4"
USB_VID = "1d6b"
USB_PID = "0104"
TTY_MAJOR = 166
TTY_MINOR = 2
MAX_FRAME = 8 * 1024 * 1024
MAX_PIXELS = 8 * 1024 * 1024
MEMORY_LIMIT = 256 * 1024 * 1024
PID_FILE = "/run/a1599-display.pid"
LOCK_FILE = "/run/a1599-usb-display.lock"
SHARED_LOCK_FILE = LOCK_FILE
HYPR_RUNTIME = "/run/user/1000"
HYPR_WAYLAND = "wayland-1"
HYPR_SIGNATURE = "efb50993780079460b0cbed1363e2166a2de1d9f_1791809838_1454362110"
DEFAULT_FPS = 3.0
running = True


class IdentityMismatch(RuntimeError):
    """The expected iPad USB gadget is absent or the tty name was reused."""


class LinkFailure(RuntimeError):
    """A bounded protocol, serial, or capture operation failed."""


def log(message: str) -> None:
    print(f"{time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())} pid={os.getpid()} {message}", flush=True)


def stop_signal(_signum, _frame):
    global running
    running = False


def read_text(path: str) -> str:
    try:
        return Path(path).read_text(encoding="ascii").strip()
    except OSError as exc:
        raise IdentityMismatch(f"cannot read pinned identity file {path}: {exc}") from exc


def verify_usb_identity() -> os.stat_result:
    """Pin ttyACM2 to the known 1-12/1d6b:0104 gadget before every open."""
    try:
        node = os.stat(TTY)
    except OSError as exc:
        raise IdentityMismatch(f"{TTY} unavailable: {exc}") from exc
    if not stat.S_ISCHR(node.st_mode) or (os.major(node.st_rdev), os.minor(node.st_rdev)) != (TTY_MAJOR, TTY_MINOR):
        raise IdentityMismatch(f"{TTY} device number changed; expected 166:2")
    usb = os.path.realpath(USB_SYSFS)
    iface = os.path.realpath("/sys/class/tty/ttyACM2/device")
    if usb != "/sys/devices/pci0000:00/0000:00:14.0/usb1/1-12":
        raise IdentityMismatch(f"USB route 1-12 changed: {usb}")
    if iface != TTY_IFACE:
        raise IdentityMismatch(f"ttyACM2 interface changed: {iface}")
    checks = {
        "idVendor": USB_VID,
        "idProduct": USB_PID,
        "busnum": "1",
        "devpath": "12",
    }
    for leaf, expected in checks.items():
        actual = read_text(os.path.join(USB_SYSFS, leaf))
        if actual.lower() != expected.lower():
            raise IdentityMismatch(f"USB {leaf} changed: {actual!r} (expected {expected!r})")
    return node


def monitor_json() -> list[dict]:
    try:
        p = subprocess.run(["sudo", "-n", "-u", "zz8011", "env",
                            f"XDG_RUNTIME_DIR={HYPR_RUNTIME}", f"WAYLAND_DISPLAY={HYPR_WAYLAND}",
                            f"HYPRLAND_INSTANCE_SIGNATURE={HYPR_SIGNATURE}",
                            "hyprctl", "-j", "monitors"], stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, timeout=3, check=True)
        data = json.loads(p.stdout)
    except (OSError, subprocess.SubprocessError, json.JSONDecodeError) as exc:
        raise LinkFailure(f"cannot query Hyprland monitors: {exc}") from exc
    if not isinstance(data, list):
        raise LinkFailure("Hyprland monitor response was not a list")
    return data


def verify_output() -> dict:
    matches = [m for m in monitor_json() if m.get("name") == OUTPUT]
    if len(matches) != 1:
        raise LinkFailure(f"expected exactly one {OUTPUT} output; found {len(matches)}")
    m = matches[0]
    if m.get("disabled") or (m.get("width"), m.get("height")) != (1536, 2048) or m.get("transform") != 0:
        raise LinkFailure(f"{OUTPUT} is disabled or not 1536x2048: {m}")
    return m


def verify_host_displays(baseline: dict[str, tuple]) -> None:
    now = {m.get("name"): (m.get("width"), m.get("height"), m.get("x"), m.get("y"))
           for m in monitor_json() if m.get("name") in baseline}
    if now != baseline:
        raise LinkFailure(f"physical output layout changed; refusing to capture: {now}")
    verify_output()


def readline(fd: int, timeout: float = 5.0) -> str:
    data = bytearray()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            chunk = os.read(fd, 1)
        except BlockingIOError:
            time.sleep(min(0.01, max(0.0, deadline - time.monotonic())))
            continue
        if not chunk:
            raise LinkFailure("ttyACM2 disconnected")
        if chunk == b"\n":
            return data.decode("ascii", "replace").rstrip("\r")
        if len(data) >= 256:
            raise LinkFailure("displayd returned an oversized line")
        data.extend(chunk)
    raise LinkFailure("timed out waiting for displayd protocol response")


def write_all(fd: int, payload: bytes, timeout: float = 8.0) -> None:
    view = memoryview(payload)
    deadline = time.monotonic() + timeout
    while view:
        try:
            n = os.write(fd, view)
            if n:
                view = view[n:]
                continue
        except BlockingIOError:
            pass
        if time.monotonic() >= deadline:
            raise LinkFailure("timed out writing to ttyACM2")
        time.sleep(min(0.01, max(0.0, deadline - time.monotonic())))


def jpeg_dimensions(data: bytes) -> tuple[int, int]:
    """Read JPEG SOF dimensions without decoding or allocating another frame."""
    if len(data) < 4 or data[:2] != b"\xff\xd8" or data[-2:] != b"\xff\xd9":
        raise LinkFailure("grim returned an incomplete JPEG")
    i = 2
    sof = {0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7, 0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF}
    while i + 3 < len(data):
        if data[i] != 0xFF:
            i += 1
            continue
        while i < len(data) and data[i] == 0xFF:
            i += 1
        if i >= len(data):
            break
        marker = data[i]
        i += 1
        if marker in (0xD8, 0xD9, 0x01) or 0xD0 <= marker <= 0xD7:
            continue
        if i + 2 > len(data):
            break
        length = int.from_bytes(data[i:i + 2], "big")
        if length < 2 or i + length > len(data):
            break
        if marker in sof and length >= 7:
            height = int.from_bytes(data[i + 3:i + 5], "big")
            width = int.from_bytes(data[i + 5:i + 7], "big")
            if not width or not height or width * height > MAX_PIXELS:
                raise LinkFailure(f"JPEG dimensions exceed bounds: {width}x{height}")
            return width, height
        i += length
    raise LinkFailure("grim output has no valid JPEG frame header")


def capture() -> bytes:
    # Explicit -o is mandatory. Never retry grim without an output selector.
    try:
        proc = subprocess.Popen(["sudo", "-n", "-u", "zz8011", "env",
                                 f"XDG_RUNTIME_DIR={HYPR_RUNTIME}", f"WAYLAND_DISPLAY={HYPR_WAYLAND}",
                                 f"HYPRLAND_INSTANCE_SIGNATURE={HYPR_SIGNATURE}",
                                 "grim", "-o", OUTPUT, "-t", "jpeg", "-q", "75", "-"],
                                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, close_fds=True)
    except OSError as exc:
        raise LinkFailure(f"cannot start grim: {exc}") from exc
    assert proc.stdout is not None
    data = bytearray()
    deadline = time.monotonic() + 8.0
    try:
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise LinkFailure("grim capture timed out")
            ready, _, _ = select.select([proc.stdout], [], [], min(0.2, remaining))
            if ready:
                chunk = os.read(proc.stdout.fileno(), min(65536, MAX_FRAME + 1 - len(data)))
                if not chunk:
                    break
                data.extend(chunk)
                if len(data) > MAX_FRAME:
                    raise LinkFailure(f"JPEG exceeds {MAX_FRAME} byte limit")
            elif proc.poll() is not None:
                break
        rc = proc.wait(timeout=max(0.1, deadline - time.monotonic()))
        if rc != 0:
            raise LinkFailure(f"grim failed with status {rc}")
    except Exception:
        proc.kill()
        proc.wait()
        raise
    finally:
        proc.stdout.close()
    if not data:
        raise LinkFailure("grim returned an empty image")
    dimensions = jpeg_dimensions(data)
    if dimensions != (1536, 2048):
        raise LinkFailure(f"capture is {dimensions[0]}x{dimensions[1]}, expected native 1536x2048")
    return bytes(data)


def send_frame(fd: int, seq: int, frame: bytes) -> tuple[str, dict[str, float]]:
    start = time.perf_counter()
    crc = binascii.crc32(frame) & 0xffffffff
    write_all(fd, f"FRAME {seq} {len(frame)} {crc:08x}\n".encode("ascii"))
    ready = readline(fd, timeout=5)
    if ready != f"FRAME-READY seq={seq}":
        raise LinkFailure(f"unexpected FRAME response: {ready}")
    ready_at = time.perf_counter()
    payload_write_start = ready_at
    write_all(fd, frame)
    payload_written = time.perf_counter()
    ack = readline(fd, timeout=12)
    ack_at = time.perf_counter()
    expected = f"FRAME-OK seq={seq} bytes={len(frame)} crc={crc:08x}"
    if ack != expected:
        raise LinkFailure(f"frame ACK mismatch: {ack!r}")
    return ack, {
        "header_to_ready_ms": (ready_at - start) * 1000,
        "payload_write_ms": (payload_written - payload_write_start) * 1000,
        # Includes device USB reception, libjpeg decode, framebuffer copy,
        # and ACK return. The protocol has no target-side timing breakdown.
        "payload_to_ack_ms": (ack_at - payload_written) * 1000,
        "send_to_ack_ms": (ack_at - start) * 1000,
    }


def await_line(fd: int, expected: str, timeout: float = 5.0) -> str:
    deadline = time.monotonic() + timeout
    last = ""
    while time.monotonic() < deadline:
        last = readline(fd, timeout=max(0.1, deadline - time.monotonic()))
        if last == expected:
            return last
        if last.startswith("ERR") or last.startswith("FRAME-ERR"):
            raise LinkFailure(f"displayd replied {last}")
    raise LinkFailure(f"expected {expected!r}, got {last!r}")


def open_pinned_tty():
    before = verify_usb_identity()
    fd = os.open(TTY, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK | os.O_CLOEXEC)
    try:
        after = verify_usb_identity()
        opened = os.fstat(fd)
        if ((before.st_rdev, after.st_rdev, opened.st_rdev) !=
                (os.makedev(TTY_MAJOR, TTY_MINOR), os.makedev(TTY_MAJOR, TTY_MINOR), os.makedev(TTY_MAJOR, TTY_MINOR))):
            raise IdentityMismatch("ttyACM2 changed during open; refusing USB write")
        saved = termios.tcgetattr(fd)
        raw = saved[:]
        raw[0] = 0
        raw[1] = 0
        raw[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        raw[3] = 0
        raw[4] = termios.B115200
        raw[5] = termios.B115200
        raw[6] = saved[6][:]
        raw[6][termios.VMIN] = 1
        raw[6][termios.VTIME] = 0
        cflag_mask = termios.CSIZE | termios.CREAD | termios.CLOCAL | termios.PARENB | termios.CSTOPB
        if hasattr(termios, "CRTSCTS"):
            cflag_mask |= termios.CRTSCTS
        already_raw = (
            saved[0] == saved[1] == saved[3] == 0
            and (saved[2] & cflag_mask) == (termios.CS8 | termios.CREAD | termios.CLOCAL)
            and saved[4] == saved[5] == termios.B115200
            and saved[6][termios.VMIN] == 1 and saved[6][termios.VTIME] == 0
        )
        if not already_raw:
            termios.tcsetattr(fd, termios.TCSANOW, raw)
        termios.tcflush(fd, termios.TCIFLUSH)
        return fd, saved
    except Exception:
        os.close(fd)
        raise


def release_tty_bounded(fd: int, saved, timeout: float = 1.0) -> bool:
    """Restore and close in a daemon worker; cdc_acm close may stall."""
    def finalize():
        try:
            current = termios.tcgetattr(fd)
            if current != saved:
                termios.tcsetattr(fd, termios.TCSANOW, saved)
        except Exception as exc:
            log(f"ttyACM2 termios restore unconfirmed: {exc}")
        try:
            os.close(fd)
        except Exception as exc:
            log(f"ttyACM2 close unconfirmed: {exc}")

    worker = threading.Thread(target=finalize, name="a1599-jpeg-tty-close", daemon=True)
    try:
        worker.start()
    except Exception as exc:
        log(f"ttyACM2 close worker failed to start: {exc}")
        return False
    worker.join(timeout)
    if worker.is_alive():
        log(f"ttyACM2 termios restore/close still blocked after {timeout:.1f}s")
        return False
    return True


def run_session(fps: float, baseline: dict[str, tuple], once: bool = False) -> int:
    global running
    fd, saved_attrs = open_pinned_tty()
    active = False
    start_sent = False
    frames = total_bytes = 0
    started = last_report = time.monotonic()
    try:
        write_all(fd, b"HELLO\n")
        await_line(fd, "DISPLAYD 1 READY")
        start_sent = True
        write_all(fd, b"START\n")
        await_line(fd, "SESSION-READY seq=1")
        active = True
        log(f"session-start output={OUTPUT} tty={TTY} usb=1-12:{USB_VID}:{USB_PID} target_fps={fps}")
        seq = 0
        while running:
            frame_started = time.monotonic()
            verify_host_displays(baseline)
            # Recheck the port identity each frame; the already-open fd must
            # still refer to the same device node before any further writes.
            expected_dev = os.makedev(TTY_MAJOR, TTY_MINOR)
            if os.fstat(fd).st_rdev != expected_dev:
                raise IdentityMismatch("open ttyACM2 descriptor no longer matches pinned device")
            capture_started = time.perf_counter()
            frame = capture()
            capture_ms = (time.perf_counter() - capture_started) * 1000
            seq += 1
            ack, timings = send_frame(fd, seq, frame)
            frames += 1
            total_bytes += len(frame)
            now = time.monotonic()
            if frames == 1 or now - last_report >= 10:
                elapsed = max(now - started, 0.001)
                log(f"ACK {ack}; frames={frames} avg_fps={frames/elapsed:.2f} "
                    f"last_bytes={len(frame)} total_bytes={total_bytes} "
                    f"capture_jpeg_ms={capture_ms:.2f} payload_write_ms={timings['payload_write_ms']:.2f} "
                    f"payload_to_ack_ms={timings['payload_to_ack_ms']:.2f} "
                    f"capture_to_ack_ms={capture_ms + timings['send_to_ack_ms']:.2f}")
                last_report = now
            if once:
                break
            delay = 1.0 / fps - (time.monotonic() - frame_started)
            if delay > 0 and running:
                time.sleep(delay)
    finally:
        if active or start_sent:
            try:
                write_all(fd, b"STOP\n", timeout=2)
                stopped = readline(fd, timeout=3)
                if stopped != "SESSION-STOPPED":
                    log(f"STOP unexpected response={stopped!r}")
                else:
                    log("STOP SESSION-STOPPED (device framebuffer snapshot restored)")
            except (OSError, LinkFailure) as exc:
                log(f"STOP unconfirmed on link loss: {exc}")
        if not release_tty_bounded(fd, saved_attrs):
            log("ttyACM2 close did not complete promptly; stopping before any retry to avoid two writers")
            running = False
            raise LinkFailure("ttyACM2 restore/close did not complete")
    elapsed = max(time.monotonic() - started, 0.001)
    if frames:
        log(f"session-summary frames={frames} elapsed={elapsed:.1f}s avg_fps={frames/elapsed:.2f} bytes={total_bytes}")
    return frames


def retry_delay(failures: int) -> float:
    return min(30.0, float(2 ** min(max(failures - 1, 0), 5)))


def supervise(session_fn, is_running, sleep_fn=time.sleep, max_failures: int | None = None) -> int:
    """Restart a failed link with bounded exponential backoff; injectable for tests."""
    failures = 0
    while is_running():
        try:
            session_fn()
            if not is_running():
                break
            failures += 1  # unexpected clean session return is still a drop
            log("session returned while runner active; reconnecting")
        except IdentityMismatch:
            raise
        except Exception as exc:
            if not is_running():
                break
            failures += 1
            log(f"link/capture failure #{failures}: {exc}")
        if max_failures is not None and failures >= max_failures:
            return failures
        delay = retry_delay(failures)
        log(f"reconnect in {delay:.0f}s")
        sleep_fn(delay)
    return failures


def create_pidfile(path: str) -> None:
    tmp = f"{path}.{os.getpid()}.tmp"
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC, 0o600)
    try:
        os.write(fd, f"{os.getpid()}\n".encode("ascii"))
        os.fsync(fd)
    finally:
        os.close(fd)
    os.replace(tmp, path)


def runner_cmdline_matches(pid: int) -> bool:
    try:
        argv = [os.fsdecode(part) for part in Path(f"/proc/{pid}/cmdline").read_bytes().split(b"\0") if part]
    except OSError:
        return False
    expected = os.path.realpath(__file__)
    return any(os.path.realpath(arg) == expected for arg in argv[1:4])


def stop_runner(pidfile: str, timeout: float = 25.0) -> int:
    try:
        raw = Path(pidfile).read_text().strip()
    except FileNotFoundError:
        print("A1599 display runner is not active (no PID file).")
        return 0
    if not raw.isdecimal() or len(raw) > 10:
        print("Refusing to signal an invalid A1599 display runner PID file.", file=sys.stderr)
        return 2
    pid = int(raw)
    if pid <= 1 or not runner_cmdline_matches(pid):
        try:
            if Path(pidfile).read_text().strip() == raw:
                os.unlink(pidfile)
        except OSError:
            pass
        print("No matching A1599 display runner; stale PID file cleared.")
        return 0
    pidfd = None
    try:
        if hasattr(os, "pidfd_open"):
            pidfd = os.pidfd_open(pid, 0)
        if pidfd is not None and hasattr(signal, "pidfd_send_signal"):
            signal.pidfd_send_signal(pidfd, signal.SIGTERM, None, 0)
        elif runner_cmdline_matches(pid):
            os.kill(pid, signal.SIGTERM)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline and runner_cmdline_matches(pid):
            time.sleep(0.1)
        if runner_cmdline_matches(pid):
            print("Stop signal sent; display runner cleanup is still in progress.", file=sys.stderr)
            return 1
        try:
            if Path(pidfile).read_text().strip() == raw:
                os.unlink(pidfile)
        except OSError:
            pass
        print("A1599 display runner stopped.")
        return 0
    except (ProcessLookupError, PermissionError) as exc:
        print(f"Could not signal A1599 display runner: {exc}", file=sys.stderr)
        return 1
    finally:
        if pidfd is not None:
            os.close(pidfd)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--fps", type=float, default=DEFAULT_FPS, help="target maximum rate, default 3 fps")
    ap.add_argument("--once", action="store_true", help="send one frame, then STOP")
    ap.add_argument("--pidfile", default=PID_FILE)
    ap.add_argument("--lockfile", default=SHARED_LOCK_FILE)
    ap.add_argument("--stop", action="store_true", help="stop the tracked runner and wait for STOP cleanup")
    ap.add_argument("--max-failures", type=int, default=3,
                    help="exit after this many consecutive link/capture failures (default 3)")
    args = ap.parse_args()
    if not 0.2 <= args.fps <= 10:
        ap.error("--fps must be between 0.2 and 10")
    if args.max_failures < 1:
        ap.error("--max-failures must be at least 1")
    if os.geteuid() != 0:
        raise RuntimeError("run only through the approved host sudo path; ttyACM2 access is required")
    if args.stop:
        return stop_runner(args.pidfile)
    resource.setrlimit(resource.RLIMIT_AS, (MEMORY_LIMIT, MEMORY_LIMIT))
    lock_fd = os.open(args.lockfile, os.O_RDWR | os.O_CREAT | os.O_CLOEXEC, 0o600)
    try:
        fcntl.flock(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError as exc:
        os.close(lock_fd)
        raise RuntimeError("another A1599 display runner holds the single-writer lock") from exc
    try:
        if os.path.exists(args.pidfile):
            os.unlink(args.pidfile)
        baseline = {}
        for m in monitor_json():
            name = m.get("name")
            if name in ("eDP-1", "DP-1", "HDMI-A-1"):
                baseline[name] = (m.get("width"), m.get("height"), m.get("x"), m.get("y"))
        if set(baseline) != {"eDP-1", "DP-1", "HDMI-A-1"}:
            raise LinkFailure(f"expected the recorded three physical outputs; found {sorted(baseline)}")
        verify_output()
        verify_usb_identity()
        signal.signal(signal.SIGINT, stop_signal)
        signal.signal(signal.SIGTERM, stop_signal)
        signal.signal(signal.SIGHUP, stop_signal)
        create_pidfile(args.pidfile)
        log(f"runner-start pidfile={args.pidfile} lock={args.lockfile} memory_limit={MEMORY_LIMIT} "
            f"baseline={baseline}; single_output_capture={OUTPUT}")
        if args.once:
            try:
                run_session(args.fps, baseline, once=True)
                return 0
            except IdentityMismatch:
                raise
        failures = supervise(lambda: run_session(args.fps, baseline), lambda: running,
                             max_failures=args.max_failures)
        if failures >= args.max_failures and running:
            log(f"stopping after {failures} consecutive failures; no background retry remains")
            return 1
        return 0
    finally:
        try:
            if os.path.exists(args.pidfile) and Path(args.pidfile).read_text().strip() == str(os.getpid()):
                os.unlink(args.pidfile)
        except OSError:
            pass
        os.close(lock_fd)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        log(f"FATAL {type(exc).__name__}: {exc}")
        raise SystemExit(1)
