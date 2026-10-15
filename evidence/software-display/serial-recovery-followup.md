# A1599 serial recovery follow-up

All actions here were limited to the existing USB gadget on host port `1-12`.
No DFU, reflash, whole-device USB reset, or restart of unrelated USB devices
was performed.

## Image and expected services

- The saved wrapper record for session `20261012T163639Z-16a00667313b` records
  bundle SHA-256
  `a069bf7743ac234a463e106f6eacce366fd5a89ca8d10a467e1cf93b790bb074`, upload
  marker, `bootm`, and USB disconnect after the Pongo transfer.
- The local bundle at that path hashes to the same value. The selected RAM
  rootfs's `/etc/inittab` configures `/dev/ttyGS0` with `/bin/sh`, `/dev/ttyGS1`
  with `file-recv`, and `/dev/ttyGS2` with `displayd`.
- The live host descriptor is `A1599 Local USB Services`, VID:PID `1d6b:0104`,
  speed 480 Mbps. `ttyACM0/1/2` resolve to interfaces `1-12:1.0`, `1-12:1.2`,
  and `1-12:1.4`, respectively. This confirms the expected gadget topology,
  not that each target userspace process is currently alive.

## Host-side checks and reconnect attempts

- No A1599 runner PID file or matching H.264/JPEG process was present. `fuser`
  reported no process holding any of the three tty nodes. There was no stale
  host reader/writer conflict.
- Before reopening ACM0, its saved settings were raw with 115200 baud,
  `VMIN=0/VTIME=0`; after open, DTR and RTS were asserted. A CR wakeup and a
  single harmless `echo A1599_PROBE_OK` produced zero receive bytes.
- ACM0 was unbound and rebound once, only at `1-12:1.0`. Its tty node returned
  as `/dev/ttyACM0`. A second single-line echo still produced zero receive
  bytes; the local process then blocked while closing the written tty and was
  terminated by its 10-second guard.
- Source `displayd.c` defines `HELLO\n` as a non-mutating readiness query and
  responds `DISPLAYD 1 READY`. ACM2 was unbound and rebound once, only at
  `1-12:1.4`. `HELLO\n` was written, but no response arrived. The selector-based
  read did not return within its guard. A later direct `O_NONBLOCK` read
  returned `EAGAIN`, confirming that no response bytes were queued.
- The USB device itself remained enumerated. Kernel messages showed the
  expected ACM0/ACM2 interface rebinds, but no whole-device disconnect, reset,
  or later USB error. ACM1 was not rebound or written during this follow-up.

The user's visible iPad screen was described as a green stripe, RGBW bars, and
a local terminal with a blinking cursor. That confirms the panel still shows
local output, not that a desktop frame is arriving. The H.264 5 FPS run had
already timed out on its first ACM1 stream write, and its target-side cleanup
was unconfirmed.

## Finding and limit

Port mapping, host process conflict, and DTR/RTS state do not explain the
failure. The host can enumerate the gadget and write small ACM0/ACM2 payloads,
but neither service returns data. The strongest current explanation is a
non-responsive target-side CDC/userspace data path after the H.264 failure;
without ACM0 output, the target process table cannot be read, so a kernel-level
USB gadget stall cannot be distinguished from dead/stuck getty and displayd
processes. No software reboot command is reachable over the existing control
channel.

The H.264 and JPEG host runners were updated offline to avoid `select()` on CDC
tty nodes, use bounded nonblocking read/write polling, skip unnecessary
termios reprogramming when the tty is already raw at 115200, and bound local
restore/close work. No stream was started after these changes.
