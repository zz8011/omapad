/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef A1599_FW_C0_ACK_H
#define A1599_FW_C0_ACK_H

#include "a1599_fw_decode_submit.h"

struct a1599_c0_ack_state {
	bool attempted;
	bool write_issued;
	bool verified;
	unsigned int after_valid_count;
	a1599_ring_u32 after[6];
};

/* One terminal observation, core 0, only the captured baseline C0. The caller
 * holds the existing ring mutex. Never frees/reuses DMA or grants another job.
 * Apple 12.5.8 VA 0xfffffff008681720..172c writes the advanced word index to
 * 0x2fe8 after copying the message. Additional validation/readback below is our
 * bounded policy, not a claim that Apple performs these same checks.
 */
static inline int a1599_c0_ack_once(
		const struct a1599_init_mmio_ops *ops, void *context,
		const struct a1599_ring_message *saved,
		bool authorized, bool snapshot_ready, bool resources_pinned,
		struct a1599_c0_ack_state *state)
{
	static const a1599_ring_u32 offsets[6] = {
		0x2ff0, 0x2fe4, 0x2ff4, 0x2ffc, 0x2fe8, 0x2fec,
	};
	static const a1599_ring_u32 expected[6] = {
		0x60cc01cc, 0x60000033, 10, 10, 0, 3,
	};
	struct c0_response parsed;
	a1599_ring_u32 value, wanted;
	unsigned int pass, i, j;
	int ret;

	if (!state || !saved || !ops || !ops->read32 || !ops->write32 ||
	    !ops->read_barrier || !ops->publish_barrier)
		return -EINVAL;
	if (!authorized)
		return -EACCES;
	if (!snapshot_ready || !resources_pinned)
		return -ENODATA;
	if (state->attempted)
		return -EALREADY;
	/* No retry even if a pre-write check fails. A posted write followed by a
	 * failed readback must remain distinguishable from no write. */
	state->attempted = true;
	if (saved->length != 12 || saved->read_index != 0 ||
	    saved->write_index != 3 ||
	    c0_parse(saved->bytes, saved->length, 1, &parsed) != C0_OK ||
	    parsed.dispatch_class != C0_DISPATCH_10 || parsed.has_detail)
		return -EBADMSG;
	for (pass = 0; pass < 2; pass++) {
		for (i = 0; i < 6; i++) {
			ret = ops->read32(context, offsets[i], &value);
			if (ret)
				return ret;
			if (value != expected[i])
				return -ESTALE;
		}
		if (pass)
			break;
		ops->read_barrier(context);
		for (i = 0; i < 3; i++) {
			ret = ops->read32(context, 0x8000 + 4 * i, &value);
			if (ret)
				return ret;
			wanted = 0;
			for (j = 0; j < 4; j++)
				wanted |= (a1599_ring_u32)saved->bytes[4 * i + j] << (8 * j);
			if (value != wanted)
				return -ESTALE;
		}
	}
	/* Sole write: consume exactly the three already-copied words. No kick,
	 * IRQ clear, producer write, reset, output store or second ID81. */
	ops->publish_barrier(context);
	state->write_issued = true;
	ops->write32(context, 0x2fe8, 3);
	for (i = 0; i < 6; i++) {
		ret = ops->read32(context, offsets[i], &state->after[i]);
		if (ret)
			return ret;
		state->after_valid_count++;
		wanted = i == 4 ? 3 : expected[i];
		if (state->after[i] != wanted)
			return -ESTALE;
	}
	state->verified = true;
	return 0;
}
#endif
