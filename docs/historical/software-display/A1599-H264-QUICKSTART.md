# A1599 USB side display

## Normal side-display path: H.264/NUT

Use [a1599_h264_runner.py](a1599_h264_runner.py) as the normal continuous
side-display path. It captures only `A1599-RAM`, encodes H.264 on the host with
NVENC, and streams NUT over ttyACM1 to Alpine FFmpeg on the iPad. Hardware
decode is unavailable with the current target driver set; decoding is done in
software. The last historical 30 FPS session stayed realtime for over 70
seconds. Choose
15–30 FPS for normal use according to smoothness and device load; the runner
also accepts 5 FPS for a short link check. The current boot's 5 FPS write
timed out, so H.264 must be revalidated after the target shell responds.

Start only after ACM0 confirms the target shell and the three tty devices are
clear. A normal 15 FPS session:

```sh
sudo -n sh -c 'nohup python3 /home/zz8011/Documents/Codex/2026-10-12/task-3/a1599_h264_runner.py --fps 15 >/run/a1599-h264-host.log 2>&1 </dev/null &'
```

Stop the runner and request target cleanup/framebuffer restoration:

```sh
sudo -n python3 /home/zz8011/Documents/Codex/2026-10-12/task-3/a1599_h264_runner.py --stop
```

The JPEG runner is a diagnostic or fallback path only. Its `--once` mode can
confirm one `FRAME-OK` followed by a `SESSION-STOPPED` restore response after
the target shell is responsive; the 3 FPS continuous JPEG mode is not the
normal side-display recommendation:

```sh
sudo -n python3 /home/zz8011/Documents/Codex/2026-10-12/task-3/a1599_display_runner.py --once
```

Both runners share `/run/a1599-usb-display.lock` because H.264 uses ttyACM2 for
display snapshot control and JPEG uses it for frame transfer. Stop whichever
runner is active before starting the other. H.264 uses bounded local cleanup
workers and byte-counted write errors; JPEG retries ordinary link errors up to
three times and rejects USB identity mismatches without writing to a reused
tty.

The H.264 stream captures only the temporary Hyprland output
`A1599-RAM` and sends an H.264/NUT byte stream over USB ACM1. The iPad decodes
with Alpine FFmpeg and writes BGRA frames to its framebuffer. No network
listener or video file is used. ACM0 remains the maintenance shell; ACM1 file
receive is paused while it carries video; ACM2 `displayd` holds a RAM
framebuffer snapshot for restoration.

## Use

`A1599-RAM` is a portrait 1536x2048 output (logical 768x1024 at scale 2),
positioned at x=3200, y=0 to the right of DP-1. Move a window to that output
to show it on the iPad. Physical output positions and persistent Hyprland
configuration are unchanged.

The H.264 runner accepts `--fps 5` through `--fps 30`. A post-recovery check
should begin at 5 FPS and confirm bytes/target decoding before increasing the
rate. Historical tests sustained 30 FPS at realtime; power telemetry does not
establish the iPad battery trend, and target decoding is CPU-based.

First post-recovery link check only (do not leave this diagnostic rate as the
normal display setting):

```sh
sudo -n sh -c 'nohup python3 /home/zz8011/Documents/Codex/2026-10-12/task-3/a1599_h264_runner.py --fps 5 >/run/a1599-h264-host.log 2>&1 </dev/null &'
```

Stop and restore the iPad framebuffer:

```sh
sudo -n python3 /home/zz8011/Documents/Codex/2026-10-12/task-3/a1599_h264_runner.py --stop
```

The stop path terminates the target FFmpeg process (with a bounded SIGKILL
fallback if its serial read ignores TERM), resumes the ACM1 file receiver,
sends `STOP` to `displayd`, restores its saved framebuffer, and removes the
host PID file. `--stop` validates the PID against this script before signaling
it, uses a pidfd when available, and waits up to 25 seconds for cleanup. On a
capture/USB error the runner follows the same local cleanup path, closes its
USB descriptors, releases its lock, and removes its PID file. Target process
resume and framebuffer restore run in daemon workers with an 8-second shared
wait bound, so a stuck serial-driver call cannot hold the host runner open.
Remote cleanup is best effort; if the cable is gone or the serial command
times out, the log records that the target restore could not be confirmed.
Host and target logs are volatile under `/run`.

## Software and reboot boundary

The host package is Arch `wf-recorder 0.6.0-2`. The iPad runs the official
Alpine 3.24.2 aarch64 `ffmpeg 8.1.2-r0` package and dependencies in RAM. The
iPad has no enabled network repository, and no internal storage was mounted
or changed. The staged APKs were removed after installation. A reboot discards
the iPad FFmpeg install, the virtual output, and all runner state. After a RAM
reboot, fetch the official Alpine 3.24 aarch64 APK set again on the host,
transfer it over ACM, and recreate the output/stream. The host `wf-recorder`
package and this workspace script remain installed.

The stream format is portrait. No usable IIO orientation sensor is exposed
in the running iPad system, so automatic rotation is unavailable; rotation
would need a separate manual display-pipeline change.

## RAM recovery after reboot

Use the gadget-fix launcher and its pinned artifacts as the recovery entry:

- Launcher: `/home/zz8011/Documents/Codex/2026-10-12/task-2/a1599-alpine-fixed/diagnostic-v2/gadget-fix/launch/alpine_dfu_wrapper.py`
- Launcher SHA-256: `835f76c533749e1af08297c7065e1f3edf736e92092f1a5ac119ee80234d1a0a`
- Bundle: `/home/zz8011/Documents/Codex/2026-10-12/task-2/a1599-alpine-fixed/diagnostic-v2/gadget-fix/out/a1599-alpine-m1n1-linux-gadget-fix.bundle`
- Bundle SHA-256: `a069bf7743ac234a463e106f6eacce366fd5a89ca8d10a467e1cf93b790bb074`
- Rootfs SHA-256: `cbf49193f14c61acbb379a6964915f997c7ff7865ae9633853f7eba641164a4b`

The host-side launcher record `20261012T130159Z-526f1ffcfe27` pins this
bundle, confirms the Pongo prompt and upload marker, and records `bootm` sent.
Subsequent live host logs show the device on USB port `1-12` as gadget
`1d6b:0104`: the JPEG display path received `FRAME-OK` acknowledgements for
2,049 frames over 683 seconds, and the H.264 path started on `ttyACM1` at
30 FPS. This is the historical three-ACM/runtime evidence. The logs are
separate records; the stream log itself does not repeat the bundle hash. This
later evidence supersedes the earlier prepared-only status in the gadget-fix
README.

Latest recovery check (2026-10-12 UTC): launcher session
`20261012T163639Z-16a00667313b` completed the DFU-to-Pongo start and the
gadget-fix upload/`bootm`. The host then observed `1d6b:0104` on port `1-12`
with `ttyACM0`, `ttyACM1`, and `ttyACM2`; a bounded maintenance-shell query
confirmed Alpine `3.24.2`. The temporary Hyprland output `A1599-RAM` is still
present at `(3200, 0)` and the three physical output modes/positions were
unchanged. This boot's target `/sys/class/power_supply` had no entries; CPU
policy0 reported `schedutil`, 840000 kHz current with 300000–1296000 kHz
limits, and the backlight reported 1638/2047 with `bl_power=0`. A subsequent
bounded 5 FPS check failed on the first serial write timeout before any frame
or byte progress was logged. The host runner exited and its PID file was
cleared; the device stayed enumerated with all three ACM nodes. The target
file-recv resume and displayd `STOP` acknowledgement were not confirmed, so
target-side cleanup and framebuffer restoration remain unknown.

Latest follow-up (2026-10-12 17:04 UTC): host read-only inspection still showed
USB `1-12` as `1d6b:0104` with ACM0/1/2 on the expected interfaces. The host
had no process holding any of the three tty nodes. Kernel log evidence showed
the gadget enumerated at 00:37 local and had no later disconnect/reset entries
in the captured tail. A single read-only ACM0 shell probe did not return its
completion marker; its local probe process exited, so the target shell and
receiver process state remain unknown. No further USB commands were sent.
Preserved host and encoder logs plus the kernel snapshot are in
`evidence/a1599-2026-10-12/`. The historically validated JPEG diagnostic runner
and H.264 runner share a host lock to prevent concurrent ttyACM2 writers. The
H.264 write error also reports bytes accepted before timeout. No fresh
frame/ACK or STOP restore was confirmed during this follow-up.

Later serial follow-up: the launch record still points to the selected
`a069bf…` gadget-fix bundle. Its local `inittab` expects `/bin/sh` on ACM0,
`file-recv` on ACM1, and `displayd` on ACM2; live service processes could not
be queried. ACM0 had raw 115200 settings with DTR/RTS asserted, but a CR and a
single read-only `echo` produced no returned bytes. One interface-only
`cdc_acm` unbind/rebind of ACM0 did not restore the response. A second
interface-only rebind of ACM2 followed by its documented non-mutating `HELLO`
also returned no response; direct nonblocking read found no queued bytes.
Both tty mappings remain correct, no host reader/writer was present, and no
whole-device USB reset or ACM1 stream retry was done. The visible local test
pattern/terminal is not evidence that desktop frames are arriving. At this
point no software reboot command is reachable over the control tty; a physical
restart is required before re-entering the approved RAM boot path.

When the device is already in DFU and a RAM recovery is appropriate, use this
launcher only:

```sh
sudo -n python3 /home/zz8011/Documents/Codex/2026-10-12/task-2/a1599-alpine-fixed/diagnostic-v2/gadget-fix/launch/alpine_dfu_wrapper.py --start
sudo -n python3 /home/zz8011/Documents/Codex/2026-10-12/task-2/a1599-alpine-fixed/diagnostic-v2/gadget-fix/launch/alpine_dfu_wrapper.py --upload SESSION_ID
```

Use the session ID returned by `--start`. Do not use the earlier fixed-v1
launcher or bundle (`4f105fa9924fa838ea945a56495fae8f6f7f74d3996f26932b7f67109dcca47b`);
it is not the selected three-ACM recovery artifact. The gadget-fix launcher
and bundle are separate from the older `launch/README.md` notes.

Do not start RAM recovery or a stream solely because DFU is visible. The last
user-reported pre-DFU battery level was 9%, and host power telemetry does not
establish that the iPad battery is charging. The host sysfs node
`/sys/class/power_supply/apple_mfi_fastcharge_1-44` is created by the host
USB driver `apple-mfi-fastcharge` for the attached Apple USB device. Its
`type=Battery` label is the host driver's synthetic, device-scoped power-supply
object, not telemetry from the iPad's own battery gauge. The driver matches
Apple product IDs `0x1200`–`0x12ff`, so it also attaches to DFU product `0x1227`.
Its `charge_type=Trickle` value is a cached default initialized at probe; a
read only returns that field. It is not a battery query or measured current.
The device's DFU power draw and battery trend are unknown. Do not change that
property or describe DFU as verified low-power.
The host's UCSI `current_now` is system-side telemetry, not an iPad current
measurement. Resume only after device power is independently known to be
adequate. The Linux driver implementation is documented in
[apple-mfi-fastcharge.c](https://raw.githubusercontent.com/torvalds/linux/master/drivers/usb/misc/apple-mfi-fastcharge.c).

## Measurement notes

The H.264/NUT path has no per-frame ACK. The host log reports serial write
wait and bitrate; the target FFmpeg log reports frames, FPS, media time,
elapsed time, and `speed`. Its input queue is configured to one packet with
low-delay/no-buffer flags and no B-frames. These are throughput and drift
proxies; end-to-end optical latency has not been measured.

The bounded rate trial used 5-second CPU samples and 6-second NVIDIA encoder
samples. At 15/20/24/30 FPS, the target produced approximately 15/20/23/30
FPS at realtime speed. Target FFmpeg CPU was about 79/93/106/128 percent of one
core; host capture/encoder process CPU was about 10/16/20/14 percent of one
core; NVIDIA encoder utilization was about 4-10/9-10/4-15/14-18 percent. The
30 FPS run remained realtime for over 70 seconds, with 267 MiB target FFmpeg
peak RSS and about 2.95 Mbit/s host stream rate. The H.264 stream has no
per-frame ACK; at 30 FPS the host reported about 1.83 seconds cumulative USB
write wait during 70 seconds, while target `speed` stayed at 1x.

## Live recovery and active JPEG fallback (2026-10-12 UTC)

The user re-entered DFU. The fixed gadget-fix launcher started from Apple
`05ac:1227` on USB `1-12`, verified `pongoOS 2.6.3-cc532fac` on that same
port, uploaded bundle SHA-256
`a069bf7743ac234a463e106f6eacce366fd5a89ca8d10a467e1cf93b790bb074`, and
sent `bootm`. Session: `20261012T183320Z-f1954fcbb088`. The resulting device
is `1d6b:0104 A1599 Local USB Services` at Bus 1/Port 12, with ttyACM0/1/2
mapped to USB interfaces 1.0/1.2/1.4. A bounded ACM0 shell query confirmed
Alpine 3.24.2, PID 1 `/bin/busybox`, `file-recv` PID 105, and `displayd` PID
106. No internal block device mount was found.

Hyprland still has `A1599-RAM` at 1536x2048, scale 2 (logical 768x1024),
position `(3200, 0)`, directly right of DP-1. The physical outputs remained
eDP-1 2560x1600 at `(0,0)`, DP-1 3840x2160 at `(1280,0)`, and HDMI-A-1
3840x2160 at `(1280,1080)`, all scale 2. The non-private `btop` window is on
`A1599-RAM`; the focused ChatGPT window remained on HDMI-A-1.

A one-frame JPEG check captured only `A1599-RAM`: 573,815 bytes, matching
`FRAME-OK seq=1` CRC, 141 ms payload-to-ACK, followed by
`SESSION-STOPPED` and confirmed framebuffer restore. A continuous JPEG runner
is now active as PID `2124206`, tracked in `/run/a1599-display.pid`, at 3 FPS.
Through frame 144 it reported 2.79 average FPS; sampled payload-to-ACK values
were 121.66–142.54 ms. The current log is `/run/a1599-display-current.log`
on the host's volatile `/run` filesystem. Stop and restore with:

```sh
sudo -n python3 /home/zz8011/Documents/Codex/2026-10-12/task-3/a1599_display_runner.py --stop
```

This boot's target RAM image has no `ffmpeg` executable (`command -v ffmpeg`
returned missing), so H.264/NUT could not be tested. The host NVENC encoder did
pass a synthetic 1536x2048 in-memory frame check. No software was installed on
the iPad; the active path is the built-in JPEG `displayd`. The iFlytek AiNote
X3 enumerated separately behind USB Bus 1/Port 9 and was not accessed.

## Successful H.264 RAM restore (2026-10-12 18:51 UTC)

The user authorized installing the required decoder in the temporary Alpine
system. Official Alpine v3.24 aarch64 indexes identified FFmpeg `8.1.2-r0` and
its 104 dependencies. The host downloaded 105 APKs from the official Alpine
CDN (46,099,698 compressed bytes); the iPad's own `apk` ran offline with its
normal signature checks and reported `OK: 112.8 MiB in 122 packages`. The
`--force-non-repository` option only permits local APK files; no untrusted
package bypass was enabled. The APKs were then deleted from iPad `/run`, whose
128 MiB tmpfs returned to 16 KiB used. The iPad had about 667 MiB
`MemAvailable` after install. FFmpeg reports version `8.1.2`, includes the
native H.264 decoder and NUT demuxer, and remains in the volatile root only.
No network was enabled on the iPad and no disk was mounted.

The actual output→USB→panel probe ran at 5 FPS for about 46 seconds. The host
sent 15.9 MB at about 2.78 Mbit/s; the target decoded 134 frames in the first
28 seconds at about 4.8 FPS and `speed` near 0.99x. The host's H.264 stream has
no per-frame ACK, so these are encoder/USB byte and target decoder statistics,
not ACK counts. Graceful stop returned `SESSION-STOPPED`, confirmed the saved
framebuffer was restored, and resumed the paused `file-recv` process.

The active detached runner is PID `2217038`, tracked in
`/run/a1599-h264-host.pid`, using `wf-recorder` PID `2217087`, NVENC, NUT and
ACM1 at 15 FPS. At 13 seconds the target had decoded 180 frames (about 13.8
FPS) at `speed=1x`. After 103 seconds the target had decoded 1,530 frames at
about 15 FPS and `speed=1x`; the host had sent 33.35 MB in 96 seconds (about
2.77 Mbit/s average; 2.04 seconds cumulative USB write wait). Its live host
log is `/run/a1599-h264-host.log`; the target log is
`/run/a1599-h264-target.log`. Current monitor/client inspection shows:

- `A1599-RAM`: 1536x2048, scale 2, at `(3200, 0)`; `btop` is on this output.
- `eDP-1`: 2560x1600 at `(0, 0)`; `DP-1`: 3840x2160 at `(1280, 0)`;
  `HDMI-A-1`: 3840x2160 at `(1280, 1080)`. These modes and positions match
  the pre-stream snapshot.
- The current active window is Chrome/X on `eDP-1`. No focus or window
  movement was performed by the stream runner.

Stop the stream and restore the iPad framebuffer with:

```sh
sudo -n python3 /home/zz8011/Documents/Codex/2026-10-12/task-3/a1599_h264_runner.py --stop
```

The durable host cache is in `vendor/alpine-v3.24-aarch64/` with a SHA-256
manifest. After a future iPad RAM boot, with the three-ACM gadget and its
`file-recv` service ready, `restore_a1599_ffmpeg_ram.sh` CRC-transfers the
cached APKs over ACM1, installs them offline through Alpine `apk`, verifies
FFmpeg, then removes the staged APKs from `/run`. Start the detached 15 FPS
runner using the command printed by that script. All installed iPad packages,
processes and logs disappear when its RAM system reboots; the host APK cache
and this quickstart remain available.

## Health sample and recovery-script review (2026-10-12 18:57 UTC)

Without changing the active 15 FPS stream, a 5-second sample measured target
FFmpeg PID 195 at 73.6% of one CPU core, 166.7 MiB RSS and 214.8 MiB peak
RSS; target `MemAvailable` was 541.8 MiB. Decoded frame count increased from
4,383 to 4,459 (15.20 FPS), with zero `error`/`failed`/`invalid` matches in
the target FFmpeg log. The host had 11.6 GiB available; runner PID 2217038
used 0.4% CPU / 27.3 MiB RSS and `wf-recorder` PID 2217087 used 0.0% CPU /
9.5 MiB RSS in the same sample. At 348 seconds, the host log still showed
the runner alive, 119.7 MB sent, 2.75 Mbit/s average and 7.66 seconds total
USB write wait.

Recovery-script review: shell syntax and Python AST parsing pass. All 105
cached APKs match their manifest SHA-256 and size. Before upload, the helper
removes only those exact manifest APK names from `/run/incoming`, so an
interrupted transfer can be retried without colliding with stale files or
deleting unrelated incoming files. The uploader checks each file's SHA-256
against the manifest before sending and also verifies the receiver's CRC ACK.
Installation still goes through target `apk` with networking disabled and
without `--allow-untrusted`. This was a source/manifest review; a second full
restore was not run against the active session.

The current stream's decoder is FFmpeg's native software H.264 decoder;
hardware decoding is not complete. 30 FPS was not re-tested in this run.
H.264/NUT has no per-frame ACK, and no optical end-to-end latency measurement
was made. The earlier JPEG ACK measurements do not describe H.264 latency.

## Resource-probe compatibility gate and USB disconnect (2026-10-12 19:37 UTC)

Read-only review of `task/resource_probe/a1599_resource_probe.c` found only
platform-resource lookup and OF metadata parsing (`platform_get_resource`,
`of_irq_count`, and phandle counts). It does not map or access registers,
request IRQs, enable clocks/resets/power, or use DMA. The module is AArch64 ELF
relocatable, SHA-256
`90631f0a0938390cc7294306c391cd509c91c618e110bcf1dfd679f0bdea116f`, with
`vermagic=7.3.0-rc1 SMP preempt aarch64` matching the live release/architecture;
`depends` is empty. It has no signer/signature metadata. The experimental OF
alias is `apple,s5l8960x-vxd-resource-probe`.

The live target reported Linux `7.3.0-rc1`, AArch64, SMP/PREEMPT. Module
loading is not globally disabled (`modules_disabled=0`) and `/sys/module`
exists, but `/lib/modules/7.3.0-rc1` and a readable kernel config are absent,
so `CONFIG_MODULES`, `CONFIG_OF_DYNAMIC`, and signature-enforcement settings
could not be confirmed. `/sys/firmware/devicetree/base` exported 126 nodes and
101 `compatible` properties; the bounded search found no `vxd`, `vxe`, `codec`,
`decoder`, or `media` node/compatible, and the probe's exact experimental
compatible has no known match. The configfs DT-overlay path was absent. No
module was copied or loaded: insertion without a matching platform node would
not call the probe and would not validate resource enumeration.

At 19:37:06 UTC the host kernel logged `usb 1-12: USB disconnect`; ttyACM0/1/2
and USB path 1-12 disappeared. The user then reported the iPad battery was
empty. The stream runner exited with ACM1 `EIO`; its cleanup could not reach
ACM0/2, so target process/framebuffer cleanup is unconfirmed. No USB reset,
restart, or stop command was issued. The host runner is no longer active; the
user is charging the iPad. Since this is a RAM boot, the FFmpeg install may be
gone if the iPad powered off; restore via the saved host cache after it is
booted again. The minimal probe next step is to inspect the re-exported live
DeviceTree and kernel config after reconnection, confirm a real matching node
and module policy, then decide whether a RAM-only load can actually bind.
