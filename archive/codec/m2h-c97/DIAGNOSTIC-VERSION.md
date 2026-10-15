# A1599 first-decode diagnostic build (offline)

This is an isolated derivative of `a1599_first_decode_candidate_frozen_20261014`.
The frozen directory remains unchanged. This derivative adds diagnostic capture
for the already existing one-shot path; it does not alter INIT or ID81 packing,
command-stream construction, or decode submission semantics.

## Captured evidence

- C0: actual response length, bounded raw bytes (up to the protocol's 127-byte
  maximum), response/record validity, and explicit presence for fields at +4
  (LastMB), +8 (Flags/status), and +12 (detail). The raw dump is independent of
  parser success, so malformed and short records remain inspectable.
- Output guard state is one of `not_checked`, `intact`, or `changed`. The state
  remains `not_checked` unless a valid zero-status C0 was followed by a guard
  read. An error, invalid record, or absent response cannot be reported as a
  changed guard.
- Before and after ID81: bounded ring snapshots, including H2M/M2H indices.
- Submission evidence: the exact 20-byte envelope returned by the existing
  serializer, all 136 command words (544 bytes), and the 12 actual IOVA to
  backing mappings plus page-table root/leaves.

## Validation and boundaries

`host-regressions/test_decode_diagnostics.c` provides offline fixtures for
zero-status success, nonzero error status, truncated response, short
error-detail record, unchecked/changed guard states, and absent C0. The helper
captures fields from bytes actually present, while retaining explicit field
presence.

The diagnostic module was compiled for the configured AArch64 kernel and has
not itself been loaded or transferred to a device. Separately, the frozen base
module was trialed once on 2026-10-14: first-ready arrived in 131 ms, INIT was
consumed on H2M, and the only ID81 response was valid C0 status/flags
`0x48064455`, detail `0`. No output was captured or exported, so hardware
decode is not established. The trial is recorded in
`../evidence/a1599-decode-trial-20261014/TRIAL-RESULT.md`. No codec root cause
is claimed. The output stability check remains a bounded observation only and
cannot establish that later DMA has ended.
