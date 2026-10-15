#!/usr/bin/env python3
"""Temporary A1599 H.264/NUT stream from A1599-RAM to the iPad.

Run as root through the approved sudo path. Uses ttyACM1 for the video byte
stream, ttyACM2 only for displayd snapshot START/STOP, and ttyACM0 briefly for
target process control. The Alpine receiver uses FFmpeg's NUT demuxer and
fbdev output. All files and logs live in /run on RAM filesystems.
"""
from __future__ import annotations

import fcntl
import argparse
import json
import os
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
TARGET_W, TARGET_H = 1536, 2048
FPS = 5
TTY_MAINT = "/dev/ttyACM0"
TTY_STREAM = "/dev/ttyACM1"
TTY_DISPLAY = "/dev/ttyACM2"
USB_ROOT = "/sys/bus/usb/devices/1-12"
USB_PARENT = "/sys/devices/pci0000:00/0000:00:14.0/usb1/1-12"
PID_FILE = "/run/a1599-h264-host.pid"
LOCK_FILE = "/run/a1599-usb-display.lock"
TARGET_PID_FILE = "/run/a1599-h264.pids"
TARGET_LOG = "/run/a1599-h264-target.log"
REC_LOG = "/run/a1599-h264-wf-recorder.log"
RUNTIME = "/run/user/1000"
WAYLAND = "wayland-1"
HYPR_SIG = "efb50993780079460b0cbed1363e2166a2de1d9f_1791809838_1454362110"
running = True


class LinkError(RuntimeError):
    pass


def log(s: str) -> None:
    print(f"{time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())} pid={os.getpid()} {s}", flush=True)


def on_signal(_sig, _frame):
    global running
    running = False


def verify_tty(path: str, minor: int, iface: str) -> os.stat_result:
    node = os.stat(path)
    if not stat.S_ISCHR(node.st_mode) or (os.major(node.st_rdev), os.minor(node.st_rdev)) != (166, minor):
        raise LinkError(f"{path} is not expected tty 166:{minor}")
    if os.path.realpath("/sys/class/tty/" + Path(path).name + "/device") != USB_PARENT + "/1-12" + iface:
        raise LinkError(f"{path} is no longer on the pinned USB function {iface}")
    for leaf, wanted in (("idVendor", "1d6b"), ("idProduct", "0104"), ("busnum", "1"), ("devpath", "12")):
        actual = Path(USB_ROOT, leaf).read_text().strip().lower()
        if actual != wanted:
            raise LinkError(f"USB {leaf} changed to {actual!r}")
    return node


def monitor_state() -> dict[str, tuple]:
    p = subprocess.run(
        ["sudo", "-n", "-u", "zz8011", "env", f"XDG_RUNTIME_DIR={RUNTIME}",
         f"WAYLAND_DISPLAY={WAYLAND}", f"HYPRLAND_INSTANCE_SIGNATURE={HYPR_SIG}",
         "hyprctl", "-j", "monitors"],
        capture_output=True, check=True, timeout=6,
    )
    data = json.loads(p.stdout)
    return {m["name"]: (m.get("width"), m.get("height"), m.get("x"), m.get("y"), m.get("transform"))
            for m in data if m.get("name") in {"eDP-1", "DP-1", "HDMI-A-1", OUTPUT}}


def verify_layout(baseline: dict[str, tuple]) -> None:
    now = monitor_state()
    if now != baseline:
        raise LinkError(f"Hyprland output layout changed; refusing capture: {now}")
    if OUTPUT not in now or now[OUTPUT][:2] != (TARGET_W, TARGET_H) or now[OUTPUT][4] != 0:
        raise LinkError(f"virtual output is not native portrait {TARGET_W}x{TARGET_H}: {now.get(OUTPUT)}")


def open_raw(path: str, minor: int, iface: str, flush_input: bool = True):
    before = verify_tty(path, minor, iface)
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK | os.O_CLOEXEC)
    try:
        verify_tty(path, minor, iface)
        if os.fstat(fd).st_rdev != before.st_rdev:
            raise LinkError(f"{path} changed while opening")
        saved = termios.tcgetattr(fd)
        raw = saved[:]
        raw[0] = raw[1] = raw[3] = 0
        raw[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        raw[4] = raw[5] = termios.B115200
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
        if flush_input:
            termios.tcflush(fd, termios.TCIFLUSH)
        return fd, saved
    except Exception:
        os.close(fd)
        raise


def put(fd: int, data: bytes, timeout: float = 8.0, label: str = "serial") -> float:
    view = memoryview(data)
    deadline = time.monotonic() + timeout
    blocked = 0.0
    sent = 0
    while view:
        try:
            n = os.write(fd, view)
            if n:
                view = view[n:]
                sent += n
                continue
        except BlockingIOError:
            pass
        except OSError as exc:
            raise LinkError(f"{label} write failed after {sent}/{len(data)} bytes: {exc}") from exc
        if time.monotonic() >= deadline:
            raise LinkError(f"{label} write timed out after {sent}/{len(data)} bytes; pending={len(view)}")
        t = time.monotonic()
        pause = min(0.01, deadline - t)
        time.sleep(pause)
        blocked += time.monotonic() - t
    return blocked


def readline(fd: int, timeout: float = 8.0) -> str:
    out = bytearray()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            chunk = os.read(fd, 1)
        except BlockingIOError:
            time.sleep(min(0.01, max(0.0, deadline - time.monotonic())))
            continue
        if not chunk:
            raise LinkError("serial read returned EOF")
        if chunk == b"\n":
            return out.decode("ascii", "replace").rstrip("\r")
        if len(out) >= 256:
            raise LinkError("oversized serial response")
        out.extend(chunk)
    raise LinkError("serial response timed out")


def release_tty_bounded(fd: int, saved, label: str, timeout: float = 1.0) -> bool:
    """Restore termios/close in a daemon worker; cdc_acm close can stall."""
    def finalize():
        try:
            current = termios.tcgetattr(fd)
            if current != saved:
                termios.tcsetattr(fd, termios.TCSANOW, saved)
        except Exception as exc:
            log(f"{label} termios restore unconfirmed: {exc}")
        try:
            os.close(fd)
        except Exception as exc:
            log(f"{label} close unconfirmed: {exc}")

    worker = threading.Thread(target=finalize, name=f"{label}-close", daemon=True)
    try:
        worker.start()
    except Exception as exc:
        log(f"{label} close worker failed to start: {exc}")
        return False
    worker.join(timeout)
    if worker.is_alive():
        log(f"{label} termios restore/close still blocked after {timeout:.1f}s")
        return False
    return True


def start_display_snapshot():
    fd, saved = open_raw(TTY_DISPLAY, 2, ":1.4")
    try:
        put(fd, b"HELLO\n")
        deadline = time.monotonic() + 8
        while True:
            if readline(fd, max(0.2, deadline - time.monotonic())) == "DISPLAYD 1 READY":
                break
            if time.monotonic() > deadline:
                raise LinkError("displayd did not become ready")
        put(fd, b"START\n")
        if readline(fd) != "SESSION-READY seq=1":
            raise LinkError("displayd did not save its framebuffer snapshot")
        return fd, saved
    except Exception:
        release_tty_bounded(fd, saved, "ttyACM2")
        raise


def stop_display_snapshot(fd: int, saved) -> None:
    try:
        put(fd, b"STOP\n", 3)
        response = readline(fd, 5)
        if response != "SESSION-STOPPED":
            log(f"framebuffer restore response={response!r}")
        else:
            log("SESSION-STOPPED; saved framebuffer restored")
    except Exception as exc:
        log(f"framebuffer restore unconfirmed: {exc}")
    try:
        termios.tcsetattr(fd, termios.TCSANOW, saved)
    finally:
        os.close(fd)


def shell_command(command: str, marker: str, timeout: float = 20.0) -> list[str]:
    # Match the previously validated maintenance-shell handshake: wake the
    # no-login shell with CR, drain prompt bytes, then send a CRLF command.
    # Preserve pending prompt output until after this synchronization.
    fd, saved = open_raw(TTY_MAINT, 0, ":1.0", flush_input=False)
    lines: list[str] = []
    rx = bytearray()
    try:
        log(f"ACM0 termios before sync: iflag={saved[0]:#x} oflag={saved[1]:#x} "
            f"cflag={saved[2]:#x} lflag={saved[3]:#x} "
            f"VMIN={saved[6][termios.VMIN]} VTIME={saved[6][termios.VTIME]}")
        try:
            modem = bytearray(4)
            fcntl.ioctl(fd, termios.TIOCMGET, modem, True)
            bits = int.from_bytes(modem, byteorder=sys.byteorder)
            log(f"ACM0 modem lines after open: DTR={bool(bits & termios.TIOCM_DTR)} "
                f"RTS={bool(bits & termios.TIOCM_RTS)} bits={bits:#x}")
        except (AttributeError, OSError) as exc:
            log(f"ACM0 modem-line query unavailable: {exc}")
        put(fd, b"\r", 2, TTY_MAINT)
        time.sleep(0.2)
        startup = bytearray()
        while len(startup) < 8192:
            try:
                chunk = os.read(fd, 4096)
            except BlockingIOError:
                break
            if not chunk:
                break
            startup.extend(chunk)
            if b"\x1b[6n" in startup:
                startup[:] = startup.replace(b"\x1b[6n", b"")
                put(fd, b"\x1b[1;1R", 2, TTY_MAINT)
        put(fd, (command + "\r\n").encode(), 10, TTY_MAINT)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                chunk = os.read(fd, 8192)
                if chunk:
                    rx.extend(chunk)
                else:
                    raise LinkError(f"{TTY_MAINT} returned EOF")
            except BlockingIOError:
                time.sleep(min(0.02, max(0.0, deadline - time.monotonic())))
            if b"\x1b[6n" in rx:
                rx[:] = rx.replace(b"\x1b[6n", b"")
                put(fd, b"\x1b[1;1R")
            while b"\n" in rx:
                raw, _, rest = rx.partition(b"\n")
                rx[:] = rest
                line = raw.decode("utf-8", "replace").rstrip("\r")
                lines.append(line)
                if marker in line:
                    return lines
        raise LinkError(f"maintenance shell timed out waiting for {marker}; output={lines[-6:]}")
    finally:
        if not release_tty_bounded(fd, saved, TTY_MAINT):
            raise LinkError(f"{TTY_MAINT} cleanup blocked; refusing to continue serial setup")


def start_target() -> tuple[int, int]:
    command = (
        f"p=$(pidof file-recv); if [ -z \"$p\" ]; then printf '\\1011599_SETUP_ERR:no-file-recv\\n'; "
        f"else kill -STOP $p; ffmpeg -nostdin -hide_banner -loglevel info -stats -stats_period 1 "
        f"-fflags nobuffer -flags low_delay -probesize 32768 -analyzeduration 0 -thread_queue_size 1 "
        f"-f nut -i /dev/ttyGS1 -map 0:v:0 -an -vf format=bgra -pix_fmt bgra -fps_mode passthrough "
        f"-f fbdev -s {TARGET_W}x{TARGET_H} /dev/fb0 </dev/ttyGS1 >/run/a1599-h264-target.log 2>&1 & "
        f"f=$!; echo $p $f >/run/a1599-h264.pids; printf '\\1011599_SETUP_READY:%s:%s\\n' $p $f; fi"
    )
    lines = shell_command(command, "A1599_SETUP_")
    result = next((s for s in lines if "A1599_SETUP_READY:" in s), None)
    if result is None:
        raise LinkError(f"target setup failed: {lines[-6:]}")
    import re
    m = re.search(r"A1599_SETUP_READY:(\d+):(\d+)", result)
    if not m:
        raise LinkError(f"could not parse target PIDs: {result!r}")
    return int(m.group(1)), int(m.group(2))


def cleanup_target(recv_pid: int | None, ff_pid: int | None) -> None:
    if recv_pid and ff_pid:
        command = (
            f"kill -TERM {ff_pid} 2>/dev/null; i=0; while kill -0 {ff_pid} 2>/dev/null && [ $i -lt 15 ]; "
            f"do sleep 0.1; i=$((i+1)); done; if kill -0 {ff_pid} 2>/dev/null; then "
            f"kill -KILL {ff_pid} 2>/dev/null; fi; wait {ff_pid} 2>/dev/null; "
            f"kill -CONT {recv_pid} 2>/dev/null; "
            f"rm -f {TARGET_PID_FILE}; printf '\\1011599_TARGET_STOPPED\\n'"
        )
    elif recv_pid:
        command = f"kill -CONT {recv_pid} 2>/dev/null; rm -f {TARGET_PID_FILE}; printf '\\1011599_TARGET_RESUMED\\n'"
    else:
        return
    try:
        lines = shell_command(command, "A1599_TARGET_", 15)
        log("target cleanup: " + " | ".join(lines[-2:]))
    except Exception as exc:
        log(f"target cleanup unconfirmed: {exc}")


def cleanup_target_worker(recv_pid: int | None, ff_pid: int | None) -> None:
    try:
        cleanup_target(recv_pid, ff_pid)
    except Exception as exc:
        log(f"target cleanup worker failed: {exc}")


def restore_display_worker(display_saved) -> None:
    fd = None
    try:
        fd, current = open_raw(TTY_DISPLAY, 2, ":1.4", flush_input=False)
        stop_display_snapshot(fd, display_saved if display_saved is not None else current)
        fd = None  # stop_display_snapshot owns and closes it
    except Exception as exc:
        log(f"display cleanup worker unconfirmed: {exc}")
    finally:
        if fd is not None:
            try:
                os.close(fd)
            except OSError:
                pass


def restore_stream_tty_worker(saved) -> None:
    if saved is None:
        return
    fd = None
    try:
        verify_tty(TTY_STREAM, 1, ":1.2")
        fd = os.open(TTY_STREAM, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK | os.O_CLOEXEC)
        verify_tty(TTY_STREAM, 1, ":1.2")
        termios.tcsetattr(fd, termios.TCSANOW, saved)
    except Exception as exc:
        log(f"ttyACM1 terminal restore unconfirmed: {exc}")
    finally:
        if fd is not None:
            try:
                os.close(fd)
            except OSError:
                pass


def runner_cmdline_matches(pid: int) -> bool:
    try:
        argv = [os.fsdecode(part) for part in Path(f"/proc/{pid}/cmdline").read_bytes().split(b"\0") if part]
    except (FileNotFoundError, ProcessLookupError, PermissionError, OSError):
        return False
    expected = os.path.realpath(__file__)
    return any(os.path.realpath(arg) == expected for arg in argv[1:4])


def clear_pid_file_if_unchanged(raw: str) -> None:
    try:
        st = os.lstat(PID_FILE)
        if not stat.S_ISREG(st.st_mode) or st.st_uid != 0:
            return
        if Path(PID_FILE).read_text().strip() == raw:
            os.unlink(PID_FILE)
    except OSError:
        pass


def stop_runner() -> int:
    """Stop only the process named by our PID file, then wait for cleanup."""
    try:
        raw = Path(PID_FILE).read_text().strip()
    except FileNotFoundError:
        print("A1599 H.264 runner is not active (no PID file).")
        return 0
    if not raw.isdecimal() or len(raw) > 10:
        print("Refusing to signal an invalid A1599 runner PID file.", file=sys.stderr)
        return 2
    pid = int(raw)
    if pid <= 1 or not runner_cmdline_matches(pid):
        clear_pid_file_if_unchanged(raw)
        print("No matching A1599 H.264 runner; stale PID file cleared.")
        return 0

    pidfd = None
    try:
        if hasattr(os, "pidfd_open"):
            pidfd = os.pidfd_open(pid, 0)
        if pidfd is not None and hasattr(signal, "pidfd_send_signal"):
            signal.pidfd_send_signal(pidfd, signal.SIGTERM, None, 0)
        elif runner_cmdline_matches(pid):
            os.kill(pid, signal.SIGTERM)
        else:
            clear_pid_file_if_unchanged(raw)
            print("A1599 H.264 runner exited before the stop signal.")
            return 0

        deadline = time.monotonic() + 25.0
        while time.monotonic() < deadline and runner_cmdline_matches(pid):
            time.sleep(0.1)
        if runner_cmdline_matches(pid):
            print("Stop signal sent; runner cleanup is still in progress. Check /run/a1599-h264-host.log.",
                  file=sys.stderr)
            return 1
        clear_pid_file_if_unchanged(raw)
        print("A1599 H.264 runner stopped; local cleanup completed.")
        return 0
    except ProcessLookupError:
        clear_pid_file_if_unchanged(raw)
        print("A1599 H.264 runner already exited.")
        return 0
    except PermissionError as exc:
        print(f"Could not signal A1599 H.264 runner: {exc}", file=sys.stderr)
        return 1
    finally:
        if pidfd is not None:
            os.close(pidfd)


def spawn_recorder(stderr_fd: int):
    env = {
        "PATH": "/usr/local/bin:/usr/bin:/bin",
        "HOME": "/home/zz8011",
        "XDG_RUNTIME_DIR": RUNTIME,
        "WAYLAND_DISPLAY": WAYLAND,
        "HYPRLAND_INSTANCE_SIGNATURE": HYPR_SIG,
    }
    cmd = [
        "sudo", "-n", "-u", "zz8011", "env",
        f"XDG_RUNTIME_DIR={RUNTIME}", f"WAYLAND_DISPLAY={WAYLAND}", f"HYPRLAND_INSTANCE_SIGNATURE={HYPR_SIG}",
        "wf-recorder", "-o", OUTPUT, "-D", "-r", str(FPS), "-c", "h264_nvenc", "-b", "0",
        "-p", "preset=p4", "-p", "tune=ll", "-p", "rc=constqp", "-p", "qp=30", "-p", f"g={FPS}",
        "-m", "nut", "-f", "pipe:1",
    ]
    return subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=stderr_fd,
                            close_fds=True, env={**os.environ, **env}, start_new_session=True)


def run_stream(fd: int, proc: subprocess.Popen) -> None:
    assert proc.stdout is not None
    start = last_log = time.monotonic()
    total = blocked_total = 0
    while running:
        ready, _, _ = select.select([proc.stdout], [], [], 0.5)
        if not ready:
            if proc.poll() is not None:
                raise LinkError(f"wf-recorder exited with status {proc.returncode}")
            continue
        try:
            chunk = os.read(proc.stdout.fileno(), 65536)
        except BlockingIOError:
            continue
        if not chunk:
            if proc.poll() is None:
                proc.wait(timeout=2)
            raise LinkError(f"wf-recorder closed stream (status {proc.returncode})")
        blocked_total += put(fd, chunk, timeout=8, label=TTY_STREAM)
        total += len(chunk)
        now = time.monotonic()
        if now - last_log >= 5:
            elapsed = now - start
            log(f"stream bytes={total} avg_mbps={total*8/max(elapsed,0.001)/1e6:.2f} "
                f"tty_write_wait_ms={blocked_total*1000:.1f} elapsed={elapsed:.1f}s")
            last_log = now


def main() -> int:
    global running, FPS
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--fps", type=int, default=5, help="capture rate (5-30 FPS); default 5")
    ap.add_argument("--stop", action="store_true", help="stop the tracked runner and wait for cleanup")
    args = ap.parse_args()
    if not 5 <= args.fps <= 30:
        ap.error("--fps must be between 5 and 30")
    FPS = args.fps
    if os.geteuid() != 0:
        raise RuntimeError("run through the approved root sudo path")
    if args.stop:
        return stop_runner()
    lockfd = os.open(LOCK_FILE, os.O_RDWR | os.O_CREAT | os.O_CLOEXEC, 0o600)
    try:
        fcntl.flock(lockfd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        os.close(lockfd)
        raise RuntimeError("another A1599 H.264 runner already holds the lock")
    if os.path.exists(PID_FILE):
        os.unlink(PID_FILE)
    fd = os.open(PID_FILE, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC, 0o600)
    os.write(fd, f"{os.getpid()}\n".encode())
    os.close(fd)
    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)
    baseline = None
    display_fd = None
    display_saved = None
    stream_fd = None
    stream_saved = None
    recv_pid = ff_pid = None
    proc = None
    stderr_fd = None
    try:
        baseline = monitor_state()
        verify_layout(baseline)
        if set(baseline) != {"eDP-1", "DP-1", "HDMI-A-1", OUTPUT}:
            raise LinkError(f"expected all physical outputs and {OUTPUT}; got {sorted(baseline)}")
        verify_tty(TTY_MAINT, 0, ":1.0")
        verify_tty(TTY_STREAM, 1, ":1.2")
        verify_tty(TTY_DISPLAY, 2, ":1.4")
        display_fd, display_saved = start_display_snapshot()
        log("displayd snapshot active on ttyACM2; framebuffer unchanged until H.264 frame")
        recv_pid, ff_pid = start_target()
        log(f"target ffmpeg pid={ff_pid}; paused file-recv pid={recv_pid}; RAM log={TARGET_LOG}")
        stream_fd, stream_saved = open_raw(TTY_STREAM, 1, ":1.2")
        stderr_fd = os.open(REC_LOG, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        verify_layout(baseline)
        proc = spawn_recorder(stderr_fd)
        log(f"wf-recorder pid={proc.pid} output={OUTPUT} resolution={TARGET_W}x{TARGET_H} "
            f"target_fps={FPS} codec=h264_nvenc mux=nut ttyACM1=1-12:1d6b:0104")
        run_stream(stream_fd, proc)
        return 0
    finally:
        running = False
        if proc is not None and proc.poll() is None:
            try:
                proc.send_signal(signal.SIGINT)
            except ProcessLookupError:
                pass
            except Exception as exc:
                log(f"wf-recorder SIGINT failed: {exc}")
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                try:
                    proc.terminate()
                except ProcessLookupError:
                    pass
                except Exception as exc:
                    log(f"wf-recorder TERM failed: {exc}")
                try:
                    proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    try:
                        proc.kill()
                    except ProcessLookupError:
                        pass
                    except Exception as exc:
                        log(f"wf-recorder KILL failed: {exc}")
                    try:
                        proc.wait(timeout=2)
                    except Exception as exc:
                        log(f"wf-recorder exit unconfirmed: {exc}")
                except Exception as exc:
                    log(f"wf-recorder exit after TERM unconfirmed: {exc}")
            except Exception as exc:
                log(f"wf-recorder exit after SIGINT unconfirmed: {exc}")
        if proc is not None and proc.stdout is not None:
            try:
                proc.stdout.close()
            except OSError as exc:
                log(f"wf-recorder stdout close failed: {exc}")
        if stream_fd is not None:
            try:
                os.close(stream_fd)
            except OSError as exc:
                log(f"ttyACM1 close failed: {exc}")
        if stderr_fd is not None:
            try:
                os.close(stderr_fd)
            except OSError as exc:
                log(f"wf-recorder log close failed: {exc}")
        if display_fd is not None:
            try:
                os.close(display_fd)
            except OSError as exc:
                log(f"ttyACM2 close failed: {exc}")

        # Serial-driver waits can outlive an unplugged ACM function. Keep those
        # best-effort remote restores off the runner's main thread so its local
        # PID file, pipes, and flock are released promptly on disconnect.
        cleanup_threads = []
        if recv_pid is not None or ff_pid is not None:
            cleanup_threads.append(threading.Thread(
                target=cleanup_target_worker, args=(recv_pid, ff_pid), daemon=True))
        if display_fd is not None:
            cleanup_threads.append(threading.Thread(
                target=restore_display_worker, args=(display_saved,), daemon=True))
        if stream_fd is not None and stream_saved is not None:
            cleanup_threads.append(threading.Thread(
                target=restore_stream_tty_worker, args=(stream_saved,), daemon=True))
        for worker in cleanup_threads:
            try:
                worker.start()
            except Exception as exc:
                log(f"remote cleanup worker could not start: {exc}")
        cleanup_deadline = time.monotonic() + 8.0
        for worker in cleanup_threads:
            if worker.ident is None:
                continue
            worker.join(max(0.0, cleanup_deadline - time.monotonic()))
            if worker.is_alive():
                log(f"{worker.name} still waiting on USB cleanup; runner will release local resources")
        try:
            if Path(PID_FILE).read_text().strip() == str(os.getpid()):
                os.unlink(PID_FILE)
        except OSError:
            pass
        try:
            os.close(lockfd)
        except OSError as exc:
            log(f"runner lock close failed: {exc}")


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        log(f"FATAL {type(exc).__name__}: {exc}")
        raise SystemExit(1)
