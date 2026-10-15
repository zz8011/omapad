# A1599 normal-C0 coherent output snapshot candidate

Current module SHA-256:
`1211a6abbf026a9354f05a44e710c16ec7e8286cb0708b86b9ee260aa59bbbaa`.

See [RESULT-20261014.md](RESULT-20261014.md) for the Linux DMA contract,
original Apple completion/timer trace, bounded snapshot gates and regressions.
With explicit load-time output opt-in, a matching normal C0 can produce an
immutable 4-KiB snapshot after two equal coherent-buffer observations. This is
not a pixel-success or permanent-DMA-quiescence claim. No device trial occurred.

Frozen 4367/c97f candidates are unchanged. Prior classification-only 006e output
is historical and archived, not the current implementation. Keep the adjacent
`../id81_host_contract` dependency when moving this directory.

Host comparator review fix: NV21 now interprets VU order correctly. All seven
comparison tests pass, including independent unequal-U/V positive and negative
NV12/NV21 fixtures. See RESULT section 4a and HOST-COMPARATOR-CHANGES.patch.
The module above was not rebuilt or changed.
