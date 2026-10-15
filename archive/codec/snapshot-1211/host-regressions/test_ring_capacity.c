#include <assert.h>
#include <stdio.h>

#include "../a1599_fw_ring_capacity.h"

int main(void)
{
	assert(!a1599_empty_ring_has_contiguous_space(5, 0, 0, 5));
	assert(a1599_empty_ring_has_contiguous_space(6, 0, 0, 5));
	assert(a1599_empty_ring_has_contiguous_space(6, 1, 1, 5));
	assert(!a1599_empty_ring_has_contiguous_space(6, 2, 2, 5));
	assert(!a1599_empty_ring_has_contiguous_space(6, 0, 1, 5));
	assert(!a1599_empty_ring_has_contiguous_space(6, 6, 6, 5));
	assert(!a1599_empty_ring_has_contiguous_space(0, 0, 0, 5));
	assert(!a1599_empty_ring_has_contiguous_space(6, 0, 0, 0));
	assert(!a1599_fw_abi_ring_snapshot_matches(false,
		0x60cc01cc, 0x60000033, 0, 0, 0, 0,
		0x80cc, 0x87fc, 0x8000, 0x80cc, 0x100000));
	assert(a1599_fw_abi_ring_snapshot_matches(true,
		0x60cc01cc, 0x60000033, 0, 0, 0, 0,
		0x80cc, 0x87fc, 0x8000, 0x80cc, 0x100000));
	assert(!a1599_fw_abi_ring_snapshot_matches(true,
		0x60cc01cd, 0x60000033, 0, 0, 0, 0,
		0x80cc, 0x87fc, 0x8000, 0x80cc, 0x100000));
	assert(!a1599_fw_abi_ring_snapshot_matches(true,
		0x60cc01cc, 0x60000033, 0, 1, 0, 0,
		0x80cc, 0x87fc, 0x8000, 0x80cc, 0x100000));
	assert(!a1599_fw_abi_ring_snapshot_matches(true,
		0x60cc01cc, 0x60000033, 0, 0, 0, 0,
		0x80cc, 0x87fc, 0x8000, 0x80cc, 0x8000));
	puts("ring capacity and firmware-ABI gate checks passed (13 cases)");
	return 0;
}
