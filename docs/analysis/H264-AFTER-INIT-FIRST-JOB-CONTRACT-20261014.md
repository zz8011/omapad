# A1599 H.264 after-INIT first-job contract (static analysis)

Date: 2026-10-14. This document records the exact static-analysis report supplied by the user. No device operation was performed for this analysis.

## Findings

The live experiment showed INIT transport consumption (producer 0→5; consumer 0→5), with no dedicated ACK and no first frame. Existing PM/DMA state remains pinned. The next layer is not raw H.264 in the ring: it is a 20-byte ID `0x81` job envelope pointing to a separately mapped codec command stream.

A host packer and C0 parser can be independently tested. There is not yet a complete minimum I-frame command stream that is safe to submit: output/bitstream addresses and mappings, parts of Apple’s 36-byte control header, VLC table mapping, and resource dimensions remain unresolved. Do not send an empty ID81, a completion-only command, or a public Intel command stream as a substitute.

## Sample and provenance

AppleVXD393 3.28.7, iOS 12.5.7 / 16H81, kernelcache SHA-256:

`0e0b42b00f69d87bcd2d1b62baf769c040513c4cced275275350f9b58c1cf3a1`

All VAs below are unslid; file offset is `VA - 0xfffffff007004000`. 12.5.8 correspondence must follow locally verified functions/data; do not infer from version numbers alone. Original source, firmware, and disassembly are not distributed here.

## 1. Role of INIT-following register bit 14

`Start` at `0xfffffff008681040` (file `0x167d040`) writes `0x610=0x4000`, then sets context `+104=1`.

The receiver at `0xfffffff008681518` reads `0x608` at `0x8681554`, tests bit 14 at `0x868155c`, and if set writes `0x4000` to `0x60c` at `0x8681570`, then processes the M→H ring. Reset writes zero to `0x610` after `0x8681234`, and separately clears state through `0x60c`.

This exact-sample comparison supports response/MTX-event interrupt enable, status, and clear semantics. It is not a ring message or another decode-start command. This does not by itself establish Linux IRQ-controller behavior, trigger type, or the complete mask/ack contract; do not enable an IRQ without a handler for polling experiments.

## 2. Ordinary core-0, mode-1 ID81 envelope

Host submission function: `0xfffffff008679f44`; send call: `0xfffffff00867a6b0` (file `0x16766b0`). The core-1 call at `0x867a488` is not the normal single-core path for this board.

`R` below is a host submission record, not the ring message itself. Mode 1 uses 20 bytes; some paths use 28 bytes only for mode >= 2.

| Envelope offset | Value / source |
|---:|---|
| +0 u8 | Low 7 bits are 20; `0x867a510–0x867a51c` preserves the source message’s bit 7. Do not assume it is always zero. The sender uses only low 7 bits for length. |
| +1 u8 | `0x81`, stores at `0x867a584/0x867a588` |
| +2 u16 | Low 16 bits of provider `+9756` group sequence, stores at `0x867a660/0x867a664` |
| +4 u16 | `R+16` flags OR terminal/intermediate flags OR dirty-derived bit 4, `0x867a618–0x867a648` |
| +6 u16 | `R+24` byte length shifted right 2, `0x867a594–0x867a59c` |
| +8 u32 | `R+28` codec command-stream address, `0x867a58c/0x867a590` |
| +12 u32 | Initially low 32 bits of `(root_device_address >> 8)`, `0x867a668–0x867a674`, then byte `+12` is overwritten with low 8 bits of submission argument `w3`, `0x867a68c/0x867a690` |
| +16 u32 | `R+20`, `0x867a5a0/0x867a5a4`; for the H.264 producer its source is `C+1432`. Full firmware semantics are not named; do not set it to zero by assumption. |

Root/context composition is `((root >> 8) & 0xffffff00) | (selector & 0xff)`. This is consistent with the known 64 KiB root alignment. The low byte is not part of the root address. Selector comes from a call argument; the complete legal selector set is not established, so do not call it a fixed core-0 value.

The group counter increments at `0x867a0b8–0x867a0c4` before the record loop. When `w26==0`, `0x4004` is added; otherwise `0x2000` is added. Mode-1 dirty nonzero adds `0x10`. Do not use `0x4004` alone as the whole flags field or label bit 4 as a confirmed host TLB flush.

ID `0x86` is another dispatcher branch (`R+84==3`, `0x867a2c4–0x867a2cc`), not a fixed pre-ID81 message. This H.264 producer writes 1 to payload `+72` (= `R+84`) and therefore selects ID81.

## 3. Command address and length provenance

The H.264 producer at `0xfffffff008697630–0xfffffff008697640` calls record allocator `0xfffffff008682c54` with record type `0x80000007` and payload length `0x70`. The helper at `0x8682c70` writes type and length, `0x8682c7c` writes magic `0xabcdefab`, and `0x8682c84` returns `R+12`.

Let producer return pointer `P=R+12`:

- `P+8` (`R+20`) receives `C+1432` at `0x8697658–0x869765c`.
- `P+12` (`R+24`) receives `current_command_cursor - command_CPU_start - already_submitted_bytes` at `0x8697660–0x8697674`.
- `P+16` (`R+28`) receives `command_buffer_codec_base + already_submitted_bytes` at `0x8697690–0x8697698`.
- `P+72` (`R+84`) receives 1 at `0x8697650–0x8697654`.

Thus ring `+8` references a codec command-buffer subrange and ring `+6` counts its words. They are not the H.264 NAL byte address/length. The allocator’s check of host record capacity does not validate the command range, its mapping, 32-bit address representability, 4-byte alignment, or 16-bit word count. A new packer should reject non-word lengths, word count over 65535, address overflow, and unmapped subranges rather than copy truncation.

## 4. Proven shape of the bitstream descriptor inside the command stream

The producer at `0xfffffff0086974f8–0xfffffff00869756c` takes bit offset `b`, computes `q=b>>5` and `residual=b&31`, advances bitstream address by `4*q`, and reduces byte length by `4*q`. A later fixture-specific source trace closes `b=32` for this IDR (8 NAL-header bits plus 24 slice-header bits), so its `q=1`, residual 0, and decoded payload extent is 17 bytes. See the addendum below.

It writes three setup words: `0xb0000001`, `residual`, and adjusted length. It calls helper `0xfffffff008682d7c`, which writes two words: `0xa0000000 | adjusted_length` and adjusted codec address. The helper checks the high four length bits, so the interface length limit is the low 28 bits. New code must reject signed/underflow/range issues rather than log and continue.

Two producer address branches exist: `0x869750c–0x8697520` builds from a resource mapping record `+16` and an offset; `0x8697524–0x8697534` uses a CPU/resource delta and resource `+24` base. The latter is now closed for the fixture under the explicitly known `parent+128==0` path; the alternate branch provenance remains open. The arithmetic for `q`, residual, and byte extent is host-tested, but arbitrary DMA addresses must not be substituted.

These two DMA-descriptor words are only one part of a complete command stream. Before them are a 36-byte 0x9-class control header, a 1088-byte VLC transfer, and table registers. After them are H.264 FE/RENDEC registers and a `0x60000000` completion marker (`0x8697590–0x8697598`).

## 5. Output/context boundary

Established: the 36-byte header plus 8 bytes at its tail comes from `PreloadBuffer` codec/GART address `C+1512`, not a CPU pointer. Header `+32` is active bit-depth/chroma-class metadata, not padding.

Unresolved: header `+12` is a packed scalar pair, not a public 32-byte MB-parameter structure address; its units and all input ranges are not fully closed. The primary reconstruction/output surface’s plane layout, strides, and mapping coverage lack a fully verified minimal I-frame instance.

Observed alternate-output fields header `+20/+24` at `0x8696d40–0x8696d5c` are built from object `C+1416`’s `+24` base plus offsets `+60/+64`. This does not establish they are the primary output in the minimal path and does not prove they may be zeroed or omitted.

A minimal test vector should be small, progressive, 8-bit 4:2:0, one I/IDR slice, without FMO/rotation/scaling. These are constraints for further source tracing, not an execution contract. The narrow next static target is the output-resource → stride/scalar → plane-address chain, not a generic full driver.

## 6. Response parsing and bounded success criteria

Receiver `0xfffffff008681518` uses byte 0 low-7-bit length to consume `ceil(length/4)` words; ring indices remain word units. Dispatch table at `0xfffffff00868180c` maps ID `0xc0` to `0x86816b4` and ID `0xc7` to `0x86816e8`.

For C0:

- `message+2` is a u16 fence read at `0x86816b8` (receive storage base also has a 16-byte host prefix).
- `message+4` is a u16 read at `0x86816b4`.
- `message+8` is a u32 status read at `0x86816bc`.
- If status bit 27 is clear, host registers `0x10`; if set, it registers `0x8010`.
- An exceptional branch additionally reads u32 at `message+12`. A safe parser must check each branch’s required bytes rather than read based only on the ID.

C7 enters diagnostic/exception handling; this report does not rename all its fields. No M→H response does not prove first-frame success; H→M consumption only shows the command was taken. The original host aggregates the last fence across multiple C0 records. Existing lifetime analysis does not prove that each C0 means all DMA has ended. A single-frame design should use a unique outstanding fence, check status, validate output, and keep all mappings pinned after unknown error/timeout until quiescence is established.

## Host-testable versus not submit-ready

Host-testable independently: ID81’s 20-byte envelope packing; command length/address arithmetic; root high bits plus low selector byte; bitstream `q`/residual arithmetic; and C0 short-packet, wrong-fence, and bit-27 error parsing. Synthetic inputs test arithmetic only and are not a device command stream.

Not submit-ready: ID81 plus bare NAL bytes, a zeroed 36-byte header, Intel’s 32-byte header, or a completion-only sequence. The PreloadBuffer request/source is now traced separately (device type 4, 648-byte request, `C+1512` codec IOVA, at least 4096-byte backing for 4-KiB pages), but actual output format, private main-emitter words, exact full stream length, and completion semantics remain open. The fixture-specific VLC table resource and bitstream child extent are recorded in the later offline evidence; do not treat a synthetic host IOVA as validated merely because INIT was consumed.

**Hard boundary:** the incomplete contract is inside the complete codec command stream’s resource parameters, not the ring envelope and not a missing INIT ACK. No device operation was authorized or performed by this static report.


## Later output-resource evidence (2026-10-14)

The direct-primary IOSurface plane-address and stride chain, remaining format/plane-count limits, and H+12/H+4 fields are separately captured in [A1599-32X32-PRIMARY-SURFACE-AND-CONTROL-HEADER-20261014.md](A1599-32X32-PRIMARY-SURFACE-AND-CONTROL-HEADER-20261014.md) and [A1599-IOSURFACE-PLANE-GETTER-IDENTITY-20261014.md](A1599-IOSURFACE-PLANE-GETTER-IDENTITY-20261014.md). The generated 57-byte fixture’s complete SPS/PPS/slice-header parse manifest is [here](fixtures/red32x32-idr-PARSE-MANIFEST.md). These additions do not close a submit-ready full command stream or prove an output surface format.

## Fixture-specific VLC and escaped-NAL resource closure (2026-10-14)

The static initialization path maps 4096 bytes into mapping record `C+936`; record `+8/+16` yield CPU pointer `C+944` and codec IOVA `C+952`. The code copies the exact 1088-byte `VlcPackedTableData` static source to CPU offset zero. The decode producer appends `[0xe0000440, C+952]`; this closes the mapped source interval and descriptor address/length, but not runtime execution.

For the already-parsed IDR, the original NAL header contributes 8 bits and the slice syntax contributes 24, so producer `b=32`, `q=1`, residual 0. The known `parent+128==0` CPU-resource branch computes codec address as `resource_codec_iova + (input_cpu-resource_cpu) + NAL_offset + 4*(bits>>5)`. The child words are `[0xb0000001, 0, 17, 0xa0000011, address]`. The 57-byte fixture mapping yields `V+40`; a 21-byte escaped IDR-only mapping yields `N+4`. Converting the full fixture to 4-byte NAL lengths creates a 58-byte resource and IDR offset 37, yielding `V+41`. All are modeled/tested as separate coordinate bases. The original parser requires length-prefixed NAL input and does not accept the raw Annex B fixture unchanged. See [the resource layout](A1599-FIRST-IDR-RESOURCE-LAYOUT-20261014.json) and [the VLC/FE/RENDEC evidence](A1599-VLC-FE-RENDEC-MINIMUM-GAPS-20261014.md).
