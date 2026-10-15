// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../a1599_fw_decode_submit.h"

typedef a1599_ring_u32 u32;
typedef a1599_ring_u64 u64;

struct fake_device {
	u32 words[0x9000 / sizeof(u32)];
	u64 now_ns;
	unsigned int kicks;
	unsigned int read_barriers;
	unsigned int payload_writes;
	bool no_response;
	bool wrong_fence;
	bool status_error;
	bool status_nonzero;
};

static int fake_read32(void *context, u32 offset, u32 *value)
{
	struct fake_device *fake = context;
	if (!value || offset % sizeof(u32) || offset >= sizeof(fake->words))
		return -EINVAL;
	*value = fake->words[offset / sizeof(u32)];
	return 0;
}

static void fake_write32(void *context, u32 offset, u32 value)
{
	struct fake_device *fake = context;
	assert(offset % sizeof(u32) == 0);
	assert(offset < sizeof(fake->words));
	if (offset >= 0x80cc && offset < 0x80cc + 5 * sizeof(u32))
		fake->payload_writes++;
	fake->words[offset / sizeof(u32)] = value;
	if (offset != 0x80)
		return;
	fake->kicks++;
	fake->words[0x2ff4 / sizeof(u32)] =
		fake->words[0x2ffc / sizeof(u32)];
	if (fake->no_response)
		return;
	{
		u32 fence = fake->wrong_fence ? 2U : 1U;
		fake->words[0x8000 / sizeof(u32)] =
			(fake->status_error ? 0x0000c010U : 0x0000c00cU) |
			(fence << 16);
		fake->words[0x8004 / sizeof(u32)] = 0;
		fake->words[0x8008 / sizeof(u32)] = fake->status_error ?
			C0_STATUS_DETAIL_BIT : (fake->status_nonzero ? 1U : 0U);
		fake->words[0x800c / sizeof(u32)] = fake->status_error ? 0xdeadbeefU : 0;
		fake->words[0x2fec / sizeof(u32)] = fake->status_error ? 4 : 3;
	}
}

static void fake_write_barrier(void *context) { (void)context; }
static void fake_read_barrier(void *context)
{
	((struct fake_device *)context)->read_barriers++;
}
static u64 fake_time_ns(void *context)
{
	return ((struct fake_device *)context)->now_ns;
}
static void fake_sleep(void *context)
{
	((struct fake_device *)context)->now_ns += 1000000ULL;
}

static const struct a1599_init_mmio_ops fake_ops = {
	.read32 = fake_read32,
	.write32 = fake_write32,
	.publish_barrier = fake_write_barrier,
	.read_barrier = fake_read_barrier,
	.time_ns = fake_time_ns,
	.sleep_1_1_5_ms = fake_sleep,
};

static struct a1599_decode_ring_snapshot snapshot(void)
{
	return (struct a1599_decode_ring_snapshot) {
		.h2m_descriptor = 0x60cc01cc,
		.m2h_descriptor = 0x60000033,
		.h2m_capacity_words = 460,
		.m2h_capacity_words = 51,
		.h2m_start = 0x80cc,
		.m2h_start = 0x8000,
	};
}

static struct id81_pack_input pack(void)
{
	return (struct id81_pack_input) {
		.preserve_byte0_bit7 = 0,
		.group_sequence = 1,
		.flags = 0x4015,
		.command_iova = 0x4002a000,
		.command_bytes = 544,
		.mapping_iova = 0x4002a000,
		.mapping_bytes = 4096,
		.root_device_address = 0x1234000000ULL,
		.selector = 0,
		.opaque = 0x10211000,
	};
}

static void init_fake(struct fake_device *fake)
{
	memset(fake, 0, sizeof(*fake));
	fake->words[0x2ff0 / sizeof(u32)] = 0x60cc01cc;
	fake->words[0x2fe4 / sizeof(u32)] = 0x60000033;
}

int main(void)
{
	struct fake_device fake;
	struct a1599_decode_submit_result result;
	struct a1599_decode_ring_snapshot s;
	struct id81_pack_input p;
	int ret;

	init_fake(&fake);
	s = snapshot();
	p = pack();
	ret = a1599_decode_submit_once(&fake_ops, &fake, &s, &p, 1, 5,
		&result);
	assert(ret == 0);
	assert(fake.kicks == 1 && fake.payload_writes == 5);
	assert(result.published && result.consumed && result.response_received);
	assert(result.response.fence == 1 && result.response.status == 0);
	assert(result.response_record_valid && !result.response_error);
	assert(result.status_class == A1599_DECODE_C0_STATUS_ZERO);
	assert(result.envelope[0] == 0x14 && result.envelope[1] == 0x81);
	assert(result.envelope[2] == 1 && result.envelope[6] == 136);
	assert(result.envelope[4] == 0x15 && result.envelope[5] == 0x40);
	assert(result.envelope[12] == 0 && result.envelope[13] == 0 &&
	       result.envelope[14] == 0x34 && result.envelope[15] == 0x12);
	assert(result.envelope[16] == 0 && result.envelope[17] == 0x10 &&
	       result.envelope[18] == 0x21 && result.envelope[19] == 0x10);
	assert(fake.read_barriers == 1);
	assert(fake.words[0x2fe8 / sizeof(u32)] == 0);

	init_fake(&fake);
	fake.wrong_fence = true;
	s = snapshot();
	p = pack();
	assert(a1599_decode_submit_once(&fake_ops, &fake, &s, &p, 1, 5,
		&result) == -EBADMSG);
	assert(result.published && result.consumed && result.response_received);
	assert(fake.kicks == 1);

	init_fake(&fake);
	fake.status_error = true;
	s = snapshot();
	p = pack();
	assert(a1599_decode_submit_once(&fake_ops, &fake, &s, &p, 1, 5,
		&result) == -EREMOTEIO);
	assert(result.response_received && result.response_record_valid &&
	       result.response_error && result.response.has_detail);
	assert(result.response.detail == 0xdeadbeefU);
	assert(result.status_class == A1599_DECODE_C0_STATUS_ERROR_DETAIL);
	assert(fake.kicks == 1);

	init_fake(&fake);
	fake.status_nonzero = true;
	s = snapshot();
	p = pack();
	assert(a1599_decode_submit_once(&fake_ops, &fake, &s, &p, 1, 5,
		&result) == -EREMOTEIO);
	assert(result.response_record_valid && result.response_error &&
	       !result.response.has_detail);
	assert(result.status_class == A1599_DECODE_C0_STATUS_NONZERO);
	assert(fake.kicks == 1);

	init_fake(&fake);
	fake.no_response = true;
	s = snapshot();
	p = pack();
	assert(a1599_decode_submit_once(&fake_ops, &fake, &s, &p, 1, 2,
		&result) == -ETIMEDOUT);
	assert(result.published && result.consumed && !result.response_received);
	assert(fake.kicks == 1 && fake.words[0x2fe8 / sizeof(u32)] == 0);

	init_fake(&fake);
	s = snapshot();
	s.h2m_read_index = 1;
	p = pack();
	assert(a1599_decode_submit_once(&fake_ops, &fake, &s, &p, 1, 2,
		&result) == -EBUSY);
	assert(fake.kicks == 0);
	puts("ID81 one-shot send/C0 MMIO mock tests: PASS (zero/nonzero/error-detail status, wrong fence, timeout, preflight)");
	return 0;
}
