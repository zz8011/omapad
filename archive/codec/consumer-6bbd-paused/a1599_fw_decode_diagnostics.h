/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef A1599_FW_DECODE_DIAGNOSTICS_H
#define A1599_FW_DECODE_DIAGNOSTICS_H

#ifdef __KERNEL__
#include <linux/types.h>
typedef u8 a1599_diag_u8;
typedef u16 a1599_diag_u16;
typedef u32 a1599_diag_u32;
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint8_t a1599_diag_u8;
typedef uint16_t a1599_diag_u16;
typedef uint32_t a1599_diag_u32;
#endif

#include "../id81_host_contract/id81_contract.h"

#define A1599_C0_DIAGNOSTIC_MAX_BYTES 127U

enum a1599_guard_state {
	A1599_GUARD_NOT_CHECKED,
	A1599_GUARD_INTACT,
	A1599_GUARD_CHANGED,
};

struct a1599_decode_diagnostic {
	size_t actual_length;
	size_t raw_length;
	a1599_diag_u8 raw[A1599_C0_DIAGNOSTIC_MAX_BYTES];
	a1599_diag_u16 last_mb;
	a1599_diag_u32 status;
	a1599_diag_u32 detail;
	bool record_valid;
	bool last_mb_present;
	bool status_present;
	bool detail_present;
	enum a1599_guard_state guard_state;
};

static inline a1599_diag_u32 a1599_diag_read_le32(const a1599_diag_u8 *p)
{
	return (a1599_diag_u32)p[0] |
		((a1599_diag_u32)p[1] << 8) |
		((a1599_diag_u32)p[2] << 16) |
		((a1599_diag_u32)p[3] << 24);
}

static inline a1599_diag_u16 a1599_diag_read_le16(const a1599_diag_u8 *p)
{
	return (a1599_diag_u16)p[0] | ((a1599_diag_u16)p[1] << 8);
}

/* Field presence describes bytes present in the captured C0, even when the
 * C0 parser rejected the record. Guard state is separate: only a valid
 * native class-0x10 response followed by an actual guard observation can
 * establish it; nonzero flags alone do not suppress an observation.
 */
static inline void a1599_decode_diagnostic_capture_c0(
		struct a1599_decode_diagnostic *out,
		const a1599_diag_u8 *message, size_t actual_length,
		bool record_valid, bool guard_checked, bool guard_intact)
{
	size_t i;

	if (!out)
		return;
	*out = (struct a1599_decode_diagnostic) {
		.actual_length = actual_length,
		.record_valid = record_valid,
		.guard_state = A1599_GUARD_NOT_CHECKED,
	};
	if (!message)
		return;
	out->raw_length = actual_length;
	if (out->raw_length > sizeof(out->raw))
		out->raw_length = sizeof(out->raw);
	for (i = 0; i < out->raw_length; i++)
		out->raw[i] = message[i];
	if (actual_length >= 6) {
		out->last_mb_present = true;
		out->last_mb = a1599_diag_read_le16(message + 4);
	}
	if (actual_length >= 12) {
		out->status_present = true;
		out->status = a1599_diag_read_le32(message + 8);
	}
	if (actual_length >= 16) {
		out->detail_present = true;
		out->detail = a1599_diag_read_le32(message + 12);
	}
	if (record_valid && out->status_present && !(out->status & C0_STATUS_DETAIL_BIT) &&
	    guard_checked)
		out->guard_state = guard_intact ? A1599_GUARD_INTACT :
			A1599_GUARD_CHANGED;
}

static inline const char *a1599_guard_state_name(enum a1599_guard_state state)
{
	switch (state) {
	case A1599_GUARD_INTACT:
		return "intact";
	case A1599_GUARD_CHANGED:
		return "changed";
	case A1599_GUARD_NOT_CHECKED:
	default:
		return "not_checked";
	}
}

#endif
