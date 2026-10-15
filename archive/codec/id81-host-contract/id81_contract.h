#ifndef ID81_CONTRACT_H
#define ID81_CONTRACT_H

#ifdef __KERNEL__
#include <linux/stddef.h>
#include <linux/types.h>
typedef u8 uint8_t;
typedef u16 uint16_t;
typedef u32 uint32_t;
typedef u64 uint64_t;
#else
#include <stddef.h>
#include <stdint.h>
#endif

#define ID81_MESSAGE_BYTES 20U
#define ID81_MESSAGE_ID 0x81U
#define ID81_LENGTH_BYTES 20U
#define ID81_MAX_COMMAND_WORDS 0xffffU
#define C0_MESSAGE_ID 0xc0U
#define C0_STATUS_DETAIL_BIT (1U << 27)

enum id81_result {
	ID81_OK = 0,
	ID81_BAD_ARGUMENT,
	ID81_BAD_LENGTH,
	ID81_WORD_COUNT_OVERFLOW,
	ID81_BAD_ALIGNMENT,
	ID81_ADDRESS_OVERFLOW,
	ID81_OUT_OF_MAPPING,
	ID81_BAD_ROOT,
};

/*
 * The values are host-supplied fields from the verified ID 0x81 envelope.
 * command_iova is the caller-supplied address for the ID81 +8 field. Its
 * expected command-buffer purpose is a caller contract, not inferred here.
 * Checks validate only a caller-provided mapping interval; they do not prove
 * the interval is mapped in VXD or that the address points to commands.
 */
struct id81_pack_input {
	uint8_t preserve_byte0_bit7;
	uint16_t group_sequence;
	uint16_t flags;
	uint32_t command_iova;
	uint32_t command_bytes;
	uint32_t mapping_iova;
	uint32_t mapping_bytes;
	uint64_t root_device_address;
	uint8_t selector;
	uint32_t opaque;
};

enum c0_result {
	C0_OK = 0,
	C0_BAD_ARGUMENT,
	C0_TRUNCATED_HEADER,
	C0_WRONG_ID,
	C0_BAD_LENGTH,
	C0_TRUNCATED_MESSAGE,
	C0_WRONG_FENCE,
};

enum c0_dispatch_class {
	C0_DISPATCH_10 = 0,
	C0_DISPATCH_8010 = 1,
};

struct c0_response {
	uint16_t fence;
	uint16_t word4;
	uint32_t status;
	enum c0_dispatch_class dispatch_class;
	uint8_t has_detail;
	uint32_t detail;
};

enum id81_result id81_pack(const struct id81_pack_input *input,
			   uint8_t output[ID81_MESSAGE_BYTES]);

enum c0_result c0_parse(const uint8_t *message, size_t available,
			uint16_t expected_fence, struct c0_response *output);

#endif
