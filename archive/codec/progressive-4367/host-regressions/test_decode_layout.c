// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>

#include "../a1599_fw_decode_layout.h"

int main(void)
{
	static const struct a1599_decode_iova_map init_maps[] = {
		{0x40000000, 0x0000e000}, {0x40010000, 0x00002000},
		{0x40100000, 0x00100000}, {0x40300000, 0x00040000},
		{0x40340000, 0x00001000},
	};
	static const a1599_layout_u32 sizes[A1599_DECODE_RESOURCE_COUNT] = {
		0x1000, 0x1000, 0x1000, 0x4000, 0x10000, 0x1000, 0x1000,
	};
	static const a1599_layout_u32 expected[A1599_DECODE_RESOURCE_COUNT] = {
		0x40012000, 0x40013000, 0x40014000, 0x40015000,
		0x40019000, 0x40029000, 0x4002a000,
	};
	struct a1599_decode_iova_map invalid[A1599_DECODE_INIT_MAPS];
	a1599_layout_u32 assigned[A1599_DECODE_RESOURCE_COUNT];
	unsigned int i;

	assert(a1599_decode_assign_iovas(init_maps, A1599_DECODE_INIT_MAPS,
		sizes, A1599_DECODE_RESOURCE_COUNT, assigned) == 0);
	for (i = 0; i < A1599_DECODE_RESOURCE_COUNT; i++)
		assert(assigned[i] == expected[i]);
	assert(a1599_decode_assign_iovas(init_maps, A1599_DECODE_INIT_MAPS - 1,
		sizes, A1599_DECODE_RESOURCE_COUNT, assigned) == -EINVAL);
	for (i = 0; i < A1599_DECODE_INIT_MAPS; i++)
		invalid[i] = init_maps[i];
	invalid[4] = invalid[0];
	assert(a1599_decode_assign_iovas(invalid, A1599_DECODE_INIT_MAPS,
		sizes, A1599_DECODE_RESOURCE_COUNT, assigned) == -EINVAL);
	for (i = 0; i < A1599_DECODE_INIT_MAPS; i++)
		invalid[i] = init_maps[i];
	invalid[1].iova += 0x1000;
	assert(a1599_decode_assign_iovas(invalid, A1599_DECODE_INIT_MAPS,
		sizes, A1599_DECODE_RESOURCE_COUNT, assigned) == -EINVAL);
	{
		a1599_layout_u32 too_large[A1599_DECODE_RESOURCE_COUNT];
		for (i = 0; i < A1599_DECODE_RESOURCE_COUNT; i++)
			too_large[i] = sizes[i];
		too_large[A1599_DECODE_INTRA] = 0x100000;
		assert(a1599_decode_assign_iovas(init_maps, A1599_DECODE_INIT_MAPS,
			too_large, A1599_DECODE_RESOURCE_COUNT, assigned) == -ERANGE);
	}
	puts("decode IOVA allocator mock tests: PASS");
	return 0;
}
