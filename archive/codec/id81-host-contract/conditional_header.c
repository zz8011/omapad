#include "conditional_header.h"

#include <stddef.h>
#include <stdint.h>

#define A1599_U32_END (UINT64_C(1) << 32)

enum a1599_header_result a1599_build_conditional_header(
	const struct a1599_header_input *input,
	uint32_t words[A1599_HEADER_WORDS], uint32_t *plane0_pitch_1040)
{
	uint64_t preload_end, output_end;
	uint32_t local[A1599_HEADER_WORDS];

	if (!input || !words || !plane0_pitch_1040)
		return A1599_HEADER_BAD_ARGUMENT;
	if (!input->preload_iova || !input->preload_mapping_bytes ||
	    input->preload_iova < input->preload_mapping_iova)
		return A1599_HEADER_PRELOAD_UNMAPPED;
	preload_end = (uint64_t)input->preload_mapping_iova +
		input->preload_mapping_bytes;
	if (preload_end > A1599_U32_END || input->preload_iova >= preload_end)
		return A1599_HEADER_PRELOAD_UNMAPPED;
	if (!input->plane0_stride || input->plane0_stride >= 65536U ||
	    (input->plane0_stride & 63U) || !input->plane1_stride ||
	    input->plane1_stride >= 65536U)
		return A1599_HEADER_BAD_STRIDE;
	if (input->output_backing_bytes < A1599_CODEC_PAGE_BYTES)
		return A1599_HEADER_OUTPUT_BACKING_SHORT;
	output_end = (uint64_t)input->output_iova + input->output_backing_bytes;
	if (!input->output_iova || output_end > A1599_U32_END)
		return A1599_HEADER_OUTPUT_ADDRESS_OVERFLOW;

	local[0] = 0x90000000U;
	local[1] = 0x00001004U;
	local[2] = input->preload_iova;
	local[3] = input->plane1_stride | (input->plane1_stride << 16);
	local[4] = 0x00000101U;
	local[5] = 0;
	local[6] = 0;
	local[7] = 0;
	local[8] = 1;
	*plane0_pitch_1040 = input->plane0_stride & 0xffffffc0U;
	for (size_t i = 0; i < A1599_HEADER_WORDS; ++i)
		words[i] = local[i];
	return A1599_HEADER_OK;
}
