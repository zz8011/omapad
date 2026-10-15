# INIT result history

The INIT-only experiment has already run. Use the current evidence record:
[INIT-RESULT.md](../evidence/a1599-init-experiment-20261014/INIT-RESULT.md).
At 04:51 UTC on 2026-10-14, the one accepted INIT store returned zero, the
producer index reached 5 and H→M consumer advanced from 0 to 5 in 0 ms. The
module logged `semantic_init_complete=unproven`, retained PM/DMA state, and no
decode was submitted.

Older design documents called for a fresh RAM boot before the INIT attempt;
that event is now history. The current boot is live and must not be unloaded,
reset or retried. This new decode candidate is unsubmitted and does not change
the current MMU. Read [DECODE-RESOURCE-LAYOUT.md](DECODE-RESOURCE-LAYOUT.md)
and [BOUNDED-DECODE-EXPERIMENT.md](BOUNDED-DECODE-EXPERIMENT.md) for the
remaining decode preparation and experiment boundary.
