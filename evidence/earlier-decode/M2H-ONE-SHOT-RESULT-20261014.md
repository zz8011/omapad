# A1599 one-shot M2H capture trial result

Date: 2026-10-14 UTC. Device was operated only through the approved RAM boot
and the one authorized candidate trial. No internal storage was accessed or
written.

## Result

The candidate reached first-ready, completed the INIT transport-consumption
prerequisite, submitted exactly one ID81 decode request, received a valid C0
response with a nonzero error-class status, and successfully captured exactly
32 bytes once from `m2h_ring_raw_once`.

The C0 status was `0x48064455`, detail `0`. The M2H consumer stayed at 0;
producer stayed at 8 across the raw capture. The 32-byte capture SHA-256 is
`d8ea871aca1430c566df5338ba7218a3403712bb02d03bbf58372aaefdfecf9b` and the
host evidence copy is `M2H-RAW-32BYTE-20261014.bin`.

**Hardware decode is not established.** The valid response is nonzero/error
class. The output guard was not checked because the decode result was an error;
no output image was captured or compared.

## DFU and RAM boot evidence

- User reported re-entering DFU. Initial five-minute read-only wait found no
  target; after replug, the filtered `lsusb -d 05ac:1227` checks first saw it at
  2026-10-14 11:02:39 UTC, Bus 001 Device 043. At 11:02:49 its target-only
  sysfs identity was `05ac:1227`, Apple Mobile Device (DFU Mode), sysfs
  `1-12`, speed 480 Mbps, CPID 8960, board ID 32.
- Fixed launcher `alpine_dfu_wrapper.py --start` returned session
  `20261014T110352Z-710547bd9344`. The host audit recorded DFU on bus 1/device
  43/sysfs `1-12`, and checkra1n/Pongo hashes. `--upload` verified the live
  Pongo banner and prompt on bus 1/device 46, same port `1-12`, sent the
  hash-pinned bundle `a069bf7743ac234a463e106f6eacce366fd5a89ca8d10a467e1cf93b790bb074`
  and `bootm`. The subsequent USB disconnect followed the upload; the
  launcher returned success and reported no post-boot service command.
- The RAM Linux gadget appeared at 2026-10-14 11:04:45 UTC on the same port:
  `1d6b:0104 A1599 Local USB Services`, bus 1/device 48, 480 Mbps. ACM0/1/2
  mapped to `1-12:1.0`, `1-12:1.2`, and `1-12:1.4`; no host process owned the
  three nodes.
- Read-only ACM0 preflight reported uid 0, kernel `7.3.0-rc1`, root filesystem
  `rootfs`, `modules_disabled=0`, empty `/run/incoming`, no `a1599` module, no
  VXD node, no ext4/f2fs/vfat mount, and live `file-recv`/`displayd` services.
  The first shell query lacked the helper's required completion marker and
  timed out after the target command returned to its prompt; it made no device
  change. The repeated query with a marker succeeded.

## File transfer and module load

The frozen module SHA-256 was rechecked as
`c97fb883bbe39269bb9f16a9074305ce870e3366f563c72bf4b5ba77d6745fb2` (866224
bytes). The firmware SHA-256 was
`6b274c576a7952d7a4f22f65adf5b1041c4329c3dfc7ce761de53668cfd4a41e` (57344
bytes). One ACM1 CRC-ACK transfer returned:

```text
ACK FILE-OK name=a1599_fw_candidate.ko bytes=866224 crc=df0e4d45 sha256=c97fb883bbe39269bb9f16a9074305ce870e3366f563c72bf4b5ba77d6745fb2
ACK FILE-OK name=vxd-staging-12.5.8-16H88.bin bytes=57344 crc=1af336b2 sha256=6b274c576a7952d7a4f22f65adf5b1041c4329c3dfc7ce761de53668cfd4a41e
RAM-TRANSFER-COMPLETE
```

The firmware was copied from `/run/incoming` to `/lib/firmware/a1599` on the
RAM rootfs. Target-side SHA-256 values for the module, staged firmware, and
firmware at the load path all matched the expected hashes.

Exactly one load command was issued:

```sh
insmod /run/incoming/a1599_fw_candidate.ko run_once=1 snapshot_once=1 init_once=1 allow_fw_abi_ring_init=1 allow_decode_submission=1 allow_output_export=1 decode_flags=0x4015 id81_byte0_bit7=0
```

Return: `INSRET=0`. Logs reported VDEC genpd attach and runtime-PM resume held
(`active=1`, `usage=1`), firmware upload, first-ready signature
`0xa5a5a5a5` in 131 ms, valid ring descriptors/bounds, and
`INIT_TRANSPORT_CONSUMED=1` (semantic INIT completion remains unproven).
Before ID81 the ring was H2M 5/5 and M2H 0/0.

## One decode and one raw capture

The initial search through `/sys/bus/platform/devices` found zero controls
because it did not follow the sysfs device symlink. Its explicit gate failed
closed; no decode write occurred. A read-only check of the exact bound device
path `/sys/devices/platform/soc/207f00000.vxd-first-ready` then showed one
`decode_once` control and one 32-byte `m2h_ring_raw_once` attribute.

Exactly one sysfs decode store was then issued:

```sh
printf '1\n' > /sys/devices/platform/soc/207f00000.vxd-first-ready/decode_once
```

Store return: `1` (the driver returns failure for the nonzero C0). Kernel logs
showed `published=1`, `h2m_consumed=1`, `response_received=1`, valid C0/fence 1,
`c0_status=0x48064455`, `c0_detail=0`, `retry=0`,
`response_ring_consumer_advanced=0`, and `decode_success=unclaimed`. Raw 16-byte
C0 record:

```text
10 c0 01 00 00 00 00 00 55 44 06 48 00 00 00 00
```

After the response, the ring snapshot showed exact descriptors
H2M `0x60cc01cc` and M2H `0x60000033`, H2M indices 10/10, M2H indices 0/8,
validation 0, and M2H bounds `[0x8000,0x80cc)`.

Exactly one 32-byte read was made with `dd` (`bs=32 count=1`) from the fixed
attribute into `/run/a1599-m2h-ring-raw-once.bin`. The command returned 0,
`1+0 records in`, `1+0 records out`, and 32 bytes. Callback log:

```text
M2H_RAW_ONCE captured_bytes=32 words=8 bounds=[0x8000,0x8020) producer_before=8 producer_after=8 consumer_before=0 consumer_after=0 host_driver_serialized=1 firmware_concurrency_excluded=0 consumer_advanced=0 retry=0
```

Raw 32 bytes:

```text
10 c0 01 00 00 00 00 00 55 44 06 48 00 00 00 00 90 c7 01 00 00 00 00 00 00 00 00 00 df 7b ef bd
```

The callback excludes concurrent CPU ring access through this driver, but not
asynchronous firmware writes; the bytes are a diagnostic snapshot, not proof
of firmware quiescence. No second capture, decode, unload, reset, or internal
storage write was performed. DMA allocations remain pinned and runtime PM is
held active; preserve the current RAM state.
