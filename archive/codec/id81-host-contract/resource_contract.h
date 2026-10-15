#ifndef A1599_RESOURCE_CONTRACT_H
#define A1599_RESOURCE_CONTRACT_H

#include <stdint.h>

enum a1599_resource_result {
	A1599_RESOURCE_OK = 0,
	A1599_RESOURCE_BAD_ARGUMENT,
	A1599_RESOURCE_BAD_MAPPING,
	A1599_RESOURCE_OUT_OF_MAPPING,
	A1599_RESOURCE_BAD_LENGTH,
	A1599_RESOURCE_ADDRESS_OVERFLOW,
};

struct a1599_mapped_resource {
	uint32_t iova;
	uint32_t mapped_bytes;
};

struct a1599_vlc_descriptor {
	uint32_t word0;
	uint32_t word1;
};

struct a1599_nal_input {
	/* Codec IOVA and length for the resource beginning at resource_cpu. */
	uint32_t mapping_iova;
	uint32_t mapping_bytes;
	/* (input_cpu - resource_cpu) + parsed NAL start offset. */
	uint32_t nal_offset;
	uint32_t nal_bytes;
	uint32_t consumed_header_bits;
};

struct a1599_bitstream_extent {
	uint32_t iova;
	uint32_t bytes;
	uint8_t residual_bits;
};

#define A1599_SLICE_CHILD_WORDS 5U

enum a1599_resource_result a1599_build_vlc_descriptor(
	const struct a1599_mapped_resource *mapping,
	uint32_t data_offset, uint32_t payload_bytes,
	struct a1599_vlc_descriptor *descriptor);

enum a1599_resource_result a1599_get_nal_payload_extent(
	const struct a1599_nal_input *input,
	struct a1599_bitstream_extent *extent);

enum a1599_resource_result a1599_build_slice_child(
	const struct a1599_mapped_resource *mapping,
	const struct a1599_bitstream_extent *extent,
	uint32_t words[A1599_SLICE_CHILD_WORDS]);

#endif
