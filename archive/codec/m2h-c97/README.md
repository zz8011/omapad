# A1599 M2H read-only snapshot candidate

This offline derivative of the first-IDR diagnostics module adds one fixed,
read-only, one-shot capture of the first eight 32-bit words in the M2H ring.
The sysfs binary attribute is `m2h_ring_raw_once` (32 bytes). It accepts only a
single read at offset zero with count 32, and only after both the INIT transport
and one decode request have been consumed. A failed precondition or read is not
retriable in the same module instance.

The implementation uses the driver's existing `devm_ioremap_resource` mapping
and serializes with all host-driver ring transactions using the existing
`a1599_ring_lock`. It checks exact expected ring descriptors, indices and M2H
bounds before reading, performs eight `readl()` calls at offsets
`0x8000..0x801c`, then rechecks descriptors and indices before returning the
bytes. It does not update the consumer index, write a ring register, accept an
address parameter, or create another mapping. The capture range is
`[0x8000,0x8020)` in the driver's MMIO resource.

The mutex excludes concurrent CPU access through this driver only. It cannot
stop the firmware from writing asynchronously, and matching producer indices
before and after do not prove that no firmware store is in flight. Treat the
bytes as a bounded diagnostic snapshot, not an atomic firmware quiescence
snapshot or proof of decode success.

To use in a future, separately approved RAM boot, load this exact module with
the required one-shot module parameters and read exactly 32 bytes once from the
platform device's `m2h_ring_raw_once` sysfs binary attribute. No such operation
is included in this deliverable. This artifact has not been uploaded or loaded.

See [BUILD-RESULT.md](BUILD-RESULT.md) for the build command and hashes.
