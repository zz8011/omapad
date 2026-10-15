# A1599 VXD/VXE bring-up dossier (offline, read-only)

Date: 2026-10-12. This is a static design and artifact review. The user later
reported that the iPad battery had run out and the USB link disconnected. A
read-only host process check found no `a1599_h264_runner` or `wf-recorder`
process; no stop signal or device reconnection was attempted. The target-side
process state was not checked. No device registers were read or written, no
driver was loaded, and no iOS firmware was downloaded or unpacked in this task.

## Executive assessment

Static A1599 firmware evidence establishes the relevant hardware/software
families for this target: the supplied iPad4,7 / j85map iOS 12.5.7
BuildManifest, DeviceTree and kernelcache report VXD393/VXE380 nodes and
matching decoder/encoder kext personalities. The forensic report is now
preserved locally at [A1599-iOS12.5.7-CODEC-EVIDENCE.md](A1599-iOS12.5.7-CODEC-EVIDENCE.md).
The examined firmware is older than the user's 12.5.8; raw Apple components
remain separate and were not copied into this workspace.

The currently running Alpine kernel does not contain a VXD/VXE driver path.
Its exact build configuration and its matching A1599 Linux DTB were found
locally. The DTB is for `apple,j85m` / `apple,s5l8960x`, but has no VXD/VXE or
media-codec node. The config has `CONFIG_MEDIA_SUPPORT` off and does not enable
USB FunctionFS. It does include generic Apple DART support, but that does not
attach an absent codec node or establish a DMA path for the codec. Therefore a
VXD hardware-decode path requires new kernel integration and likely firmware
and protocol work; the current image cannot use it as a userspace toggle.

The best next step remains static: compare the supplied iOS 12.5.7 node/kext
metadata against the exact iOS 12.5.8 components, then compare firmware
strings, memory/queue structures and platform resource descriptions against
pinned public driver sources. The presence of the blocks is no longer the
open question; register semantics, firmware ABI and DMA/clock/reset wiring
are. Do not probe MMIO.

## Evidence and provenance

| Evidence | Finding | Confidence / caveat |
|---|---|---|
| Running iPad, maintenance-shell read | `Linux 7.3.0-rc1`, model `Apple iPad mini 3 (Wi-Fi)`; `/dev/dri/card0` is `simple-framebuffer`; framebuffer name `simpledrmdrmfb`; no VXD/VXE/DART/codec/video/IOMMU-named runtime DT paths were found by bounded name search. | Direct current-device observation. No register access. |
| Host USB (before the reported battery discharge) | iPad enumerated as a High-Speed 480 Mbps composite CDC ACM gadget with three ACM functions; the stream used ACM1. | Historical host observation. User later reported the iPad ran out of battery and disconnected; no host runner or `wf-recorder` remains. Target-side state was not checked. |
| Running-image binding | Current fixed-v1 bundle's first 9,553,530 bytes and v2.4 bundle's first 9,553,530 bytes both hash to `93ae225e1f25c2cdc0de508ef11471b4acb9e3b32b26b0270b7df378e7e7084c`. The prefix includes the unchanged Linux Image.gz and DTB. | Direct SHA-256 comparison; ties the v2.4 build config/DTB to the kernel prefix used by the running fixed-v1 bundle. |
| Matching Linux config | `out/kernel/config-v2.4`, SHA-256 `89fbdcb8ba5dd840cca838ebc42e119eadb268784060f434315ed13993a90ae5`. `CONFIG_MEDIA_SUPPORT` unset; `CONFIG_DRM_SIMPLEDRM=y`; `CONFIG_DRM_GUD` unset; `CONFIG_USB_CONFIGFS_ACM=y`; `CONFIG_USB_CONFIGFS_F_FS` and `CONFIG_USB_FUNCTIONFS` unset; `CONFIG_DMA_SHARED_BUFFER=y`; `CONFIG_CMA=y` with 16 MiB; `CONFIG_APPLE_DART=y`; `CONFIG_COMMON_CLK=y`; `CONFIG_RESET_CONTROLLER=y`; `CONFIG_APPLE_PMGR_PWRSTATE=y`; generic PM domains enabled. | Exact local config for the matching kernel prefix. DART build support is not proof that a DART/codec route is described in this DTB. |
| Matching Linux DTB | `s5l8960x-j85m-v2.4.dtb`, SHA-256 `23a55a0cac3e543c00741f36ce0d1b3550d99e1248a5ef77f5d9b89b1cd12239`; compatibility `apple,j85m`, `apple,s5l8960x`. `dtc` inspection shows USB, PMGR and framebuffer nodes, but no VXD/VXE/codec/video node. | Exact local DTB paired with the current kernel prefix. Its omissions explain why this Linux build does not bind a codec driver; they do not disprove the silicon block. |
| iOS 12.5.7 identity/extraction (parallel report, saved locally) | BuildManifest identifies iPad4,7 / j85map / CPID `0x8960` / BDID `0x32`, build 16H81. Report says only ZIP directory, manifest and two components were fetched by HTTP ranges; ZIP CRCs checked; kernelcache decompressed to 27,192,152 bytes with Adler32 and ARM64 Mach-O validation; decompressed-kernel SHA-256 `0e0b42b00f69d87bcd2d1b62baf769c040513c4cced275275350f9b58c1cf3a1`. | Strong supplied static provenance. The local file preserves the report, not raw extracted bytes. Firmware is older than the user's iOS 12.5.8. |
| iOS 12.5.7 DeviceTree VXD/VXE resources | Exact j85map DT report: VXD compatibles `vxd,s5l8960x` / fallback `vxd,s5l8920x`, child offset `0x07f00000`, size `0x100000`, IRQ 80, powergate 119, clock ID 287, version 4. VXE compatibles `vxe,s5l8960x` / `vxe,s5l8940x`, offset `0x07d00000`, size `0x100000`, IRQ 82, powergate 120, clock ID 286. Parent arm-io range reportedly maps child 0 to `0x200000000`, yielding candidate CPU physical bases `0x207f00000` / `0x207d00000`. | Static exact-target node evidence from the supplied extraction report. DT offsets/size/IDs are known; physical bases are range translations, not permission to access. Reset wiring, IRQ trigger/type and DART stream IDs remain unknown. |
| iOS 12.5.7 prelinked kext personalities | Decoder module 141: `com.apple.driver.AppleVXD393` v3.28.7, `IOClass=AppleVXD393`, matches `vxd,s5l8920x`. Encoder module 159: `com.apple.driver.AppleVXE380` v445.6.0, `IOClass=AppleVXE380Driver`, matches `vxe,s5l8940x` and `vxe,s5l8945x`. j85map DT supplies those fallback strings and s5l8960x identities. | Exact-board static match, subject to the 12.5.7 vs 12.5.8 version difference. Confirms family selection, not a Linux implementation or compatible firmware ABI. |
| Decoder firmware/interface strings (received from cloud task) | `AppleVXD393` contains strings including `MSVDXDevInit`, `MSVDXStart`, MMU and firmware-loading terms. | Useful search anchors for related Imagination MSVDX material. Strings alone do not establish Intel Poulsbo register layout, firmware message ABI, command ring format or binary compatibility. |
| Local iOS firmware artifacts | The forensic report is saved locally; raw IPSW/IM4P/kernelcache/kext bytes are not. | Report metadata is available for local analysis. Raw extraction is not independently re-hashed here; do not search outside scoped paths. |

## Gap matrix

| Layer | What exists now | What VXD393/VXE380 bring-up still needs | Likely work class |
|---|---|---|---|
| Device discovery / MMIO | Supplied exact-target iOS 12.5.7 j85map DT reports VXD/VXE register windows, and the prelinked kext match strings agree with the SoC fallback compatibles. Local Linux DTB remains without codec nodes. | Compare the same properties in iOS 12.5.8. Reported child offsets are `0x07f00000` (VXD) / `0x07d00000` (VXE), each `0x100000`; reported arm-io mapping yields candidate CPU physical bases `0x207f00000` / `0x207d00000`. Internal register offsets/revision IDs are unknown. | Linux DT source must add reviewed codec resources. Physical base calculations are clues only; absolutely no MMIO access in this phase. |
| Clock, reset, power | Supplied iOS DT reports VXD clock ID 287/powergate 119 and VXE clock ID 286/powergate 120. Linux image has generic clock/reset/PM support and Apple PMGR power-state support; its power-state driver matches generic `apple,pmgr-pwrstate`. | Clock provider/ID namespace for A7 is not identified in local Linux sources; no s5l8960x clock-controller match was found. Need correlate each ID and powergate to its PMGR parent and probe ordering. Reset resources/lines and reset timing are unknown from the supplied summary. | PMGR power-state machinery may be reusable; clock and reset integration remains a likely missing platform layer. Do not assume Linux's generic support makes these resources controllable. |
| IRQ / firmware boot | Supplied iOS DT reports VXD IRQ 80 / VXE IRQ 82; matching Linux DTB has no codec IRQ consumer. | Confirm interrupt-parent/specifier, trigger, Linux virq mapping, reset/clock sequence, firmware source/licensing, upload transport, firmware compatibility and panic recovery. The numbers `80/82` alone do not specify Linux IRQ semantics. | New kernel driver plus legally sourced firmware metadata; first validation must not write hardware until a reviewed init sequence exists. |
| MTX firmware / boot ABI | Cloud static comparison reports VXD393/PSB register-level overlap at MTX access and firmware-upload registers, including bank-control and interrupt handling. | No ready firmware ABI. Reported communication signature differs (`0x2fe0` VXD393 vs `0x2cc0` Poulsbo); firmware format, message IDs/semantics, boot mailbox and complete H.264 command payload remain unresolved. | A bounded static reuse hypothesis is justified; no direct PSB firmware or protocol port. |
| Command ring / messages | Cloud static comparison reports ring-index differences, descriptor-derived buffer layout, and message-length field width 7 bits (VXD393) vs 8 bits (PSB). | Full VXD393 ring ownership/geometry, descriptor semantics, message catalog, fences and completion/error handling remain unresolved. | Do not reuse PSB ring structs or message parser unchanged. |
| MMU / DMA | Linux config has CMA 16 MiB, DMA-BUF, and Apple DART; bundled DART driver has `apple,s5l8960x-dart` support. Apple decoder strings mention MMU, but supplied DT report has not provided DART stream/context IDs. | Need exact codec DART association, IOVA width/aperture, page-table format/ownership, invalidation protocol, cache coherency, alignment/tiling and firmware-visible addresses. | DART IP driver may be a foundation; codec attach/wiring and DMA ABI are not established. Do not translate guessed IOVAs or write DART registers. |
| Userspace API | No V4L2 media support in current config; no `/dev/video*` device. | For the H.264 display workload, expose a stateful V4L2 mem2mem decoder (or a narrowly scoped prototype API); implement formats, controls, streaming queues, timestamps, drain/flush, errors and DMABUF/import strategy. VXE is an encoder and is not needed to decode the current host stream. | Kernel/media subsystem implementation and FFmpeg integration. |
| USB display path | The validated implementation sent H.264 over ACM1; three ACM ports were used and FunctionFS is not enabled. The link is currently reported disconnected. | A hardware decoder could replace FFmpeg's software decode if V4L2 works and output reaches fb0 with low-copy conversion. A separate GUD path also needs a gadget function while preserving ACM0/1/2. | Prefer codec-only prototype first; GUD is a separate USB/display project. |

## Register / firmware / queue cross-check status

| Item | Static fact currently available | Still unknown (do not guess) |
|---|---|---|
| VXD resource | 12.5.7 j85map DT report: child offset `0x07f00000`, length `0x100000`; arm-io translation reports base `0x207f00000`; `vxd,s5l8960x` / fallback `vxd,s5l8920x`; version 4; matching AppleVXD393 personality. | Register map, endian/stride, reset sequence, revision ID, internal RAM windows and errata. The physical base is an inference from ranges and must remain unused. |
| VXE resource | 12.5.7 j85map DT report: child offset `0x07d00000`, length `0x100000`; arm-io translation reports base `0x207d00000`; compatibles `vxe,s5l8960x` / `vxe,s5l8940x`; AppleVXE380 also matches `vxe,s5l8945x`. | Register map/revision, VXE boot sequence, encoder firmware and internal memory layout. VXE is not needed for current H.264 decode. |
| Interrupts | DT reports VXD 80 and VXE 82. | Parent interrupt controller, cell encoding, level/edge, polarity, Linux runtime IRQ mapping and status/ack registers. |
| Clocks / power domains | DT reports VXD clock/powergate IDs 287/119 and VXE 286/120. Current Linux has generic PMGR power-state support. | A7 clock provider/ID namespace, PMGR translation, regulator dependencies, reset lines and ordering/retention behavior. |
| DART / MMU | Current Linux source supports S5L8960x DART; Apple VXD393 strings mention MMU. | Exact codec DART association/stream ID, context, page table descriptors, invalidation, firmware-visible addresses, DMA alignment and cache rules. |
| Firmware/MTX | Decompressed ARM64 kernel image SHA-256 `0e0b42b00f69d87bcd2d1b62baf769c040513c4cced275275350f9b58c1cf3a1`; AppleVXD393 v3.28.7; strings include `MSVDXDevInit`, `MSVDXStart`, MMU and firmware loading. | Firmware placement/format, entry point, MTX ABI/version, boot mailbox, message IDs, and compatibility with any PSB/TI format. |
| Command queue | PSB MSVDX shows MTX ring indices/message IDs/fences; TI defines PVDEC firmware messages. | VXD393 ring geometry, framing/ownership, message schema and fence semantics are not verified by strings. No foreign register or structure constants may be assumed. |

## Cloud static register/firmware crosswalk (not hardware validation)

The parallel kernelcache analysis reports concrete register overlap between
AppleVXD393 and the public Poulsbo MSVDX implementation:

| Area | Reported VXD393 ↔ Poulsbo overlap | Reported difference / consequence |
|---|---|---|
| MTX register access | `0xf8` / `0xfc` access pair | Register-level similarity is evidence for shared MTX lineage, not by itself a whole-core ABI match. |
| MTX RAM upload | `0x104` / `0x108` / `0x10c` upload sequence, including bank-control fields | This may support reuse of upload mechanics after offset/bitfield-level static cross-check; no register writes are authorized. |
| MTX interrupt | Handler flow has matching MTX interrupt handling | Exact status, mask, clear/ack semantics and all error cases remain to be compared. |
| Firmware comms signature | VXD393 signature at `0x2fe0`; Poulsbo at `0x2cc0` | Different signatures mean firmware/bootstrap memory layout cannot be assumed identical. |
| Command ring | Ring index definitions differ | The VXD393 implementation must use its own indices and ring layout. |
| Buffer addressing | VXD393 buffers are derived from descriptors | Do not assume PSB's buffer/object address calculation or ownership rules. |
| Message framing | VXD393 message-length field is 7 bits; Poulsbo uses 8 bits | A PSB message parser/validator cannot be reused unchanged; every message layout needs VXD393-specific validation. |

This result supports a **register-helper / MTX-structure reference** only. It
does not establish the complete H.264 command payload, compatible executable
firmware, redistribution rights, DMA/MMU mapping, clock/reset path, or parity
with the user's iOS 12.5.8. It is a cloud static-analysis result; no matching
register was read on the iPad and no driver or firmware was run.

The source artifact was a merged, stripped kernelcache. Although the report
identifies module indices, kext personalities and code slices, it does not
justify claiming an independently extractable AppleVXD393 Mach-O kext.

## Code-reuse assessment

### TI Imagination VXE/VXD (user-supplied reference)

The supplied forensic report identifies TI's `ti-linux-6.6.y` tree at
`drivers/media/platform/imagination/vxe-vxd` as GPL-2.0 V4L2 code for a
D5520MP2 decoder and VXE384MP2 encoder. Its report describes PVDEC/MSVDX
register headers, `img_vdec_fw_msg.h`, Topaz encoder firmware headers, a
GPL-2.0 encoder firmware C array, and decoder firmware `pvdec_full_bin.fw`.
Those core identities differ from A7's VXD393/VXE380. The report's source
review is preserved in the companion evidence file; this task did not fetch
or execute TI code.

Potentially reusable after side-by-side source review: modern V4L2 mem2mem and
videobuf2 structure, runtime-PM/clock/IRQ lifecycle patterns, queue/fence
handling, format negotiation, and separation of hardware-independent media
API from register backend. Not reusable without proof: PVDEC/MSVDX offsets,
core IDs, `pvdec_full_bin.fw`, message ABI, IMG MMU setup, DMA tiling, reset
sequence, interrupt semantics, codec profiles or per-family capability data.

The local Linux tree has Imagination's E5010 JPEG encoder as a modern V4L2
mem2mem example (`drivers/media/platform/imagination/e5010-jpeg-enc.c`), but
E5010 is a different encoder block. It can inform Linux API organization only,
not VXD/VXE programming.

### Intel Poulsbo MSVDX (`gregkh/psb-kmp`)

The cloud static comparison reports register-level overlap with
`psb_msvdx.h` / `psb_msvdxinit.c` around MTX access, RAM upload, bank fields
and interrupts. It also reports materially different firmware communication
signature, ring indices, descriptor-derived buffers and message-length width.
These findings justify checking the relevant register helper mechanics, not
porting the driver. The files explicitly target Poulsbo D0/D1 and
`msvdx_fw.bin`; the driver also depends on PCI, Intel GTT/TTM and old DRM,
which do not map directly to A7 platform MMIO, DART, PMGR or modern V4L2
mem2mem. Full H.264 command ABI and firmware compatibility remain open.
The local Linux build tree's `drivers/gpu/drm/gma500` directory contains the
display/GMA500 sources but no `psb_msvdx.c`, `psb_msvdx.h` or codec init file;
the local A1599 image therefore has no staged copy of the old codec driver.

License note: the MSVDX source files carry an MIT-style permission notice, but
the repo's `psb_drv.c` is GPL-2.0. Treat the repository as mixed-license and
review each file before any code reuse.

### Intel AOSP PSB VA-API userspace

The companion report identifies Google's AOSP `hardware/intel/img/psb_video`
as a VA-API userspace reference. It reports permissive licensing in
`configure.ac`, dependencies on libva/libdrm/libwsbm/X11, MSVDX/Topaz firmware
directories and Android build targets including PSBVIDEO_VXD392. VXD392 is
adjacent Imagination hardware, not evidence of VXD393 compatibility. Potential
reuse is limited to userspace codec/buffer-submission patterns after checking
its UAPI and firmware assumptions; it is not the A7 kernel module.

## Minimal static analysis sequence

1. Use the saved forensic report and hashes as the fixed iOS 12.5.7 baseline;
   keep the 12.5.8 version distinction explicit.
2. Compare only the separately verified iOS 12.5.8 DeviceTree and relevant
   kernelcache/kext metadata. Do not fetch a full IPSW. Preserve component hashes.
3. Use the delivered static VXD393/PSB register findings as a candidate
   crosswalk; compare full H.264 commands, firmware packaging and MMU details
   only when the parallel report supplies those facts. Do not redo its merged,
   stripped kernelcache code-slice analysis here or describe it as an extracted
   standalone kext Mach-O.
4. Separately verify the same resources against iOS 12.5.8 metadata before
   treating them as current. No MMIO reads/writes.
5. Keep VXD decode as the first functional target because the former stream
   was H.264-in to framebuffer; defer VXE encode and USB GUD until decode and
   framebuffer handoff are proven independently. The stream is currently
   disconnected and no host capture process was found.

## Sources

- A1599 local build artifacts: `../task-2/a1599-alpine-fixed/MANIFEST.txt`,
  `../task-2/a1599-alpine-fixed/out/a1599-alpine-m1n1-linux-fixed-v1.bundle`,
  and `/home/zz8011/Documents/Codex/2026-10-11/task-2/.build/a1599/v2.4/out/`.
- Saved parallel-task iOS 12.5.7 report: [A1599-iOS12.5.7-CODEC-EVIDENCE.md](A1599-iOS12.5.7-CODEC-EVIDENCE.md).
- TI source lead (not fetched in this task):
  https://git.ti.com/cgit/ti-linux-kernel/ti-linux-kernel/tree/drivers/media/platform/imagination/vxe-vxd?h=ti-linux-6.6.y
- TI reference files named in the supplied report:
  https://github.com/TexasInstruments/ti-linux-kernel/blob/ti-linux-6.6.y/drivers/media/platform/imagination/vxe-vxd/decoder/vxd_pvdec_regs.h
  https://github.com/TexasInstruments/ti-linux-kernel/blob/ti-linux-6.6.y/drivers/media/platform/imagination/vxe-vxd/decoder/img_vdec_fw_msg.h
  https://github.com/TexasInstruments/ti-linux-kernel/blob/ti-linux-6.6.y/drivers/media/platform/imagination/vxe-vxd/decoder/vxd_core.c
  https://github.com/TexasInstruments/ti-linux-kernel/blob/ti-linux-6.6.y/drivers/media/platform/imagination/vxe-vxd/encoder/fw_headers/topazscfwif.h
- MSVDX reference sources:
  https://github.com/gregkh/psb-kmp/blob/master/psb_msvdx.c
  https://github.com/gregkh/psb-kmp/blob/master/psb_msvdx.h
  https://github.com/gregkh/psb-kmp/blob/master/psb_msvdxinit.c
  https://github.com/gregkh/psb-kmp/blob/master/psb_drv.c
- Intel AOSP VA-API reference:
  https://android.googlesource.com/platform/hardware/intel/img/psb_video/
- TI hardware family data sheet (D5520MP2/VXE384MP2):
  https://www.ti.com/lit/ds/symlink/tda4vm.pdf
