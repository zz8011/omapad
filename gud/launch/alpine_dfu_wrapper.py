#!/usr/bin/env python3
"""Fail-closed launcher pinned to the A1599 Alpine RAM system bundle."""

from __future__ import annotations

import datetime as _dt
import hashlib
import json
import os
import select
from pathlib import Path
import re
import secrets
import stat
import subprocess
import sys
import time


ROOT = Path("/home/zz8011/Documents/Codex/2026-10-11/task-2")
CHECKRA1N = ROOT / ".build/a1599/out/checkra1n-0.1337.3-exec"
PONGO = ROOT / ".build/a1599/out/a1599-host-candidate/Pongo.bin"
PONGOTERM = ROOT / ".build/a1599/out/pongoterm"
BUNDLE = Path(__file__).resolve().parent.parent / "out/a1599-gud-snak-powerkey.bundle"
LOG_DIR = Path("/run/a1599-alpine-maintenance")
SYS_USB = Path("/sys/bus/usb/devices")

CHECKRA1N_SHA256 = "53154d87594cf67c56659f24547f1b5bb5336f84ea58c2d42f25a7b2df81a964"
PONGO_SHA256 = "86c9b1989c35a98f7dc2abfe369a06c73b029e9ebae17b29c822a709a008f0b5"
PONGOTERM_SHA256 = "50e68a1c2b516cb984c293601ce93059990286ef859f1dbac8583f0b58f835b3"
BUNDLE_SHA256 = "ea2bbc51dc8f40cd001232a565188302f9965721e0408734d9a1db0ef0666ead"
PONGO_BANNER = "pongoOS 2.6.3-cc532fac"
PONGO_READY_TIMEOUT = 45.0
PONGO_BANNER_TIMEOUT = 30.0
DFU_ID = ("05ac", "1227")
PONGO_ID = ("05ac", "4141")

CHECKRA1N_ARGV = [
    str(CHECKRA1N),
    "--override-pongo",
    str(PONGO),
    "--pongo",
    "--early-exit",
]
PONGOTERM_ARGV = [str(PONGOTERM)]
UPLOAD_BYTES = f"/send {BUNDLE}\nbootm\n".encode("ascii")
SESSION_RE = re.compile(r"^\d{8}T\d{6}Z-[0-9a-f]{12}$")
ANSI_RE = re.compile(rb"\x1b\[[0-?]*[ -/]*[@-~]")


class WrapperError(RuntimeError):
    pass


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def validate_artifacts() -> dict[str, str]:
    expected = {
        "checkra1n": (CHECKRA1N, CHECKRA1N_SHA256),
        "Pongo": (PONGO, PONGO_SHA256),
        "bundle": (BUNDLE, BUNDLE_SHA256),
        "pongoterm": (PONGOTERM, PONGOTERM_SHA256),
    }
    actual: dict[str, str] = {}
    for name, (path, wanted) in expected.items():
        try:
            digest = sha256_file(path)
        except OSError as exc:
            raise WrapperError(f"missing/unreadable fixed {name} artifact") from exc
        if wanted is not None and digest != wanted:
            raise WrapperError(f"fixed {name} SHA-256 mismatch")
        actual[name] = digest
    return actual


def _read_usb_id(device: Path) -> tuple[str, str] | None:
    try:
        vendor = (device / "idVendor").read_text(encoding="ascii").strip().lower()
        product = (device / "idProduct").read_text(encoding="ascii").strip().lower()
    except (OSError, UnicodeError):
        return None
    if not re.fullmatch(r"[0-9a-f]{4}", vendor) or not re.fullmatch(r"[0-9a-f]{4}", product):
        return None
    return vendor, product


def usb_identity(device: Path) -> dict[str, str] | None:
    """Return public USB topology/instance identity, never device serial data."""
    if _read_usb_id(device) is None:
        return None
    try:
        vendor, product = _read_usb_id(device) or ("", "")
        busnum = (device / "busnum").read_text(encoding="ascii").strip()
        devnum = (device / "devnum").read_text(encoding="ascii").strip()
        if not busnum.isdecimal() or not devnum.isdecimal():
            return None
        return {"sysfs_name": device.name, "busnum": busnum, "devnum": devnum,
                "vendor": vendor, "product": product}
    except (OSError, UnicodeError):
        return None


def matching_usb_devices(target: tuple[str, str], sys_usb: Path = SYS_USB) -> list[dict[str, str]]:
    try:
        entries = sorted(sys_usb.iterdir())
    except OSError as exc:
        raise WrapperError("USB sysfs is unavailable") from exc
    return [identity for p in entries if _read_usb_id(p) == target
            if (identity := usb_identity(p)) is not None]


def require_exactly_one(target: tuple[str, str], label: str) -> dict[str, str]:
    found = matching_usb_devices(target)
    if len(found) != 1:
        raise WrapperError(f"expected exactly one {label} USB device; found {len(found)}")
    return found[0]


def require_same_pongo(expected: dict[str, str], label: str = "Pongo") -> dict[str, str]:
    current = require_exactly_one(PONGO_ID, label)
    if current != expected:
        raise WrapperError("Pongo USB identity changed; upload remains gated")
    return current


def _utc_now() -> str:
    return _dt.datetime.now(_dt.timezone.utc).isoformat(timespec="seconds")


def _session_paths(session: str) -> tuple[Path, Path]:
    if not SESSION_RE.fullmatch(session):
        raise WrapperError("invalid session id")
    return (
        LOG_DIR / f"a1599-alpine-wrapper-{session}.jsonl",
        LOG_DIR / f"a1599-alpine-wrapper-{session}.json",
    )


def _write_new_private(path: Path, data: bytes) -> None:
    dirfd = _open_private_log_dir()
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0)
    try:
        fd = os.open(path.name, flags, 0o600, dir_fd=dirfd)
    except BaseException:
        os.close(dirfd)
        raise
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
    except BaseException:
        try:
            os.unlink(path.name, dir_fd=dirfd)
        except OSError:
            pass
        raise
    finally:
        os.close(dirfd)


def _append_event(log_path: Path, event: dict) -> None:
    dirfd = _open_private_log_dir()
    flags = os.O_WRONLY | os.O_APPEND | getattr(os, "O_NOFOLLOW", 0)
    fd = os.open(log_path.name, flags, dir_fd=dirfd)
    try:
        if not _root_owned_regular_private(os.fstat(fd)):
            raise WrapperError("session audit log is not a private root-owned regular file")
        os.fchmod(fd, 0o600)
        with os.fdopen(fd, "a", encoding="utf-8") as f:
            f.write(json.dumps(event, sort_keys=True) + "\n")
    except BaseException:
        raise
    finally:
        os.close(dirfd)


def _save_state(state_path: Path, state: dict) -> None:
    tmp = state_path.with_suffix(".json.tmp")
    dirfd = _open_private_log_dir()
    try:
        flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0)
        fd = os.open(tmp.name, flags, 0o600, dir_fd=dirfd)
        with os.fdopen(fd, "wb") as f:
            f.write((json.dumps(state, sort_keys=True) + "\n").encode())
        os.replace(tmp.name, state_path.name, src_dir_fd=dirfd, dst_dir_fd=dirfd)
    except BaseException:
        try:
            os.unlink(tmp.name, dir_fd=dirfd)
        except OSError:
            pass
        raise
    finally:
        os.close(dirfd)


def _open_private_log_dir() -> int:
    flags = os.O_RDONLY | getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_NOFOLLOW", 0)
    try:
        fd = os.open(LOG_DIR, flags)
    except OSError as exc:
        raise WrapperError("private session log directory is unavailable") from exc
    if not _private_dir_fd_ok(fd):
        os.close(fd)
        raise WrapperError("session log directory must be root-owned and not group/other writable")
    return fd


def _private_dir_fd_ok(fd: int) -> bool:
    st = os.fstat(fd)
    return stat.S_ISDIR(st.st_mode) and st.st_uid == 0 and not (st.st_mode & 0o022)


def _root_owned_regular_private(st) -> bool:
    return stat.S_ISREG(st.st_mode) and st.st_uid == 0 and not (st.st_mode & 0o077)


def _require_private_log_dir() -> None:
    fd = _open_private_log_dir()
    os.close(fd)


def _load_state(state_path: Path) -> dict:
    dirfd = _open_private_log_dir()
    try:
        fd = os.open(state_path.name, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0), dir_fd=dirfd)
        st = os.fstat(fd)
        if not _root_owned_regular_private(st):
            os.close(fd)
            raise WrapperError("session state is not a private regular file")
        with os.fdopen(fd, "r", encoding="utf-8") as f:
            return json.load(f)
    except WrapperError:
        raise
    except (OSError, ValueError) as exc:
        raise WrapperError("session state is missing or invalid") from exc
    finally:
        os.close(dirfd)


def _read_private_log(log_path: Path) -> bytes:
    dirfd = _open_private_log_dir()
    try:
        fd = os.open(log_path.name, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0), dir_fd=dirfd)
        st = os.fstat(fd)
        if not _root_owned_regular_private(st):
            os.close(fd)
            raise WrapperError("session audit log is not a private root-owned regular file")
        with os.fdopen(fd, "rb") as f:
            return f.read()
    except WrapperError:
        raise
    except OSError as exc:
        raise WrapperError("session audit log is missing or unreadable") from exc
    finally:
        os.close(dirfd)


def _require_root() -> None:
    if os.geteuid() != 0:
        raise WrapperError("run this fixed wrapper through the approved elevated sudo path")


def _make_session() -> str:
    return _dt.datetime.now(_dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + secrets.token_hex(6)


def dry_run() -> None:
    hashes = validate_artifacts()
    # Deliberately redact all absolute paths in this display-only mode.
    print("artifact SHA-256 checks: OK")
    for name in ("checkra1n", "Pongo", "bundle", "pongoterm"):
        print(f"{name}: {hashes[name]}")
    print("planned checkra1n argv: [checkra1n, --override-pongo, <Pongo.bin>, --pongo, --early-exit]")
    print("planned Pongo argv: [pongoterm] with one live connection for exact banner check and fixed upload")
    print("planned commands after exact banner: /send <bundle-by-sha256>\\nbootm\\n")
    print(f"expected Pongo banner: {PONGO_BANNER}")
    print("DFU gate: exactly one Apple 05ac:1227 in USB sysfs")
    print("Pongo gate: wait up to 45 seconds for one Apple 05ac:4141 identity, then hold it unchanged through banner check and upload")


def wait_for_pongo(expected_sysfs_name: str, timeout: float = PONGO_READY_TIMEOUT) -> dict[str, str]:
    """Wait through DFU USB reset/re-enumeration without accepting another target."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        found = matching_usb_devices(PONGO_ID)
        if len(found) > 1:
            raise WrapperError("multiple Pongo USB devices appeared; upload remains gated")
        if len(found) == 1:
            if found[0]["sysfs_name"] != expected_sysfs_name:
                raise WrapperError("Pongo enumerated on a different USB port than the authorized DFU target")
            return found[0]
        time.sleep(0.25)
    raise WrapperError(f"Pongo USB device did not become ready within {timeout:g} seconds")


def read_until_banner_prompt(stream, timeout: float = PONGO_BANNER_TIMEOUT,
                             limit: int = 65536) -> bytes:
    """Wait for both the pinned Pongo banner and shell prompt, not prompt alone."""
    output = bytearray()
    deadline = time.monotonic() + timeout
    while len(output) < limit:
        remaining = deadline - time.monotonic()
        if remaining <= 0 or not select.select([stream], [], [], remaining)[0]:
            break
        chunk = os.read(stream.fileno(), min(4096, limit - len(output)))
        if not chunk:
            break
        output.extend(chunk)
        clean = ANSI_RE.sub(b"", output)
        if PONGO_BANNER.encode("ascii") in clean and b"pongoOS> " in clean:
            break
    return bytes(output)


def start() -> str:
    _require_root()
    hashes = validate_artifacts()
    dfu_identity = require_exactly_one(DFU_ID, "DFU")
    if matching_usb_devices(PONGO_ID):
        raise WrapperError("Pongo USB device already present; refusing ambiguous start")
    session = _make_session()
    log_path, state_path = _session_paths(session)
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    _require_private_log_dir()
    _write_new_private(log_path, b"")
    state = {
        "session": session,
        "created_utc": _utc_now(),
        "stage": "starting",
        "hashes": hashes,
        "dfu_identity": dfu_identity,
    }
    _save_state(state_path, state)
    _append_event(log_path, {
        "time_utc": _utc_now(),
        "event": "checkra1n_start",
        "dfu_identity": dfu_identity,
        "argv": CHECKRA1N_ARGV,
        "hashes": hashes,
    })
    try:
        result = subprocess.run(CHECKRA1N_ARGV, check=False, timeout=180)
    except (OSError, subprocess.TimeoutExpired) as exc:
        state["stage"] = "start_failed"
        _save_state(state_path, state)
        _append_event(log_path, {"time_utc": _utc_now(), "event": "checkra1n_exec_error", "error": type(exc).__name__})
        raise WrapperError("could not execute the fixed checkra1n binary") from exc
    state["stage"] = "pongo_pending" if result.returncode == 0 else "start_failed"
    _save_state(state_path, state)
    _append_event(log_path, {"time_utc": _utc_now(), "event": "checkra1n_exit", "returncode": result.returncode})
    if result.returncode != 0:
        raise WrapperError(f"checkra1n exited with status {result.returncode}")
    print(f"Pongo startup returned successfully; next verify the banner with session {session}.")
    print(f"local log: {log_path}")
    return session


def _read_until_prompt(stream, timeout: float, limit: int = 65536) -> bytes:
    """Read pongoterm output incrementally until the prompt or a bounded timeout."""
    output = bytearray()
    deadline = time.monotonic() + timeout
    while len(output) < limit:
        remaining = deadline - time.monotonic()
        if remaining <= 0 or not select.select([stream], [], [], remaining)[0]:
            break
        chunk = os.read(stream.fileno(), min(4096, limit - len(output)))
        if not chunk:
            break
        output.extend(chunk)
        if b"pongoOS> " in ANSI_RE.sub(b"", output):
            break
    return bytes(output)


def _pongoterm_ready_prompt(output: bytes) -> bool:
    """pongoterm locally emits `> ` when the target has no new startup text."""
    clean = ANSI_RE.sub(b"", output).decode("ascii", "replace")
    return clean.endswith("> ")


def _utc_datetime(value: str) -> _dt.datetime:
    parsed = _dt.datetime.fromisoformat(value)
    if parsed.tzinfo is None:
        raise WrapperError("session timestamps lack timezone information")
    return parsed.astimezone(_dt.timezone.utc)


def _read_kernel_usb_history(start: _dt.datetime, end: _dt.datetime) -> list[dict]:
    """Extract only USB port, VID/PID, device number, event kind, and timestamp."""
    cmd = ["journalctl", "-k", "--since", "@" + str(int(start.timestamp())),
           "--until", "@" + str(int(end.timestamp())), "-o", "json", "--no-pager"]
    try:
        result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                timeout=15, check=False)
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise WrapperError("could not read bounded kernel USB history") from exc
    if result.returncode != 0:
        raise WrapperError("kernel USB history is unavailable")
    history: list[dict] = []
    pending: dict[str, tuple[str, str]] = {}
    for raw in (result.stdout or b"").splitlines():
        try:
            row = json.loads(raw)
            message = row.get("MESSAGE", "")
            stamp = _dt.datetime.fromtimestamp(int(row["__REALTIME_TIMESTAMP"]) / 1_000_000,
                                               _dt.timezone.utc)
        except (ValueError, KeyError, TypeError, json.JSONDecodeError):
            continue
        newdev = re.search(r"usb ([0-9]+-[^:]+): new [^:]*device number ([0-9]+)", message)
        if newdev:
            pending[newdev.group(1)] = (newdev.group(2), stamp.isoformat())
            continue
        found = re.search(r"usb ([0-9]+-[^:]+): New USB device found, idVendor=([0-9a-fA-F]{4}), idProduct=([0-9a-fA-F]{4})", message)
        if found:
            port = found.group(1)
            devnum, attached = pending.get(port, ("", stamp.isoformat()))
            history.append({"kind": "attach", "port": port, "devnum": devnum,
                            "vid": found.group(2).lower(), "pid": found.group(3).lower(),
                            "time_utc": attached})
            continue
        gone = re.search(r"usb ([0-9]+-[^:]+): USB disconnect, device number ([0-9]+)", message)
        if gone:
            history.append({"kind": "disconnect", "port": gone.group(1),
                            "devnum": gone.group(2), "time_utc": stamp.isoformat()})
    return history


def validate_resume_continuity(state: dict, audit_events: list[dict], identity: dict,
                               history: list[dict], now: _dt.datetime) -> dict:
    """Bind the saved version banner to the still-enumerated same Pongo instance."""
    if state.get("stage") != "pongo_verified" or state.get("banner") != PONGO_BANNER:
        raise WrapperError("saved Pongo verification is absent or stale")
    created = _utc_datetime(state.get("created_utc", ""))
    verified = _utc_datetime(state.get("verified_utc", ""))
    starts = [e for e in audit_events if e.get("event") == "checkra1n_start"]
    exits = [e for e in audit_events if e.get("event") == "checkra1n_exit" and e.get("returncode") == 0]
    checks = [e for e in audit_events if e.get("event") == "pongo_banner_check_result"
              and e.get("returncode") == 0 and e.get("banner_ok") is True
              and e.get("prompt_ok") is True and e.get("observed_banner") == PONGO_BANNER]
    if len(starts) != 1 or not exits or not checks:
        raise WrapperError("session audit does not prove successful startup and exact Pongo banner")
    if starts[0].get("dfu_sysfs_name") != identity.get("sysfs_name"):
        raise WrapperError("saved DFU/Pongo USB port does not match current Pongo port")
    if any(e.get("pongo_sysfs_name") != identity.get("sysfs_name") for e in
           audit_events if e.get("event") == "pongo_banner_check_start"):
        raise WrapperError("saved Pongo banner check used a different USB port")
    check_times = [_utc_datetime(e.get("time_utc", "")) for e in checks]
    if not any(abs((t - verified).total_seconds()) <= 2 for t in check_times):
        raise WrapperError("saved verification time does not match the successful banner event")
    port = identity.get("sysfs_name", "")
    candidates = [e for e in history if e.get("kind") == "attach" and e.get("port") == port
                  and e.get("vid") == PONGO_ID[0] and e.get("pid") == PONGO_ID[1]
                  and created <= _utc_datetime(e.get("time_utc", "")) <= verified]
    if len(candidates) != 1:
        raise WrapperError("kernel log does not identify one Pongo enumeration for this session")
    attach = candidates[0]
    if attach.get("devnum") != identity.get("devnum"):
        raise WrapperError("current Pongo device number differs from the verified enumeration")
    if str(port).split("-", 1)[0] != str(identity.get("busnum")):
        raise WrapperError("current Pongo bus number does not match its USB port")
    attached_at = _utc_datetime(attach["time_utc"])
    if any(e.get("port") == port and _utc_datetime(e.get("time_utc", "")) > attached_at
           and (e.get("kind") == "attach" or
                (e.get("kind") == "disconnect" and e.get("devnum") == attach.get("devnum")))
           for e in history):
        raise WrapperError("kernel USB history shows Pongo disconnect or re-enumeration")
    if now < verified:
        raise WrapperError("host clock precedes the saved verification time")
    return {"pongo_identity": identity, "kernel_attach_utc": attach["time_utc"],
            "verified_utc": verified.isoformat(timespec="seconds")}


def resume_upload(session: str) -> None:
    """Resume the already verified Pongo session when kernel history proves continuity."""
    _require_root()
    _require_private_log_dir()
    hashes = validate_artifacts()
    log_path, state_path = _session_paths(session)
    state = _load_state(state_path)
    if state.get("session") != session or state.get("hashes") != hashes:
        raise WrapperError("session ID or fixed artifact hashes do not match saved verification")
    audit_bytes = _read_private_log(log_path)
    try:
        audit_events = [json.loads(line) for line in audit_bytes.splitlines() if line]
    except ValueError as exc:
        raise WrapperError("session audit log is malformed") from exc
    identity = require_exactly_one(PONGO_ID, "Pongo")
    created = _utc_datetime(state.get("created_utc", ""))
    now = _dt.datetime.now(_dt.timezone.utc)
    history = _read_kernel_usb_history(created - _dt.timedelta(seconds=5), now)
    continuity = validate_resume_continuity(state, audit_events, identity, history, now)
    source_log_sha256 = hashlib.sha256(audit_bytes).hexdigest()
    check_time = _utc_now()
    _append_event(log_path, {"time_utc": check_time, "event": "saved_pongo_resume_continuity_verified",
                             "source_log_sha256": source_log_sha256,
                             "continuity": continuity})
    usb_id = identity
    proc = None
    try:
        proc = subprocess.Popen(PONGOTERM_ARGV, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, bufsize=0)
        prompt_output = _read_until_prompt(proc.stdout, timeout=8)
        if not _pongoterm_ready_prompt(prompt_output):
            raise WrapperError("Pongo shell prompt is unavailable; no bundle was sent")
        require_same_pongo(usb_id)
        proc.stdin.write(f"/send {BUNDLE}\n".encode("ascii"))
        proc.stdin.flush()
        transfer = _read_until_prompt(proc.stdout, timeout=60)
        transfer_text = ANSI_RE.sub(b"", transfer).decode("ascii", "replace")
        uploaded = f"Uploaded {BUNDLE.stat().st_size} bytes" in transfer_text
        require_same_pongo(usb_id)
        _append_event(log_path, {"time_utc": _utc_now(), "event": "resume_fixed_payload_transfer_result",
                                 "uploaded_marker": uploaded, "pongo_identity": usb_id})
        if not uploaded:
            raise WrapperError("fixed bundle upload was not confirmed; bootm was not sent")
        proc.stdin.write(b"bootm\n")
        proc.stdin.flush()
        try:
            tail = _read_until_prompt(proc.stdout, timeout=10)
        except (OSError, ValueError):
            tail = b""
        rendered = ANSI_RE.sub(b"", tail).decode("ascii", "replace")
        bad_boot = "Bad command: bootm" in rendered
        proc.stdin.close()
        try:
            returncode = proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.terminate()
            returncode = None
        _append_event(log_path, {"time_utc": _utc_now(), "event": "resume_fixed_payload_result",
                                 "returncode": returncode, "uploaded_marker": uploaded,
                                 "bootm_rejected": bad_boot,
                                 "usb_disconnect_after_upload": "USB error:" in rendered})
        if bad_boot:
            raise WrapperError("Pongo rejected bootm; no frame command was sent")
    except WrapperError:
        if proc is not None and proc.poll() is None:
            proc.terminate()
        raise
    except (OSError, subprocess.TimeoutExpired) as exc:
        if proc is not None and proc.poll() is None:
            proc.terminate()
        raise WrapperError("Pongo resume connection failed; no frame command was sent") from exc
    finally:
        if proc is not None:
            for stream in (proc.stdin, proc.stdout):
                try:
                    if stream and not stream.closed:
                        stream.close()
                except OSError:
                    pass
    state["stage"] = "bootm_sent"
    state["resume_source_log_sha256"] = source_log_sha256
    state["resume_checked_utc"] = check_time
    state["pongo_identity"] = usb_id
    state["bootm_sent_utc"] = _utc_now()
    _save_state(state_path, state)
    print("Fixed Alpine bundle uploaded from the verified session and bootm sent; no frame command was sent.")
    print(f"local log: {log_path}")


def upload(session: str) -> None:
    """Verify startup banner and upload over one uninterrupted pongoterm process."""
    _require_root()
    _require_private_log_dir()
    hashes = validate_artifacts()
    log_path, state_path = _session_paths(session)
    state = _load_state(state_path)
    if state.get("session") != session or state.get("stage") != "pongo_pending":
        raise WrapperError("session is stale, forged, or already used; upload remains gated")
    if state.get("hashes") != hashes:
        raise WrapperError("artifacts changed after Pongo startup")
    dfu_identity = state.get("dfu_identity")
    if not isinstance(dfu_identity, dict) or not dfu_identity.get("sysfs_name"):
        raise WrapperError("authorized DFU port is absent from the fresh session")
    usb_id = wait_for_pongo(dfu_identity["sysfs_name"])
    _append_event(log_path, {"time_utc": _utc_now(), "event": "pongo_live_session_start",
                             "pongo_identity": usb_id, "argv": PONGOTERM_ARGV})
    proc = None
    bootm_sent = False
    try:
        proc = subprocess.Popen(PONGOTERM_ARGV, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, bufsize=0)
        startup = read_until_banner_prompt(proc.stdout)
        clean = ANSI_RE.sub(b"", startup).decode("ascii", "replace")
        banner_ok = PONGO_BANNER in clean
        prompt_ok = "pongoOS> " in clean
        current_id = require_same_pongo(usb_id)
        _append_event(log_path, {"time_utc": _utc_now(), "event": "pongo_live_banner_result",
                                 "banner_ok": banner_ok, "prompt_ok": prompt_ok,
                                 "pongo_identity": current_id})
        if not banner_ok or not prompt_ok:
            raise WrapperError("live Pongo banner/prompt did not match; upload remains gated")

        proc.stdin.write(f"/send {BUNDLE}\n".encode("ascii"))
        proc.stdin.flush()
        uploaded_output = _read_until_prompt(proc.stdout, timeout=120)
        uploaded_text = ANSI_RE.sub(b"", uploaded_output).decode("ascii", "replace")
        uploaded = f"Uploaded {BUNDLE.stat().st_size} bytes" in uploaded_text
        current_id = require_same_pongo(usb_id)
        _append_event(log_path, {"time_utc": _utc_now(), "event": "fixed_payload_transfer_result",
                                 "uploaded_marker": uploaded, "pongo_identity": current_id})
        if not uploaded:
            raise WrapperError("fixed bundle upload was not confirmed; bootm was not sent")

        proc.stdin.write(b"bootm\n")
        proc.stdin.flush()
        bootm_sent = True
        _append_event(log_path, {"time_utc": _utc_now(), "event": "fixed_payload_bootm_sent",
                                 "pongo_identity": usb_id,
                                 "stdin_ascii": f"/send <bundle sha256 {BUNDLE_SHA256}>\\nbootm\\n",
                                 "bundle_sha256": BUNDLE_SHA256})
        try:
            tail = _read_until_prompt(proc.stdout, timeout=10)
        except (OSError, ValueError):
            tail = b""
        rendered_tail = ANSI_RE.sub(b"", tail).decode("ascii", "replace")
        bad_boot = "Bad command: bootm" in rendered_tail
        proc.stdin.close()
        try:
            returncode = proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.terminate()
            returncode = None
        _append_event(log_path, {"time_utc": _utc_now(), "event": "fixed_payload_upload_result",
                                 "returncode": returncode, "uploaded_marker": uploaded,
                                 "bootm_rejected": bad_boot,
                                 "usb_disconnect_after_upload": "USB error:" in rendered_tail})
        if bad_boot:
            raise WrapperError("Pongo rejected bootm; no frame command was sent")
    except WrapperError:
        if proc is not None and proc.poll() is None:
            proc.terminate()
        raise
    except (OSError, subprocess.TimeoutExpired) as exc:
        if proc is not None and proc.poll() is None:
            proc.terminate()
        _append_event(log_path, {"time_utc": _utc_now(), "event": "pongo_live_session_error",
                                 "error": type(exc).__name__, "bootm_sent": bootm_sent})
        raise WrapperError("Pongo live connection failed; no frame command was sent") from exc
    finally:
        if proc is not None:
            for stream in (proc.stdin, proc.stdout):
                try:
                    if stream and not stream.closed:
                        stream.close()
                except OSError:
                    pass
    state["stage"] = "bootm_sent"
    state["banner"] = PONGO_BANNER
    state["pongo_identity"] = usb_id
    state["bootm_sent_utc"] = _utc_now()
    _save_state(state_path, state)
    print("Fixed Alpine bundle uploaded and bootm sent; no post-boot service command was sent.")
    print(f"local log: {log_path}")

def main(argv: list[str]) -> int:
    try:
        if argv == ["--dry-run"]:
            dry_run()
        elif argv == ["--start"]:
            start()
        elif len(argv) == 2 and argv[0] == "--upload":
            upload(argv[1])
        elif len(argv) == 2 and argv[0] == "--resume-upload":
            resume_upload(argv[1])
        else:
            raise WrapperError("usage: dfu_boot_wrapper.py --dry-run | --start | --upload SESSION | --resume-upload SESSION")
    except WrapperError as exc:
        print(f"STOP: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
