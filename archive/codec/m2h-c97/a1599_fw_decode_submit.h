/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef A1599_FW_DECODE_SUBMIT_H
#define A1599_FW_DECODE_SUBMIT_H

#include "../id81_host_contract/id81_contract.h"
#include "a1599_fw_ring_capacity.h"

struct a1599_decode_ring_snapshot {
	a1599_ring_u32 h2m_descriptor;
	a1599_ring_u32 m2h_descriptor;
	a1599_ring_u32 h2m_read_index;
	a1599_ring_u32 h2m_write_index;
	a1599_ring_u32 m2h_read_index;
	a1599_ring_u32 m2h_write_index;
	a1599_ring_u32 h2m_capacity_words;
	a1599_ring_u32 m2h_capacity_words;
	a1599_ring_u32 h2m_start;
	a1599_ring_u32 m2h_start;
};

enum a1599_decode_c0_status_class {
	A1599_DECODE_C0_STATUS_ZERO,
	A1599_DECODE_C0_STATUS_NONZERO,
	A1599_DECODE_C0_STATUS_ERROR_DETAIL,
};

struct a1599_decode_submit_result {
	uint8_t envelope[ID81_MESSAGE_BYTES];
	struct a1599_init_mmio_result ring_send;
	struct a1599_ring_message message;
	struct c0_response response;
	a1599_ring_u32 consumer_after_poll;
	bool published;
	bool consumed;
	bool response_received;
	bool response_record_valid;
	bool response_error;
	enum a1599_decode_c0_status_class status_class;
};

/* One H2M ID81 and one bounded M2H observation. This deliberately leaves the
 * M2H consumer index untouched, so no further job can race an uncertain
 * decoder lifetime; the caller must retain every mapping on all outcomes.
 */
static inline int a1599_decode_submit_once(
		const struct a1599_init_mmio_ops *ops, void *context,
		const struct a1599_decode_ring_snapshot *snapshot,
		const struct id81_pack_input *pack,
		uint16_t expected_fence, a1599_ring_u32 timeout_ms,
		struct a1599_decode_submit_result *result)
{
	a1599_ring_u32 words[ID81_MESSAGE_BYTES / sizeof(a1599_ring_u32)];
	a1599_ring_u32 expected_regs[6], read_index;
	unsigned int i;
	int ret, parse_ret;

	if (!ops || !snapshot || !pack || !result || !timeout_ms)
		return -EINVAL;
	memset(result, 0, sizeof(*result));
	if (snapshot->h2m_read_index != snapshot->h2m_write_index ||
	    snapshot->m2h_read_index != snapshot->m2h_write_index)
		return -EBUSY;
	ret = id81_pack(pack, result->envelope);
	if (ret)
		return -EINVAL;
	for (i = 0; i < ID81_MESSAGE_BYTES / sizeof(a1599_ring_u32); i++)
		words[i] = (a1599_ring_u32)result->envelope[i * 4] |
			((a1599_ring_u32)result->envelope[i * 4 + 1] << 8) |
			((a1599_ring_u32)result->envelope[i * 4 + 2] << 16) |
			((a1599_ring_u32)result->envelope[i * 4 + 3] << 24);
	expected_regs[0] = snapshot->h2m_descriptor;
	expected_regs[1] = snapshot->m2h_descriptor;
	expected_regs[2] = snapshot->h2m_read_index;
	expected_regs[3] = snapshot->h2m_write_index;
	expected_regs[4] = snapshot->m2h_read_index;
	expected_regs[5] = snapshot->m2h_write_index;
	read_index = snapshot->h2m_read_index;
	ret = a1599_init_mmio_send_once(ops, context, snapshot->h2m_start,
		snapshot->h2m_capacity_words, read_index,
		snapshot->h2m_write_index, expected_regs, words,
		&result->ring_send);
	if (ret)
		return ret;
	result->published = true;
	ret = a1599_init_mmio_wait_consumed(ops, context,
		snapshot->h2m_capacity_words,
		(snapshot->h2m_write_index + ID81_MESSAGE_BYTES / sizeof(a1599_ring_u32)) %
			snapshot->h2m_capacity_words,
		&result->consumer_after_poll);
	if (ret)
		return ret;
	result->consumed = true;
	ret = a1599_m2h_mmio_wait_message(ops, context, snapshot->m2h_start,
		snapshot->m2h_capacity_words, snapshot->m2h_read_index,
		timeout_ms, &result->message);
	if (ret)
		return ret;
	result->response_received = true;
	parse_ret = c0_parse(result->message.bytes, result->message.length,
		expected_fence, &result->response);
	if (parse_ret != C0_OK)
		return -EBADMSG;
	result->response_record_valid = true;
	if (result->response.status & C0_STATUS_DETAIL_BIT)
		result->status_class = A1599_DECODE_C0_STATUS_ERROR_DETAIL;
	else if (result->response.status)
		result->status_class = A1599_DECODE_C0_STATUS_NONZERO;
	else
		result->status_class = A1599_DECODE_C0_STATUS_ZERO;
	result->response_error = result->response.status != 0;
	return result->response_error ? -EREMOTEIO : 0;
}

#endif
