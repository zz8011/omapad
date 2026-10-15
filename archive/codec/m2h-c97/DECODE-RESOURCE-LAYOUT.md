# First-IDR decode resource layout

All intervals are half-open codec IOVA intervals. Linux DMA handles are
retained separately and are never substituted for codec IOVAs.

## Exact INIT mapping contract

Both serializers require these five mappings in this exact order and with
these exact extents:

| Mapping | IOVA | Bytes |
|---|---:|---:|
| Firmware staging | `0x40000000` | `0xe000` |
| INIT extra | `0x40010000` | `0x2000` |
| INIT context large | `0x40100000` | `0x100000` |
| INIT context medium | `0x40300000` | `0x40000` |
| INIT context small backing | `0x40340000` | `0x1000` |

No missing, reordered, duplicate, or changed map is accepted. Firmware rings
are within the 1-MiB VXD MMIO resource, not codec IOVA allocations.

## Seven decode allocations

Each allocation is DMA-API backed, zeroed, checked for physical overlap, and
paired with a page-aligned codec IOVA before page-table construction. Current
IOVA assignment is deterministic within the free gap; DMA handles are freshly
provided by the kernel allocator.

| Allocation | Requested/used | Backing | Codec IOVA | Serialized reference |
|---|---:|---:|---:|---|
| PreloadBuffer | 648 B | 4 KiB | `0x40012000` | control header word 2 |
| VLC table | 1088 B | 4 KiB | `0x40013000` | descriptor word 1 |
| Bitstream | 21 B fixture | 4 KiB | `0x40014000` | payload descriptor word 4 |
| Current-picture params | 16 KiB | 16 KiB | `0x40015000` | AD8 payload 6 |
| INTRA | 64 KiB | 64 KiB | `0x40019000` | `0x1018` value |
| Output surface | 3072 B active | 4 KiB | `0x40029000` | two `0x1000` plane addresses |
| Command stream | 544 B | 4 KiB | `0x4002a000` | ID81 `+8` address |

The current-picture requested size is `align_up((4 macroblocks + 100)*128,
4096)`. The recovered VLC bytes are 1088 bytes from the matching kernelcache
at offset `0x14e9e8`, SHA-256
`8098a4dc421d35bdd38a5ac16d4618a86f1167d2e7f8aa87f4f09fbd5a3b9f65`.
The small IDR fixture and output geometry are experiments, not validated
hardware semantics.

The output hypothesis uses 64-byte stride, plane 0 offset 0 with 2048 bytes,
plane 1 offset 2048 with 1024 bytes, and a 1024-byte trailing guard. The
candidate checks/logs the guard after a C0 response and retains the allocation;
the guard observation does not prove all DMA has stopped or pixels are stable.

## Lifetime and hardware limits

All seven allocations are made before first hardware start and included in the
sealed table set. Any pre-start allocation failure rolls back in reverse
order. Once start succeeds, the shared lifetime helper refuses release. The
candidate sends at most one ID81 and never advances the M2H consumer.

The OF defaults are 32-bit masks and noncoherent DMA; the successful `590d4af6`
baseline temporarily sets streaming and coherent masks to 36 bits for
`dma_alloc_coherent()`, then restores the defaults. The current candidate
retains this same allocation policy. It adds these seven buffers and their
PTEs to the existing 4-MiB codec IOVA region; the root allocation (4 KiB,
64-KiB aligned), leaf format, PTE address encoder, firmware SHA, and five INIT
IOVA mappings match the baseline. The seven new mapped pages share the existing
root index and need no additional root entry beyond that already used by the
region. The seven allocations are performed before allocating root/leaves, so
their runtime DMA handles can differ from a prior boot; PTEs use the returned
handles, and ID81 uses the current root handle. This is a required consequence
of including the new DMA buffers before table construction, not a hard-coded
root change. The DTS/embedded DTBO and firmware SHA are identical to baseline.
VXD PTE translation/cache behavior for the added pages, prefetch bounds and
output layout still require hardware evidence. See the readiness document for
remaining assumptions.
