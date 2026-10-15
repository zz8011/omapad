#include "id81_contract.h"

#ifdef __KERNEL__
#include <linux/limits.h>
#include <linux/string.h>
#else
#include <string.h>
#endif

#define ID81_ADDRESS_SPACE_END (1ULL << 32)
#define ID81_ROOT_MAX 0xffffffffffULL

static void put_le16(uint8_t *p, uint16_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
}

static void put_le32(uint8_t *p, uint32_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
	p[2] = (uint8_t)(value >> 16);
	p[3] = (uint8_t)(value >> 24);
}

static uint16_t get_le16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

enum id81_result id81_pack(const struct id81_pack_input *input,
			   uint8_t output[ID81_MESSAGE_BYTES])
{
	uint8_t wire[ID81_MESSAGE_BYTES] = {0};
	uint64_t mapping_end;
	uint64_t command_end;
	uint64_t root_word;
	uint32_t context_word;
	uint32_t command_words;

	if (!input || !output)
		return ID81_BAD_ARGUMENT;
	if (input->preserve_byte0_bit7 > 1)
		return ID81_BAD_ARGUMENT;
	if (!input->command_bytes || (input->command_bytes & 3U))
		return ID81_BAD_LENGTH;
	command_words = input->command_bytes / 4U;
	if (command_words > ID81_MAX_COMMAND_WORDS)
		return ID81_WORD_COUNT_OVERFLOW;
	if ((input->command_iova & 3U) || (input->mapping_iova & 3U))
		return ID81_BAD_ALIGNMENT;

	mapping_end = (uint64_t)input->mapping_iova + input->mapping_bytes;
	command_end = (uint64_t)input->command_iova + input->command_bytes;
	if (mapping_end > ID81_ADDRESS_SPACE_END ||
	    command_end > ID81_ADDRESS_SPACE_END)
		return ID81_ADDRESS_OVERFLOW;
	if (!input->mapping_bytes || input->command_iova < input->mapping_iova ||
	    command_end > mapping_end)
		return ID81_OUT_OF_MAPPING;

	if ((input->root_device_address & 0xffffU) != 0 ||
	    input->root_device_address > ID81_ROOT_MAX)
		return ID81_BAD_ROOT;
	root_word = (input->root_device_address >> 8) & 0xffffff00ULL;
	context_word = (uint32_t)root_word | input->selector;

	wire[0] = (uint8_t)(ID81_LENGTH_BYTES |
			   (input->preserve_byte0_bit7 ? 0x80U : 0U));
	wire[1] = ID81_MESSAGE_ID;
	put_le16(wire + 2, input->group_sequence);
	put_le16(wire + 4, input->flags);
	put_le16(wire + 6, (uint16_t)command_words);
	put_le32(wire + 8, input->command_iova);
	put_le32(wire + 12, context_word);
	put_le32(wire + 16, input->opaque);
	memcpy(output, wire, sizeof(wire));
	return ID81_OK;
}

enum c0_result c0_parse(const uint8_t *message, size_t available,
			uint16_t expected_fence, struct c0_response *output)
{
	struct c0_response parsed = {0};
	size_t declared_bytes;

	if (!message || !output)
		return C0_BAD_ARGUMENT;
	if (available < 2)
		return C0_TRUNCATED_HEADER;
	if (message[1] != C0_MESSAGE_ID)
		return C0_WRONG_ID;

	declared_bytes = message[0] & 0x7fU;
	if (declared_bytes < 12)
		return C0_BAD_LENGTH;
	if (declared_bytes > available)
		return C0_TRUNCATED_MESSAGE;

	parsed.fence = get_le16(message + 2);
	parsed.word4 = get_le16(message + 4);
	parsed.status = get_le32(message + 8);
	if (parsed.fence != expected_fence)
		return C0_WRONG_FENCE;

	if (parsed.status & C0_STATUS_DETAIL_BIT) {
		if (declared_bytes < 16)
			return C0_BAD_LENGTH;
		parsed.dispatch_class = C0_DISPATCH_8010;
		parsed.has_detail = 1;
		parsed.detail = get_le32(message + 12);
	} else {
		parsed.dispatch_class = C0_DISPATCH_10;
	}

	*output = parsed;
	return C0_OK;
}
