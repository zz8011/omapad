# First decode candidate readiness (2026-10-14)

## Subsequent device-trial status

The offline-only statements below describe the candidate before the later
one-shot trial. On 2026-10-14, the frozen base module (`cf08ac4b…62960a4`)
was loaded once in RAM: first-ready arrived in 131 ms and INIT was consumed on
H2M, while semantic INIT remained unproven. Exactly one ID81 request returned
a valid C0 with status/flags `0x48064455` and detail `0`. No output was captured
or exported; hardware decode is not established. The diagnostic derivative
(`46de66e0…be74607`) has not been loaded. See
[`TRIAL-RESULT.md`](../evidence/a1599-decode-trial-20261014/TRIAL-RESULT.md)
for the unchanged evidence record.

## Implemented offline

The candidate now allocates and zeroes seven decode spans before building and
sealing the VXD tables: preload, VLC, bitstream, current-picture parameters,
INTRA, output, and command stream. Runtime codec IOVAs are page-aligned,
disjoint from the fixed five INIT ranges and from one another. The command
serializer and Python contract require exactly those five INIT mappings in
their exact order and extents.

The platform path fills the recovered VLC table and IDR fixture, serializes
the 136-word/544-byte command from the allocated codec IOVAs, executes a DMA
write barrier, and exposes a one-shot ID81 path. The path is default-off and
requires explicit load-time opt-in. The source-resolved ID81 byte is `0x14`
and flags are `0x4015` (R16=1, final-group `0x4004`, mapping-dirty `0x10`).
It checks both rings
empty, publishes once, polls bounded H2M consumption and a C0 record, and does
not advance the M2H consumer. Resources remain pinned after start or any
submission outcome. Host mocks exercise allocation rollback/lifetime, exact
mapping contracts, the shared DMA mask contract, and ID81/C0 status classes.

## Limits and hardware gates

This is not a verified hardware decode. The later device trial produced only
the nonzero C0 result summarized above. The gate uses observed INIT transport
consumption as its prerequisite; semantic INIT completion remains unproven and
no dedicated ACK is required. Source-backed values select
the first context selector 0, sequence/fence 1, root from the live codec
page-table root, and opaque `0x10211000`. C0 parsing checks structure and fence;
bit 27 is classified as an error-detail response, and any nonzero status is
returned as an error. Even status zero does not establish decoded output: no
pixels are captured or compared, and the log says
`pixels_compared=0 decode_success=unclaimed`.

The read-only `decode_output` binary sysfs file is a separate default-off
feature. After a zero-status C0, the module checks an intact guard, samples
the entire 4096-byte output allocation twice 50 ms apart, and makes an
immutable snapshot available only if the samples match and the guard remains
intact. This bounded stable interval does not prove the device will never DMA
later. `tools/compare_output.py` screens finite YUV420P, YVU420P, NV12, and
NV21 stride/offset candidates against FFmpeg's 32×32 software reference and
reports `UNMATCHED_LAYOUT` if none match.

The OF defaults are 32-bit masks with noncoherent DMA and no `dma-ranges` or
IOMMU. The successful `590d4af6` baseline temporarily sets streaming and
coherent masks to 36 bits around its DMA allocations, then restores the OF
values; the current candidate preserves that exact policy. First-ready and
INIT succeeded on that baseline, whose logged DMA handles were above 4 GiB.
The new decode allocations use the same 36-bit policy. Their codec PTE
translation and VXD cache/coherency behavior remain untested, as do output
plane layout, prefetch safety, and decoded contents. A matching C0 response is
not proof that the device has stopped accessing the buffers, so the code never
frees allocations after first start.

The module is built for the observed AArch64 `7.3.0-rc1 SMP preempt` kernel.
Its firmware `a1599/vxd-staging-12.5.8-16H88.bin` SHA-256
`6b274c576a7952d7a4f22f65adf5b1041c4329c3dfc7ce761de53668cfd4a41e`, OF
preflight, 36-bit temporary DMA-mask policy, root and
leaf table encoding, five INIT IOVAs, PM setup, and symbol-independent overlay
match the successful `590d4af6` baseline. The DTS hash is
`14bd24c1acf25d40a3493f202bd61923ec0088bd30f3ab37b133467460532717`; the
embedded DTBO hash is
`04a9b819e9969b5098e78719a95041bd58346286e8d233dfc0c10d41c21e0c77`. Both are
byte-identical to the baseline overlay and have no live-symbol fixups. The
added differences are seven pre-start DMA allocations, seven codec IOVAs/PTE
ranges within the existing root-table region, decode fixture/stream
preparation, and the gated ID81/C0 path. The 64-KiB root alignment, 4-KiB
root allocation, leaf format and existing INIT map values are unchanged. The
root/leaf DMA handles can shift because allocations are inserted before them;
they are dynamically consumed from the DMA API and encoded into the PTEs. The
ID81 packet uses that actual live root handle rather than a fixed address.

The 590d source checks the OF's initial `0xffffffff` masks and noncoherent
status, then temporarily calls `dma_set_mask_and_coherent(DMA_BIT_MASK(36))`
for allocation and restores the original masks. Its successful first-ready
and INIT run used this same 36-bit policy. The revised candidate keeps those checks
and the same temporary mask/restore path; it does not attempt decode with a
32-bit mask. The new code adds a write barrier before ID81 publication and a
read barrier before parsing the response. It continues to use the baseline's
`dma_alloc_coherent()` buffer policy; cache visibility in the VXD fabric is
not independently established by this source comparison.

The comparison baseline is the
[590d INIT candidate source](../a1599_init_fw_abi_ring_candidate/a1599_fw_first_ready_platform_harness.c)
and its [static-memory allocator](../a1599_init_fw_abi_ring_candidate/a1599_fw_static_boot_memory.c),
module SHA-256
`590d4af6aae49f38aa1c8e3d324ba39a887a0b82ac35b694c82823dff806a413`, recorded
in the [04:51 successful INIT result](../evidence/a1599-init-experiment-20261014/INIT-RESULT.md).
The artifact is for further review and a separately authorized hardware
experiment; it should not be treated as a completed codec feature.
