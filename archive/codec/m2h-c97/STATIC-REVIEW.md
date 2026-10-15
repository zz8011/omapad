# Static review status

## Implemented and checked offline

- Exactly five authoritative INIT mappings are required in fixed order and
  exact extents in both C and Python serializers.
- Seven decode buffers are DMA-API allocated and zeroed before first hardware
  start; pre-start failure rolls back allocations, while started memory is
  permanently pinned.
- The page-table builder validates all twelve mappings and verifies the seven
  decode PTEs against their runtime codec IOVAs and DMA handles.
- The harness fills the recovered 1088-byte VLC table and IDR fixture, creates
  the 136-word/544-byte stream using real allocated codec IOVAs, and performs a
  DMA write barrier before the gated one-shot send.
- The production probe calls the same tested DMA preflight helper as the host
  test: it accepts only initial 32-bit masks, noncoherent DMA, no range map,
  and no IOMMU. Allocation temporarily widens to 36 bits and restores both
  original masks; partial-set rollback is covered by the shared mock test.
- The ID81 helper checks ring emptiness, publishes once, polls bounded H2M
  consumption and M2H response without advancing the M2H consumer, and
  validates C0 fence 1. It classifies bit-27 detail and any other nonzero C0
  status as an error. Host mocks cover zero status, both nonzero classes,
  malformed fence, timeout, and preflight rejection.
- Submission is default-off. The first single-IDR type-1 packet is fixed to
  ID81 byte 0 `0x14`, flags `0x4015` (R16=1, final group `0x4004`,
  mapping-dirty `0x10`), sequence/fence 1, selector 0, opaque `0x10211000`,
  actual command codec IOVA, and current runtime root.
- INIT's prerequisite is H2M transport consumption, explicitly named in state
  and logs; semantic INIT completion remains unproven. C0 status zero is not
  reported as decode success: the code compares no pixels and logs
  `pixels_compared=0 decode_success=unclaimed`. Read-only binary export is
  separately default-off; after zero-status C0 it requires intact guard and
  equal full-buffer reads 50 ms apart, then exposes only an immutable 4-KiB
  snapshot. An offline FFmpeg comparator checks bounded planar/semiplanar
  candidates and marks unmatched layouts.

## Not established

- One device trial of the frozen base module occurred after this static review:
  first-ready arrived in 131 ms, INIT was consumed on H2M, and exactly one
  ID81 request returned a valid C0 with status `0x48064455` and detail `0`.
  No output was captured or exported, and hardware decode is not established.
  The diagnostic derivative itself has not been loaded. See
  `../evidence/a1599-decode-trial-20261014/TRIAL-RESULT.md`.
- Ring consumption does not prove semantic INIT completion. A C0 record with a
  matching fence and zero status still does not prove pixels were decoded or
  that all device DMA has ceased.
- OF defaults are 32-bit masks and noncoherent DMA. The successful `590d4af6`
  baseline temporarily applied 36-bit streaming/coherent masks around DMA
  allocations, restored the OF defaults, and completed first-ready plus INIT;
  the current candidate copies that exact policy. Its logged baseline DMA
  handles were above 4 GiB. The added decode PTEs, VXD cache visibility, output
  format/plane order, prefetch extent and decoded-pixel validity remain
  unverified.
- The output layout and actual decoded pixels remain unverified. A short
  unchanged interval and exact match to the software reference do not establish
  that the hardware will never DMA later or reproduce the same result.

## Device-trial outcome

The latest recorded trial reached first-ready and INIT transport consumption.
Its single ID81 response was a valid nonzero C0 (`0x48064455`, detail `0`).
The trial did not capture or export an image. These observations do not
identify an Apple-side codec root cause. Preserve the raw evidence at
`../evidence/a1599-decode-trial-20261014/TRIAL-RESULT.md`.

The output guard is observed and all memory remains pinned, but the current
candidate does not claim a stable output frame or completed codec feature.
