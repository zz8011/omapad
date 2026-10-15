# Five-second display acceptance record

## Latest run

- Existing RAM-boot session; no reboot or new exploit during this run.
- UTC execution window: `2026-10-11T19:17:05.973136280Z` to
  `2026-10-11T19:17:14.043709144Z`. This includes serial prompt synchronization;
  device-reported visible interval is 5 seconds.
- Before opening ACM, `fuser -v /dev/ttyACM0` showed no process using the port.
- Host reader synchronized its initial prompt and idle prompt, then sent exactly
  one `colorbars\r`; no retry was attempted.
- Device response: `colorbars: fixed 320x128 eight-bar pattern shown for 5
  seconds`; `colorbars: original region restored from volatile RAM snapshot`;
  then `a1599>`.
- User visual acceptance: “看到了” and, after the display ended, “恢复正常了”.
  This confirms the user saw the test and confirmed the image returned to normal.
- The restore message is device self-report backed by the user's visual
  confirmation. No independent byte-for-byte framebuffer comparison was made.

Private host log:
`.build/a1599/logs/a1599-display-diag-readback-20261011T191706Z-430867-fdcf7924.log`

The test painted only the fixed bottom-right rectangle `(1200,1904)`, size
`320x128`, on the tablet's existing `1536x2048` fb0. This proves the local
framebuffer can display a bounded temporary pattern and return to the prior
image in the reported test. It does not prove host-to-tablet video transport,
Wayland capture, or a working extended desktop.

User additionally confirmed after the second run: “恢复正常了”. This is a
separate visual restoration confirmation, not a memory byte comparison.

## Frame-stream RAM boot and read-only bring-up

- The user explicitly confirmed the device was black/in DFU for this new
  session. A read-only host sysfs check found exactly one DFU target at USB
  port `1-12`, bus 1/device 81. The fixed wrapper started checkra1n once and
  returned session `20261011T200741Z-cde5dc6e9134`.
- The first same-session upload attempt stopped before opening `pongoterm`
  because Pongo had not re-enumerated yet. No second exploit was run. Bounded
  kernel USB history later recorded `05ac:4141` on the same port `1-12` at
  `2026-10-11T20:08:00Z`, now bus 1/device 84.
- The same pending wrapper session then passed a fresh exact Pongo banner and
  prompt check (`pongoOS 2.6.3-cc532fac`), with the USB identity unchanged
  through upload. Its audit records the fixed bundle hash
  `27f3a55fdcdb741268d99048c3a17411777ca8d0726a0e8baa914c6a11a682a9`, the
  exact uploaded-byte marker, `bootm` sent, and subsequent Pongo USB
  disconnect. The process exit 255 followed that disconnect; the target
  subsequently enumerated as the expected Linux ACM gadget.
- Host sysfs found one `1d6b:0104` “A1599 USB serial” device at `1-12` with one
  associated `/dev/ttyACM0`. A fresh idle prompt was synchronized with one
  empty-line no-op, then only the read-only `version`, `display-status`, and
  `pagesize` commands were sent.
- Readback: Linux `7.3.0-rc1`; `AT_PAGESZ=4096`; `simpledrmdrmfb`; fb0
  `1536x2048`, 32 bpp, stride 6144, 12,582,912-byte mapping, RGB888 channel
  layout; DRM connector `card0-Unknown-1` reports connected/enabled at
  `1536x2048`. `renderD128` and the DT `chosen.framebuffer` attributes were
  unavailable in sysfs. The command stream and ACK path were responsive.
- No `frame-once`, `colorbars`, or image payload was sent in this bring-up.
  It verifies that the candidate boots and exposes readable display
  parameters; it does not verify host-to-tablet frame streaming or a complete
  second-display workflow.

## Frame-stream host sync diagnostic

- The authorized one-shot sender invocation stopped during the empty-line
  prompt sync check. It did not send `frame-once` or any frame bytes, and no
  frame metrics or ACK were produced. No retry or additional device command
  followed.
- The sender had captured the sync response but its old error path discarded
  the bytes. The protected host log directory is not readable in this
  execution environment, so the exact original RX bytes could not be
  recovered. The root cause therefore remains unconfirmed; console residue,
  echo, or line-ending variation are possibilities, not established facts.
- Offline host-side repair: compare initial-prompt framing; separate and reject
  pre-sync residue; accept only CR/LF empty-line framing (plus bounded CSI
  control sequences); reject non-whitespace text and post-prompt residue; and
  include a bounded RX hex prefix in sync errors. A dedicated readback-only
  mode sends only `version` after successful synchronization. Endpoint
  identity and frame arming/ACK/CRC validation are unchanged.
- Regression suite: 11 sender tests passed, including accepted CRCRLF/ANSI
  framing, rejection with bounded diagnostics, initial-preamble rejection,
  readback-only behavior, unique endpoint checks, and no-frame-on-sync-error.
- An initial non-sudo workspace exec could not open `/dev/ttyACM0` (`EACCES`).
  That was an execution-context mismatch: the later formal host channel
  succeeded with `sudo -n` as root, as recorded below. No permissions were
  changed.

## Authorized host-channel retry and frame arming result

- On the formal host channel, `sudo -n id -u` returned `0`; read-only `stat`
  showed `/dev/ttyACM0` as `root:uucp`, character device `a6:0`. No ownership or
  permissions were changed.
- The first privileged `--readback-only` call passed unique endpoint checking
  and empty-line prompt synchronization, sent only `version`, then failed its
  strict response framing check. The bounded captured prefix decoded to
  `version\r\nversion\r\r\nLinux version 7.3.0-rc1 ...`; build-identifying
  text is intentionally omitted here. This established the observed duplicate
  tty/application echo and CRCRLF framing.
- The one authorized `--frames 3` invocation then passed endpoint and prompt
  sync, sent one fixed `frame-once\r`, and stopped before any image payload.
  It received a second exact command echo `frame-once\r\r\n` where it expected
  `FRAME-READY seq=1`; the sender closed without retry. Captured bytes were the
  bounded literal `frame-once\r\r\n` (hex
  `6672616d652d6f6e63650d0d0a`). No frame bytes, `FRAME-READY` acknowledgement,
  framebuffer change, or frame metrics were observed by the host.
- The receiver's documented deadline is 10 seconds. After waiting beyond it,
  a fresh `sudo -n ... --readback-only` synchronized to the console, queried
  only `version`, and confirmed kernel release `7.3.0-rc1`. This confirms the
  console returned; it does not provide the receiver's lost timeout report.
- Root cause for the earlier empty-line sync rejection remains unknown because
  that old sender did not log its received bytes and no matching transcript
  existed in the protected logs. The later live sync itself succeeded.
- Host-only fix: consume at most two exact recognized command echoes before
  requiring exact `FRAME-READY seq=N`; unknown output still stops. Read-only
  version framing accepts only the observed duplicate echo variant plus the
  canonical single-echo form. Thirteen sender regressions pass, including
  duplicate echo and no-payload-on-sync-error coverage. Target bundle and
  framebuffer logic were not changed.
- The failed `frame-once` closed the target's bounded three-frame session on
  its 10-second receive timeout. Another frame sequence in this boot is not
  possible; a fresh volatile RAM boot would be needed to reset that in-memory
  session. No new DFU/boot was started.

## Offline host ↔ production C protocol E2E

- A socketpair harness connected the actual Python host sender to the
  production `target/init.c` PID 1 dispatcher and `receiver.c`. The harness
  mocked only OS syscall/device boundaries (TTY, framebuffer, clock, poll,
  mmap); no USB, iPad, network, or boot operation was involved.
- In one PID 1 instance, the fixed read-only `version` query succeeded first,
  then three sequential frames each delivered exactly 12,324 bytes and
  received their matching ACK. The receiver recorded three 250 ms holds
  (750,000,000 ns total). Full framebuffer memory and tty state matched their
  original snapshots afterward; raw mode, fd, and mapping were all released.
- Fault injection dropped the wire after 37 bytes. The production receiver
  timed out with `code=2` after about 10 seconds, sent no ACK, left the full
  framebuffer unchanged, restored tty state, and closed its resources.
- Fault injection flipped one payload bit after the frame header. The
  production receiver rejected the 12,324-byte frame with CRC `code=5`, drew
  nothing, sent no ACK, restored tty state, and closed its resources.
- All three E2E cases passed. This proves sender/receiver protocol behavior
  under the syscall harness; it does not prove physical USB timing,
  framebuffer driver behavior, visible pixels, or desktop capture.
- The physical iPad boot's earlier one-shot timeout still consumed its
  in-memory frame session. This offline harness did not reset that device
  session, and no new boot was started.
