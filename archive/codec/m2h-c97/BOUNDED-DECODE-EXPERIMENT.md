# Bounded first-IDR experiment

This file records the one-shot procedure and parameters used with the frozen
base module. The frozen-module trial has already occurred; do not repeat it.
Its outcome is in
`../evidence/a1599-decode-trial-20261014/TRIAL-RESULT.md`: first-ready in
131 ms, INIT consumed on H2M, then one valid error-class C0 (`0x48064455`,
detail `0`), with no output captured. The diagnostics derivative itself was
built offline and has not been loaded.

## Known command parameters

The source-backed values are: byte 0 `0x14`, message ID `0x81`, flags `0x4015`,
group sequence 1, selector 0, expected C0 fence 1, opaque word `0x10211000`,
command length 136 words, actual command codec IOVA from the sealed table, and
the current runtime codec page-table root. `0x4015` combines R16=1, final-group
`0x4004`, and mapping-dirty bit `0x10`. The command is limited to the
32×32 progressive 8-bit 4:2:0 fixture. Both rings must be empty. The H2M
consumer must advance through INIT as the transport prerequisite; there is no
dedicated INIT ACK requirement, and semantic INIT completion remains
unproven.

The byte and full flags word are now source-resolved for this path. The
candidate pins them and rejects changes from byte `0x14` / flags `0x4015`.

## Parameters used in the recorded trial (historical; do not repeat)

The frozen base module was loaded with these parameters:

```text
run_once=1 snapshot_once=1 init_once=1 allow_fw_abi_ring_init=1
allow_decode_submission=1 allow_output_export=1
decode_flags=0x4015 id81_byte0_bit7=0
```

```sh
insmod ./a1599_fw_candidate.ko run_once=1 snapshot_once=1 init_once=1 \
  allow_fw_abi_ring_init=1 allow_decode_submission=1 allow_output_export=1 \
  decode_flags=0x4015 id81_byte0_bit7=0
```

Use the hash-pinned firmware from the successful `590d4af6` baseline. The
`init_once=1` load parameter requests one INIT after first-ready; continue only
if logs report `INIT_TRANSPORT_CONSUMED=1` (transport through all five words,
not semantic INIT completion). Then issue one `decode_once=1` store using the
sysfs command below. The decode request has a 2-second response bound. On a valid zero-status
C0, output export waits 50 ms, copies the entire 4096-byte allocation, waits
another 50 ms, and requires an identical second backing read and intact
`0xa5` guard `[3072,4096)`. Exported bytes come from that immutable snapshot,
not a user-selected physical or DMA address. Do not submit a second command,
reset, unload, or write internal storage. All DMA allocations remain pinned
after the first possible hardware access.

Read the binary sysfs file in one bounded operation, for example:

```sh
mapfile -t controls < <(find /sys/bus/platform/devices -name decode_once -print)
test "${#controls[@]}" -eq 1
decode_ctl=${controls[0]}
printf '1\n' > "$decode_ctl"
output_file="${decode_ctl%decode_once}decode_output"
dd if="$output_file" of=/run/a1599-output-4096.bin \
  bs=4096 count=1 iflag=fullblock status=none
test "$(wc -c < /run/a1599-output-4096.bin)" -eq 4096
```

Compare it offline with the software fixture:

```sh
python3 tools/compare_output.py /run/a1599-output-4096.bin \
  ../evidence/a1599-init-experiment-20261014/fixtures/red32x32-idr.h264
```

The tool checks the guard and compares a finite candidate set (YUV420P,
YVU420P, NV12, NV21 with bounded strides and offsets) against FFmpeg's
YUV420P decode. It reports `UNMATCHED_LAYOUT` when no exact candidate exists.
A match is evidence for the captured bytes and candidate layout only.

The 36-bit DMA allocation policy, firmware, symbol-independent overlay, root
and leaf encoding, and five INIT maps match the successful baseline. Added
decode PTE reachability, coherency visibility, output plane layout and
prefetch extent are the one-trial assumptions to observe. Stop on timeout,
USB loss/re-enumeration, malformed response, any nonzero C0 status, guard
change, or unexpected ring/register state. Preserve the RAM state and pinned
buffers; do not retry.

C0 parsing validates structure and fence. Bit 27 is classified as an
error-detail response, and other nonzero statuses also return an error. A
zero-status C0 is not a decode result. No result may be reported as decoded
until the snapshot is compared; even then, the matching finite layout set and
short stable window do not independently establish future DMA quiescence.
