/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef A1599_FW_DECODE_STREAM_H
#define A1599_FW_DECODE_STREAM_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
typedef uint32_t u32;
#endif

struct a1599_decode_span {
	u32 base_iova;
	u32 bytes;
	u32 codec_iova;
};

struct a1599_decode_resources {
	struct a1599_decode_span preload;       /* 648-byte request, >= 4-KiB map */
	struct a1599_decode_span vlc;           /* 1088-byte table, >= 4-KiB map */
	struct a1599_decode_span bitstream;     /* 17-byte payload */
	struct a1599_decode_span current_picture; /* 16-KiB request */
	struct a1599_decode_span intra;         /* 64-KiB request */
	struct a1599_decode_span output;        /* candidate output surface */
	struct a1599_decode_span command;       /* 544-byte stream backing */
	const struct a1599_decode_span *existing_iova_maps;
	size_t existing_iova_map_count;
	u32 plane0_offset;
	u32 plane1_offset;
};

#define A1599_DECODE_STREAM_WORDS 136U
#define A1599_DECODE_STREAM_BYTES (A1599_DECODE_STREAM_WORDS * 4U)

/* CPU serialization only. This helper has no DMA, MMIO, ring, or submit path. */
int a1599_build_fixture_decode_stream(void *output, size_t output_capacity,
		const struct a1599_decode_resources *resources);

#endif
