/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef A1599_FW_DECODE_LAYOUT_H
#define A1599_FW_DECODE_LAYOUT_H

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/types.h>
typedef u32 a1599_layout_u32;
#else
#include <errno.h>
#include <stdint.h>
typedef uint32_t a1599_layout_u32;
#endif

#define A1599_DECODE_INIT_MAPS 5U
#define A1599_DECODE_RESOURCE_COUNT 7U
#define A1599_DECODE_IOVA_START 0x40012000U
#define A1599_DECODE_IOVA_LIMIT 0x40100000U
#define A1599_DECODE_PAGE_BYTES 0x1000U

enum a1599_decode_resource_id {
	A1599_DECODE_PRELOAD,
	A1599_DECODE_VLC,
	A1599_DECODE_BITSTREAM,
	A1599_DECODE_CURRENT_PICTURE,
	A1599_DECODE_INTRA,
	A1599_DECODE_OUTPUT,
	A1599_DECODE_COMMAND,
};

struct a1599_decode_iova_map {
	a1599_layout_u32 iova;
	a1599_layout_u32 bytes;
};

static inline int a1599_decode_assign_iovas(
	const struct a1599_decode_iova_map *init_maps, unsigned int init_count,
	const a1599_layout_u32 *resource_bytes, unsigned int resource_count,
	a1599_layout_u32 *resource_iovas)
{
	static const struct a1599_decode_iova_map expected[A1599_DECODE_INIT_MAPS] = {
		{0x40000000U, 0x0000e000U}, {0x40010000U, 0x00002000U},
		{0x40100000U, 0x00100000U}, {0x40300000U, 0x00040000U},
		{0x40340000U, 0x00001000U},
	};
	a1599_layout_u32 cursor = A1599_DECODE_IOVA_START;
	unsigned int i, j;

	if (!init_maps || !resource_bytes || !resource_iovas ||
	    init_count != A1599_DECODE_INIT_MAPS ||
	    resource_count != A1599_DECODE_RESOURCE_COUNT)
		return -EINVAL;
	for (i = 0; i < A1599_DECODE_INIT_MAPS; i++) {
		if (init_maps[i].iova != expected[i].iova ||
		    init_maps[i].bytes != expected[i].bytes)
			return -EINVAL;
	}
	for (i = 0; i < A1599_DECODE_INIT_MAPS; i++) {
		a1599_layout_u32 end = init_maps[i].iova + init_maps[i].bytes;
		if ((init_maps[i].iova & (A1599_DECODE_PAGE_BYTES - 1)) ||
		    !init_maps[i].bytes ||
		    (init_maps[i].bytes & (A1599_DECODE_PAGE_BYTES - 1)) ||
		    end < init_maps[i].iova)
			return -ERANGE;
		for (j = 0; j < i; j++) {
			a1599_layout_u32 prior_end = init_maps[j].iova + init_maps[j].bytes;
			if (init_maps[i].iova < prior_end &&
			    init_maps[j].iova < end)
				return -EADDRINUSE;
		}
	}
	for (i = 0; i < A1599_DECODE_RESOURCE_COUNT; i++) {
		a1599_layout_u32 end;
		if (!resource_bytes[i] ||
		    (resource_bytes[i] & (A1599_DECODE_PAGE_BYTES - 1)) ||
		    cursor > A1599_DECODE_IOVA_LIMIT ||
		    resource_bytes[i] > A1599_DECODE_IOVA_LIMIT - cursor)
			return -ERANGE;
		resource_iovas[i] = cursor;
		end = cursor + resource_bytes[i];
		for (j = 0; j < A1599_DECODE_INIT_MAPS; j++) {
			a1599_layout_u32 init_end = init_maps[j].iova + init_maps[j].bytes;
			if (cursor < init_end && init_maps[j].iova < end)
				return -EADDRINUSE;
		}
		cursor = end;
	}
	return 0;
}

#endif
