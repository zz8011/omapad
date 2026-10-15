# M2H read-only patch build result (2026-10-14)

This offline derivative adds a fixed 32-byte M2H payload snapshot interface to
`a1599_fw_first_ready_platform_harness.c`. It builds as an AArch64 Linux kernel
module against the existing 7.3.0-rc1 kernel output tree.

Build command:

```sh
make -C /home/zz8011/Documents/Codex/2026-10-11/task-2/.build/a1599/src/linux \
  O=/tmp/a1599-kbuild-decode \
  M=/home/zz8011/Documents/Codex/2026-10-13/task-6/a1599_first_decode_candidate_m2h_snapshot_20261014 \
  MO=/tmp/a1599-m2h-snapshot-module \
  ARCH=arm64 LLVM=1 KCFLAGS=-DCONFIG_A1599_FW_EXPLICIT_EXPERIMENT modules
```

Result: successful compile, link, and MODPOST. `modinfo -F vermagic` reports
`7.3.0-rc1 SMP preempt aarch64`.

Artifact: `build/a1599_fw_candidate.ko`  
SHA-256: `c97fb883bbe39269bb9f16a9074305ce870e3366f563c72bf4b5ba77d6745fb2`

Source: `a1599_fw_first_ready_platform_harness.c`  
SHA-256: `63e017e4f3edc39b6ad023ba8d4c6646144748eb1e75e349aeb3e9de1422ebf2`

The device was not contacted for this build. This module was not uploaded or
loaded, no new SRAM was read, and the existing device/module/PM/DMA state was
not altered by these host-side actions.
