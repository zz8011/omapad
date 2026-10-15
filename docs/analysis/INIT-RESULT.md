# A1599 one-shot INIT experiment result

Date: 2026-10-14 UTC. Only the RAM gadget and RAM-root filesystem were used.
No decode command or internal-storage write was issued.

## Recovery and runtime preflight

- First launcher session `20261014T044021Z-ab4a6d2d315f` entered checkra1n,
  but Pongo did not enumerate cleanly. Kernel USB history later showed a brief
  `05ac:4141` with configuration descriptor errors `-71`; no bundle was sent.
- After the user's physical DFU re-entry, read-only inspection found exactly
  one `05ac:1227` on USB bus 1, device 21, port `1-12`. ACM0/1/2 were free and
  no concurrent launcher/transfer process was found.
- Second launcher session `20261014T044412Z-197d023df6ae` succeeded. Its private
  audit log records the expected Pongo banner and prompt on the same USB port,
  `uploaded_marker=true` for fixed bundle SHA-256
  `a069bf7743ac234a463e106f6eacce366fd5a89ca8d10a467e1cf93b790bb074`, and
  `bootm` sent without a rejection. Gadget `1d6b:0104` appeared at bus 1,
  device 26, port `1-12`; ttyACM0/1/2 mapped to interfaces 1.0/1.2/1.4.
- ACM0 returned a unique read-only preflight marker and reported Alpine 3.24.2,
  kernel `7.3.0-rc1`, uid 0, `modules_disabled=0`, a RAM `rootfs` `/`, empty
  `/run/incoming`, and active `file-recv`/`displayd` processes. No internal
  storage mount was present.
- ACM1 CRC-ACKed the candidate module (781440 bytes, CRC `8f7a732f`) and
  firmware (57344 bytes, CRC `1af336b2`). Source and RAM-side SHA-256 values
  matched:
  - Module: `590d4af6aae49f38aa1c8e3d324ba39a887a0b82ac35b694c82823dff806a413`
  - Firmware: `6b274c576a7952d7a4f22f65adf5b1041c4329c3dfc7ce761de53668cfd4a41e`
- Firmware was copied only into RAM `/lib/firmware/a1599/`; the module was
  copied to RAM `/run/` and loaded from that path.

## Ready and fresh ring snapshot

The single module load was:

```sh
insmod /run/a1599_fw_candidate.ko run_once=1 snapshot_once=1 allow_fw_abi_ring_init=1
```

`init_once` was omitted. It returned `INSRET=0`. First-ready matched
`0xa5a5a5a5` after 122 ms. Preflight validated context IOVAs
`0x40100000`, `0x40300000`, and `0x40340000`.

The candidate's after-ready log and a subsequent sysfs snapshot both matched:

- H→M descriptor `0x60cc01cc`, capacity 460 words, read/write indices `0/0`.
- M→H descriptor `0x60000033`, capacity 51 words, read/write indices `0/0`.
- Intervals H→M `[0x80cc,0x87fc)`, M→H `[0x8000,0x80cc)`.
- `validation=0`, `resource_bounded=1`, `comm_ram_proven=0`.

Thus the fresh state met the reviewed firmware-ABI gate and empty-index
conditions. The independent physical communication-RAM extent remains
unproven.

## One accepted INIT store and result

A first attempted shell input did not produce a completion marker. Read-only
logs and indices then showed no `INIT_TRIGGER` and producer still zero, so that
input had not reached the sysfs store. After a marker-verified ACM0 query, one
sysfs store was accepted (`SYSFS_INIT_WRITE_RC=0`). No further INIT store was
issued.

The kernel recorded:

- `RING_SNAPSHOT_INIT_TRIGGER`: exact expected descriptors and all-zero indices.
- One payload publication and kick; producer index written as 5 and read back
  as 5 (`producer_readback_match=1`).
- H→M consumer advanced from 0 to 5; `consume_poll_ret=0`,
  `consume_poll_elapsed_ms=0`, `poll_timeout_ms=2000`.
- M→H read/write indices stayed `0/0`; `m2h_indices_changed=0`.
- `RING_SNAPSHOT_AFTER_INIT_KICK`: H→M read/write `5/5`, M→H read/write
  `0/0`, `validation=0`, `resource_bounded=1`.
- The module explicitly logged `semantic_init_complete=unproven`,
  `dedicated_ack=not_required`, and `retry=0`.

After the accepted store, ACM0 returned another unique completion marker,
ring snapshot, and kernel log output. The candidate remained loaded, runtime
PM reported `active` with usage count `1`, and `file-recv`/`displayd` remained
alive. Host inspection still found the gadget on port `1-12` and no host
process owning ACM0/1/2.

No decode was submitted, so this is transport-level INIT ring consumption,
not proof of semantic initialization or a decoded frame. The device and its
PM/DMA state remain live; do not unload, reset, or retry.
