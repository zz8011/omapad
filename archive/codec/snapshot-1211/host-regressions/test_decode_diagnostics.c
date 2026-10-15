#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../a1599_fw_decode_diagnostics.h"
#include "../../id81_host_contract/id81_contract.h"

static void put_le16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static size_t make_c0(uint8_t *p, size_t length, uint32_t status,
		      uint32_t detail)
{
	memset(p, 0, 16);
	p[0] = (uint8_t)(0x80U | length);
	p[1] = C0_MESSAGE_ID;
	put_le16(p + 2, 1);
	put_le32(p + 8, status);
	put_le32(p + 12, detail);
	return length;
}

static struct a1599_decode_diagnostic capture(const uint8_t *bytes,
					     size_t length,
					     uint16_t expected_fence,
					     bool guard_checked,
					     bool guard_intact,
					     enum c0_result *parse_result)
{
	struct c0_response parsed = {0};
	struct a1599_decode_diagnostic diag;
	enum c0_result result = c0_parse(bytes, length, expected_fence, &parsed);
	bool valid = result == C0_OK;

	*parse_result = result;
	a1599_decode_diagnostic_capture_c0(&diag, bytes, length, valid,
					   guard_checked, guard_intact);
	return diag;
}

static void test_zero_status_records_guard_intact_after_real_check(void)
{
	uint8_t c0[16];
	enum c0_result parsed;
	size_t len = make_c0(c0, 12, 0, 0);
	struct a1599_decode_diagnostic d = capture(c0, len, 1, true, true, &parsed);

	assert(parsed == C0_OK);
	assert(d.actual_length == 12);
	assert(d.raw_length == 12 && memcmp(d.raw, c0, 12) == 0);
	assert(d.last_mb_present && d.last_mb == 0);
	assert(d.status_present && d.status == 0);
	assert(!d.detail_present);
	assert(d.guard_state == A1599_GUARD_INTACT);
}

static void test_error_status_keeps_guard_unchecked_and_captures_raw_fields(void)
{
	uint8_t c0[16];
	enum c0_result parsed;
	size_t len = make_c0(c0, 16, 0x48064455U, 0);
	c0[4] = 0x34;
	c0[5] = 0x12;
	struct a1599_decode_diagnostic d = capture(c0, len, 1, true, false, &parsed);

	assert(parsed == C0_OK);
	assert(d.actual_length == 16);
	assert(d.raw_length == 16 && memcmp(d.raw, c0, 16) == 0);
	assert(d.last_mb_present && d.last_mb == 0x1234);
	assert(d.status_present && d.status == 0x48064455U);
	assert(d.detail_present && d.detail == 0);
	assert(d.guard_state == A1599_GUARD_NOT_CHECKED);
}

static void test_truncated_c0_reports_only_fields_wholly_present(void)
{
	uint8_t c0[16];
	enum c0_result parsed;
	size_t len = make_c0(c0, 12, 0, 0);
	struct a1599_decode_diagnostic d = capture(c0, 10, 1, true, false, &parsed);

	assert(len == 12);
	assert(parsed == C0_TRUNCATED_MESSAGE);
	assert(d.actual_length == 10);
	assert(d.raw_length == 10 && memcmp(d.raw, c0, 10) == 0);
	assert(d.last_mb_present);
	assert(!d.status_present && !d.detail_present);
	assert(d.guard_state == A1599_GUARD_NOT_CHECKED);
}

static void test_short_error_detail_has_status_but_no_detail_field(void)
{
	uint8_t c0[16];
	enum c0_result parsed;
	size_t len = make_c0(c0, 12, C0_STATUS_DETAIL_BIT | 0x455U, 0);
	struct a1599_decode_diagnostic d = capture(c0, len, 1, true, false, &parsed);

	assert(parsed == C0_BAD_LENGTH);
	assert(d.actual_length == 12);
	assert(d.last_mb_present);
	assert(d.status_present && d.status == (C0_STATUS_DETAIL_BIT | 0x455U));
	assert(!d.detail_present);
	assert(d.guard_state == A1599_GUARD_NOT_CHECKED);
}

static void test_successful_response_can_report_guard_changed_only_after_check(void)
{
	uint8_t c0[16];
	enum c0_result parsed;
	size_t len = make_c0(c0, 12, 0, 0);
	struct a1599_decode_diagnostic d = capture(c0, len, 1, true, false, &parsed);

	assert(parsed == C0_OK);
	assert(d.guard_state == A1599_GUARD_CHANGED);
}

static void test_absent_response_is_unchecked(void)
{
	enum c0_result parsed;
	struct a1599_decode_diagnostic d = capture(NULL, 0, 1, true, false, &parsed);

	assert(parsed == C0_BAD_ARGUMENT);
	assert(d.actual_length == 0);
	assert(d.raw_length == 0);
	assert(!d.status_present && !d.detail_present);
	assert(d.guard_state == A1599_GUARD_NOT_CHECKED);
}

static void test_nonzero_class10_reports_only_actual_guard_observation(void)
{
	uint8_t c0[16];
	enum c0_result parsed;
	size_t len = make_c0(c0, 12, 0x600644d5U, 0);
	struct a1599_decode_diagnostic d = capture(c0, len, 1, true, true, &parsed);
	assert(parsed == C0_OK && d.guard_state == A1599_GUARD_INTACT);
	d = capture(c0, len, 1, true, false, &parsed);
	assert(d.guard_state == A1599_GUARD_CHANGED);
	d = capture(c0, len, 1, false, true, &parsed);
	assert(d.guard_state == A1599_GUARD_NOT_CHECKED);
}

int main(void)
{
	test_zero_status_records_guard_intact_after_real_check();
	test_error_status_keeps_guard_unchecked_and_captures_raw_fields();
	test_truncated_c0_reports_only_fields_wholly_present();
	test_short_error_detail_has_status_but_no_detail_field();
	test_successful_response_can_report_guard_changed_only_after_check();
	test_absent_response_is_unchecked();
	test_nonzero_class10_reports_only_actual_guard_observation();
	puts("decode diagnostic fixtures: PASS");
	return 0;
}
