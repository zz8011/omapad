// SPDX-License-Identifier: GPL-2.0-only
/* Fixture-specific host/kernel CPU serializer; never submits the stream. */
#include "a1599_fw_decode_stream.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <limits.h>
#include <string.h>
#endif

#define PAGE_BYTES 4096U
#define U32_SPACE 0x100000000ULL

struct register_pair { u32 target; u32 value; };

static const struct register_pair static_pairs[] = {
	{0x048008ec, 0x00026000}, {0x048008f0, 0x000738a0},
	{0x048008f4, 0x000828f4}, {0x048008f8, 0x000a312d},
	{0x048008fc, 0x000b5959}, {0x04800900, 0x000c517b},
	{0x04800904, 0x000d1196}, {0x04800908, 0x000db1ad},
	{0x0480090c, 0x000e21be}, {0x04800910, 0x000e59c8},
	{0x04800914, 0x000e79cd}, {0x04800918, 0x000eb1d3},
	{0x0480091c, 0x000ed1d8}, {0x04800920, 0x000f09dd},
	{0x04800924, 0x000f71e7}, {0x04800928, 0x001041f6},
	{0x0480092c, 0x1256a4dd}, {0x04800930, 0x01489292},
	{0x04800934, 0x11248050}, {0x04800938, 0x000022a2},
	{0x0480093c, 0x00002a02}, {0x04800940, 0x8108282a},
	{0x04800944, 0x00000012}, {0x048009c0, 0x0010920e},
	{0x048009c4, 0x0010d216}, {0x048009c8, 0x0010f21c},
};

static void put_le32(unsigned char *out, u32 value)
{
	out[0] = (unsigned char)value;
	out[1] = (unsigned char)(value >> 8);
	out[2] = (unsigned char)(value >> 16);
	out[3] = (unsigned char)(value >> 24);
}

static int valid_span(const struct a1599_decode_span *span, u32 minimum_bytes,
		      u32 referenced_bytes)
{
	unsigned long long end;

	if (!span || !span->bytes || (span->base_iova & (PAGE_BYTES - 1)) ||
	    (span->bytes & (PAGE_BYTES - 1)) || span->bytes < minimum_bytes ||
	    span->codec_iova < span->base_iova)
		return -EINVAL;
	end = (unsigned long long)span->base_iova + span->bytes;
	if (end > U32_SPACE ||
	    (unsigned long long)span->codec_iova + referenced_bytes > end)
		return -ERANGE;
	return 0;
}

static int validate_disjoint_iovas(const struct a1599_decode_resources *r)
{
	static const struct a1599_decode_span authoritative_init_maps[] = {
		{0x40000000U, 0x0000e000U, 0x40000000U},
		{0x40010000U, 0x00002000U, 0x40010000U},
		{0x40100000U, 0x00100000U, 0x40100000U},
		{0x40300000U, 0x00040000U, 0x40300000U},
		{0x40340000U, 0x00001000U, 0x40340000U},
	};
	const struct a1599_decode_span *spans[] = {
		&r->preload, &r->vlc, &r->bitstream, &r->current_picture,
		&r->intra, &r->output, &r->command,
	};
	size_t count = sizeof(spans) / sizeof(spans[0]);
	size_t i, j;

	if (!r->existing_iova_maps ||
	    r->existing_iova_map_count !=
			 sizeof(authoritative_init_maps) / sizeof(authoritative_init_maps[0]))
		return -EINVAL;
	for (i = 0; i < r->existing_iova_map_count; i++)
		if (r->existing_iova_maps[i].base_iova !=
				authoritative_init_maps[i].base_iova ||
		    r->existing_iova_maps[i].bytes !=
				authoritative_init_maps[i].bytes ||
		    r->existing_iova_maps[i].codec_iova !=
				authoritative_init_maps[i].codec_iova)
			return -EINVAL;
	for (i = 0; i < count; i++) {
		unsigned long long end_i = (unsigned long long)spans[i]->base_iova +
			spans[i]->bytes;
		for (j = i + 1; j < count; j++) {
			unsigned long long end_j =
				(unsigned long long)spans[j]->base_iova + spans[j]->bytes;
			if (spans[i]->base_iova < end_j &&
			    spans[j]->base_iova < end_i)
				return -EADDRINUSE;
		}
		for (j = 0; j < r->existing_iova_map_count; j++) {
			const struct a1599_decode_span *old = &r->existing_iova_maps[j];
			unsigned long long end_old =
				(unsigned long long)old->base_iova + old->bytes;
			if (!old->bytes || (old->base_iova & (PAGE_BYTES - 1)) ||
			    (old->bytes & (PAGE_BYTES - 1)) || end_old > U32_SPACE)
				return -EINVAL;
			if (spans[i]->base_iova < end_old && old->base_iova < end_i)
				return -EADDRINUSE;
		}
	}
	for (i = 0; i < r->existing_iova_map_count; i++) {
		unsigned long long end_i = (unsigned long long)r->existing_iova_maps[i].base_iova +
			r->existing_iova_maps[i].bytes;
		for (j = i + 1; j < r->existing_iova_map_count; j++) {
			unsigned long long end_j = (unsigned long long)r->existing_iova_maps[j].base_iova +
				r->existing_iova_maps[j].bytes;
			if (r->existing_iova_maps[i].base_iova < end_j &&
			    r->existing_iova_maps[j].base_iova < end_i)
				return -EADDRINUSE;
		}
	}
	return 0;
}

static int append_word(unsigned char *out, size_t *count, size_t capacity, u32 v)
{
	if (*count >= capacity)
		return -ENOSPC;
	put_le32(out + (*count) * 4, v);
	(*count)++;
	return 0;
}

static int append_pairs(unsigned char *out, size_t *count, size_t capacity,
			const struct register_pair *pairs, size_t pair_count)
{
	size_t i = 0;
	int ret;

	while (i < pair_count) {
		size_t first = i, run = 1;
		while (first + run < pair_count &&
		       pairs[first + run].target == pairs[first + run - 1].target + 4)
			run++;
		ret = append_word(out, count, capacity,
			0x10000000U | ((u32)run << 16) | (pairs[first].target & 0xffff));
		if (ret)
			return ret;
		while (run--) {
			ret = append_word(out, count, capacity, pairs[i++].value);
			if (ret)
				return ret;
		}
	}
	return 0;
}

int a1599_build_fixture_decode_stream(void *output, size_t output_capacity,
		const struct a1599_decode_resources *r)
{
	struct register_pair fe_pairs[] = {
		{0x0480080c, 0x11}, {0x04800a00, 0x7701},
		{0x04800a04, 0}, {0x04800a08, 0x30108},
		{0x04800a0c, 0x10080000}, {0x04800a10, 0x5000},
		{0x04800a18, 0},
	};
	unsigned char *out = output;
	size_t n = 0;
	u32 extent = 0x0001f01f;
	u32 words[81];
	size_t w = 0, i;
	int ret;

	if (!out || !r || output_capacity < A1599_DECODE_STREAM_BYTES ||
	    (output_capacity & 3))
		return -EINVAL;
	if ((ret = valid_span(&r->preload, PAGE_BYTES, 648)) ||
	    (ret = valid_span(&r->vlc, PAGE_BYTES, 1088)) ||
	    (ret = valid_span(&r->bitstream, PAGE_BYTES, 17)) ||
	    (ret = valid_span(&r->current_picture, 0x4000, 0x4000)) ||
	    (ret = valid_span(&r->intra, 0x10000, 0x10000)) ||
	    (ret = valid_span(&r->output, PAGE_BYTES, PAGE_BYTES)) ||
	    (ret = valid_span(&r->command, PAGE_BYTES, A1599_DECODE_STREAM_BYTES)))
		return ret;
	ret = validate_disjoint_iovas(r);
	if (ret)
		return ret;
	if ((unsigned long long)r->plane0_offset + 64U * 32U > r->output.bytes ||
	    (unsigned long long)r->plane1_offset + 64U * 16U > r->output.bytes ||
	    (unsigned long long)r->output.codec_iova + r->plane0_offset > UINT_MAX ||
	    (unsigned long long)r->output.codec_iova + r->plane1_offset > UINT_MAX)
		return -ERANGE;
	if (r->plane0_offset & 63 || r->plane1_offset & 63)
		return -EINVAL;
	if ((unsigned long long)r->plane0_offset + 64U * 32U > r->plane1_offset &&
	    (unsigned long long)r->plane1_offset + 64U * 16U > r->plane0_offset)
		return -EADDRINUSE;

#define APP(v) do { ret = append_word(out, &n, output_capacity / 4, (v)); \
		if (ret) return ret; } while (0)
	/* 36-byte control header; current fixture is 32x32, 8-bit, 4:2:0. */
	APP(0x90000000); APP(0x00001008); APP(r->preload.codec_iova);
	APP(0x00400040); APP(0x00000101); APP(0); APP(0); APP(0); APP(1);
	APP(0xe0000440); APP(r->vlc.codec_iova);
	ret = append_pairs(out, &n, output_capacity / 4,
			   static_pairs, sizeof(static_pairs) / sizeof(static_pairs[0]));
	if (ret) return ret;
	APP(0xb0000001); APP(0); APP(17); APP(0xa0000011); APP(r->bitstream.codec_iova);
	ret = append_pairs(out, &n, output_capacity / 4, fe_pairs,
			   sizeof(fe_pairs) / sizeof(fe_pairs[0]));
	if (ret) return ret;

	/* Exact 81-word main emitter for the closed 2x2-MB fixture branch. */
	words[w++] = 0x50010810; words[w++] = 0x11;
	words[w++] = 0x50070ad8; words[w++] = 0x7701; words[w++] = 0;
	words[w++] = 0x30108; words[w++] = 0x10080000; words[w++] = 0x5000;
	words[w++] = r->current_picture.codec_iova; words[w++] = 0;
	words[w++] = 0x50380700;
	for (i = 0; i < 56; i++) words[w++] = 0x10101010;
	words[w++] = 0x50051000; words[w++] = extent; words[w++] = extent;
	words[w++] = 0x10211000;
	words[w++] = r->output.codec_iova + r->plane0_offset;
	words[w++] = r->output.codec_iova + r->plane1_offset;
	words[w++] = 0x50011018; words[w++] = r->intra.codec_iova;
	words[w++] = 0x50011024; words[w++] = 0x55;
	words[w++] = 0x50011040; words[w++] = 0x40;
	words[w++] = 0x500111b4; words[w++] = 1;
	if (w != 81) return -EINVAL;
	for (i = 0; i < w; i++) APP(words[i]);
	APP(0x60000000);
#undef APP
	return n == A1599_DECODE_STREAM_WORDS ?
		(int)A1599_DECODE_STREAM_BYTES : -EINVAL;
}
