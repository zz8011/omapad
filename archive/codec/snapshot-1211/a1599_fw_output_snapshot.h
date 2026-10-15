/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef A1599_FW_OUTPUT_SNAPSHOT_H
#define A1599_FW_OUTPUT_SNAPSHOT_H

#include "a1599_fw_decode_submit.h"

#define A1599_OUTPUT_SNAPSHOT_BYTES 4096U
#define A1599_OUTPUT_GUARD_START 3072U
#define A1599_OUTPUT_SAMPLE_INTERVAL_MS 50U

struct a1599_output_snapshot_input {
	const struct a1599_decode_submit_result *response;
	const uint8_t *cpu;
	size_t bytes;
	bool authorized;
	/* Verified by the caller against the retained dma_alloc_coherent record,
	 * not dev_is_dma_coherent(): a non-coherent device can use this API. */
	bool coherent_allocation;
	bool resources_pinned;
};

struct a1599_output_snapshot_ops {
	int (*read_ring)(void *context, struct a1599_decode_ring_snapshot *ring);
	void (*read_barrier)(void *context);
	void (*wait_ms)(void *context, unsigned int milliseconds);
};

struct a1599_output_snapshot_result {
	bool eligible;
	bool guard_checked;
	bool guard_intact;
	bool stability_checked;
	bool stable;
	bool snapshot_ready;
	unsigned int copies;
	unsigned int ring_checks;
};

static inline bool a1599_output_ring_is_single_c0(
		const struct a1599_decode_ring_snapshot *s)
{
	return s->h2m_descriptor == 0x60cc01cc &&
		s->m2h_descriptor == 0x60000033 &&
		s->h2m_capacity_words == 460 && s->m2h_capacity_words == 51 &&
		s->h2m_start == 0x80cc && s->m2h_start == 0x8000 &&
		s->h2m_read_index == 10 && s->h2m_write_index == 10 &&
		s->m2h_read_index == 0 && s->m2h_write_index == 3;
}

static inline bool a1599_output_copy_guard_intact(const uint8_t *copy)
{
	unsigned int i;

	for (i = A1599_OUTPUT_GUARD_START; i < A1599_OUTPUT_SNAPSHOT_BYTES; i++)
		if (copy[i] != 0xa5)
			return false;
	return true;
}

static inline int a1599_output_check_ring(
		const struct a1599_output_snapshot_ops *ops, void *context,
		struct a1599_output_snapshot_result *result)
{
	struct a1599_decode_ring_snapshot ring;
	int ret;

	memset(&ring, 0, sizeof(ring));
	result->ring_checks++;
	ret = ops->read_ring(context, &ring);
	if (ret)
		return ret;
	return a1599_output_ring_is_single_c0(&ring) ? 0 : -ESTALE;
}

/* One diagnostic observation: at most two read-only 4-KiB copies, one 50-ms
 * interval and three six-register ring observations. No write/unmap/free or
 * consumer advance. Coherent allocation permits CPU/device parallel access;
 * equality is bounded observation, not an assertion of future DMA quiescence
 * or frame correctness. The caller publishes copy_a only after return 0 using
 * release/acquire synchronization, and keeps both copies immutable thereafter.
 */
static inline int a1599_output_snapshot_once(
		const struct a1599_output_snapshot_input *in,
		const struct a1599_output_snapshot_ops *ops, void *context,
		uint8_t *copy_a, uint8_t *copy_b, size_t copy_bytes,
		struct a1599_output_snapshot_result *out)
{
	const struct a1599_decode_submit_result *r;
	int ret;

	if (!out)
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	if (!in || !ops || !ops->read_ring || !ops->read_barrier ||
	    !ops->wait_ms || !copy_a || !copy_b || copy_a == copy_b ||
	    copy_bytes != A1599_OUTPUT_SNAPSHOT_BYTES)
		return -EINVAL;
	if (!in->authorized)
		return -EACCES;
	if (!in->coherent_allocation || !in->resources_pinned || !in->cpu ||
	    in->bytes != A1599_OUTPUT_SNAPSHOT_BYTES ||
	    copy_a == in->cpu || copy_b == in->cpu)
		return -EINVAL;
	r = in->response;
	if (!r || !r->published || !r->consumed || !r->response_received ||
	    !r->response_record_valid || r->response_error ||
	    r->status_class != A1599_DECODE_C0_CLASS_10 ||
	    r->response.dispatch_class != C0_DISPATCH_10 ||
	    (r->response.status & C0_STATUS_DETAIL_BIT) ||
	    r->response.has_detail || r->response.fence != 1 ||
	    r->message.length != 12 || r->message.read_index != 0 ||
	    r->message.write_index != 3)
		return -ENODATA;
	/* word4 (historical LastMB=3) is deliberately not a bound or completion
	 * predicate. The raw response and native dispatch remain separate facts. */
	ret = a1599_output_check_ring(ops, context, out);
	if (ret)
		return ret;
	out->eligible = true;
	ops->read_barrier(context);
	memcpy(copy_a, in->cpu, A1599_OUTPUT_SNAPSHOT_BYTES);
	out->copies = 1;
	out->guard_checked = true;
	out->guard_intact = a1599_output_copy_guard_intact(copy_a);
	if (!out->guard_intact)
		return -EOVERFLOW;
	ops->wait_ms(context, A1599_OUTPUT_SAMPLE_INTERVAL_MS);
	ret = a1599_output_check_ring(ops, context, out);
	if (ret)
		return ret;
	ops->read_barrier(context);
	memcpy(copy_b, in->cpu, A1599_OUTPUT_SNAPSHOT_BYTES);
	out->copies = 2;
	out->guard_intact = a1599_output_copy_guard_intact(copy_b);
	if (!out->guard_intact)
		return -EOVERFLOW;
	out->stability_checked = true;
	out->stable = !memcmp(copy_a, copy_b, A1599_OUTPUT_SNAPSHOT_BYTES);
	if (!out->stable)
		return -ESTALE;
	ret = a1599_output_check_ring(ops, context, out);
	if (ret)
		return ret;
	out->snapshot_ready = true;
	return 0;
}

#endif
