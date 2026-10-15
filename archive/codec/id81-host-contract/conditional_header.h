#ifndef A1599_CONDITIONAL_HEADER_H
#define A1599_CONDITIONAL_HEADER_H

#include <stdint.h>

#define A1599_HEADER_WORDS 9U
#define A1599_CODEC_PAGE_BYTES 4096U

enum a1599_header_result {
	A1599_HEADER_OK = 0,
	A1599_HEADER_BAD_ARGUMENT,
	A1599_HEADER_BAD_STRIDE,
	A1599_HEADER_PRELOAD_UNMAPPED,
	A1599_HEADER_OUTPUT_BACKING_SHORT,
	A1599_HEADER_OUTPUT_ADDRESS_OVERFLOW,
};

/* Fixed fixture branch: H264, 8-bit direct-primary, 32x32 progressive IDR. */
struct a1599_header_input {
	uint32_t preload_iova;
	uint32_t preload_mapping_iova;
	uint32_t preload_mapping_bytes;
	uint32_t plane0_stride;
	uint32_t plane1_stride;
	uint32_t output_iova;
	uint32_t output_backing_bytes;
};

enum a1599_header_result a1599_build_conditional_header(
	const struct a1599_header_input *input,
	uint32_t words[A1599_HEADER_WORDS], uint32_t *plane0_pitch_1040);

#endif
