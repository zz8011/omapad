# Frozen A1599 first-IDR output candidate

This independent directory freezes the source, documentation, regression tests,
output comparator, and AArch64 module for same-host review. At freeze time no
iPad, USB, module load, or hardware submission had been performed; the later
single trial of this frozen module is recorded below.

- Module: `arm64-build/a1599_fw_candidate.ko`
- Module SHA-256: `cf08ac4b28b1ac64186d372c43f0d666db5dc88320e5ff5cf33a0d40662960a4`
- `vermagic`: `7.3.0-rc1 SMP preempt aarch64`
- Fixed ID81: byte `0x14`, flags `0x4015`, sequence/fence 1, selector 0,
  136 command words, runtime command IOVA and current root, opaque `0x10211000`.
- All active actions remain explicit load-time opt-ins. Output export is limited
  to a fixed 4096-byte snapshot after zero-status C0, intact guard, and equal
  whole-buffer reads 50 ms apart.
- The frozen base module received one RAM-only trial on 2026-10-14. See
  `../evidence/a1599-decode-trial-20261014/TRIAL-RESULT.md` for its outcome;
  do not repeat the one-shot. `BOUNDED-DECODE-EXPERIMENT.md` records the
  parameters and procedure used in that trial.

Verify the complete frozen tree with `sha256sum -c FROZEN-MANIFEST.sha256` and
verify the module with `sha256sum -c arm64-build/SHA256SUMS`.
