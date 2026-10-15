#include "resource_contract.h"

#include <stddef.h>
#include <stdint.h>

#define A1599_ADDRESS_SPACE_END (UINT64_C(1) << 32)
#define A1599_VLC_DESCRIPTOR_TAG 0xe0000000U
#define A1599_VLC_DESCRIPTOR_MAX_BYTES 0xfffeU

static enum a1599_resource_result resource_end(
	const struct a1599_mapped_resource *mapping, uint64_t *end)
{
	if (!mapping || !end)
		return A1599_RESOURCE_BAD_ARGUMENT;
	if (!mapping->iova || !mapping->mapped_bytes)
		return A1599_RESOURCE_BAD_MAPPING;
	*end = (uint64_t)mapping->iova + mapping->mapped_bytes;
	if (*end > A1599_ADDRESS_SPACE_END)
		return A1599_RESOURCE_ADDRESS_OVERFLOW;
	return A1599_RESOURCE_OK;
}

enum a1599_resource_result a1599_build_vlc_descriptor(
	const struct a1599_mapped_resource *mapping,
	uint32_t data_offset, uint32_t payload_bytes,
	struct a1599_vlc_descriptor *descriptor)
{
	struct a1599_vlc_descriptor local;
	uint64_t end, data_start, data_end;
	enum a1599_resource_result result;

	if (!descriptor)
		return A1599_RESOURCE_BAD_ARGUMENT;
	result = resource_end(mapping, &end);
	if (result != A1599_RESOURCE_OK)
		return result;
	if (!payload_bytes || payload_bytes > A1599_VLC_DESCRIPTOR_MAX_BYTES)
		return A1599_RESOURCE_BAD_LENGTH;
	data_start = (uint64_t)mapping->iova + data_offset;
	data_end = data_start + payload_bytes;
	if (data_end < data_start || data_end > end)
		return A1599_RESOURCE_OUT_OF_MAPPING;
	if (data_start >= A1599_ADDRESS_SPACE_END)
		return A1599_RESOURCE_ADDRESS_OVERFLOW;

	local.word0 = A1599_VLC_DESCRIPTOR_TAG | payload_bytes;
	local.word1 = (uint32_t)data_start;
	*descriptor = local;
	return A1599_RESOURCE_OK;
}

enum a1599_resource_result a1599_get_nal_payload_extent(
	const struct a1599_nal_input *input,
	struct a1599_bitstream_extent *extent)
{
	struct a1599_bitstream_extent local;
	struct a1599_mapped_resource mapping;
	uint64_t end, nal_start, nal_end, payload_start;
	enum a1599_resource_result result;

	if (!input || !extent)
		return A1599_RESOURCE_BAD_ARGUMENT;
	mapping.iova = input->mapping_iova;
	mapping.mapped_bytes = input->mapping_bytes;
	result = resource_end(&mapping, &end);
	if (result != A1599_RESOURCE_OK)
		return result;
	nal_start = (uint64_t)input->mapping_iova + input->nal_offset;
	nal_end = nal_start + input->nal_bytes;
	if (nal_start < input->mapping_iova || nal_end < nal_start ||
	    nal_end > end)
		return A1599_RESOURCE_OUT_OF_MAPPING;
	if (input->consumed_header_bits > (uint64_t)input->nal_bytes * 8U)
		return A1599_RESOURCE_BAD_LENGTH;
	payload_start = nal_start + input->consumed_header_bits / 8U;
	if (payload_start >= A1599_ADDRESS_SPACE_END)
		return A1599_RESOURCE_ADDRESS_OVERFLOW;

	local.iova = (uint32_t)payload_start;
	local.bytes = input->nal_bytes - input->consumed_header_bits / 8U;
	local.residual_bits = (uint8_t)(input->consumed_header_bits % 8U);
	*extent = local;
	return A1599_RESOURCE_OK;
}

enum a1599_resource_result a1599_build_slice_child(
	const struct a1599_mapped_resource *mapping,
	const struct a1599_bitstream_extent *extent,
	uint32_t words[A1599_SLICE_CHILD_WORDS])
{
	uint32_t local[A1599_SLICE_CHILD_WORDS];
	uint64_t end, slice_end;
	enum a1599_resource_result result;

	if (!extent || !words)
		return A1599_RESOURCE_BAD_ARGUMENT;
	result = resource_end(mapping, &end);
	if (result != A1599_RESOURCE_OK)
		return result;
	if (extent->residual_bits)
		return A1599_RESOURCE_BAD_LENGTH;
	if (!extent->bytes || extent->iova < mapping->iova)
		return A1599_RESOURCE_OUT_OF_MAPPING;
	slice_end = (uint64_t)extent->iova + extent->bytes;
	if (slice_end < extent->iova || slice_end > end)
		return A1599_RESOURCE_OUT_OF_MAPPING;

	local[0] = 0xb0000001U;
	local[1] = 0;
	local[2] = extent->bytes;
	local[3] = 0xa0000011U;
	local[4] = extent->iova;
	for (size_t i = 0; i < A1599_SLICE_CHILD_WORDS; ++i)
		words[i] = local[i];
	return A1599_RESOURCE_OK;
}
