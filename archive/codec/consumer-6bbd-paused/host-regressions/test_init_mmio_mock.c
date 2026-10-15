// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../a1599_fw_ring_capacity.h"

struct fake_mmio {
	a1599_ring_u32 regs[0x3000 / 4];
	a1599_ring_u32 payload[8];
	a1599_ring_u64 now_ns;
	unsigned int payload_writes;
	unsigned int producer_writes;
	unsigned int kicks;
	unsigned int sleeps;
	unsigned int advance_after_sleeps;
	bool ignore_producer_write;
};

static int fake_read(void *context, a1599_ring_u32 offset,
		     a1599_ring_u32 *value)
{
	struct fake_mmio *f = context;

	if (offset == 0x2ff4 && f->kicks && f->sleeps >= f->advance_after_sleeps)
		*value = 5;
	else
		*value = f->regs[offset / 4];
	return 0;
}

static void fake_write(void *context, a1599_ring_u32 offset,
		       a1599_ring_u32 value)
{
	struct fake_mmio *f = context;

	if (offset >= 0x80cc && offset < 0x80cc + 5 * 4) {
		f->payload[(offset - 0x80cc) / 4] = value;
		f->payload_writes++;
	} else if (offset == 0x2ffc) {
		f->producer_writes++;
		if (!f->ignore_producer_write)
			f->regs[offset / 4] = value;
	} else if (offset == 0x80) {
		f->kicks++;
	} else {
		f->regs[offset / 4] = value;
	}
}

static void fake_barrier(void *context) { (void)context; }
static void fake_read_barrier(void *context) { (void)context; }
static a1599_ring_u64 fake_time_ns(void *context)
{
	return ((struct fake_mmio *)context)->now_ns;
}
static void fake_sleep(void *context)
{
	struct fake_mmio *f = context;

	f->now_ns += 1000000;
	f->sleeps++;
}

static const struct a1599_init_mmio_ops fake_ops = {
	.read32 = fake_read,
	.write32 = fake_write,
	.publish_barrier = fake_barrier,
	.read_barrier = fake_read_barrier,
	.time_ns = fake_time_ns,
	.sleep_1_1_5_ms = fake_sleep,
};

static const a1599_ring_u32 expected[6] = {
	0x60cc01cc, 0x60000033, 0, 0, 0, 0,
};
static const a1599_ring_u32 words[5] = {
	0x8014, 0x40100000, 0x40300000, 0x00400100, 0x40340000,
};

static void init_fake(struct fake_mmio *f)
{
	memset(f, 0, sizeof(*f));
	f->regs[0x2ff0 / 4] = expected[0];
	f->regs[0x2fe4 / 4] = expected[1];
	f->advance_after_sleeps = 3;
}

static void test_changed_descriptor_rejects_before_writes(void)
{
	struct fake_mmio f;
	struct a1599_init_mmio_result result;

	init_fake(&f);
	f.regs[0x2ff0 / 4]++;
	assert(a1599_init_mmio_send_once(&fake_ops, &f, 0x80cc, 460, 0, 0,
		       expected, words, &result) == -ESTALE);
	assert(f.payload_writes == 0 && f.producer_writes == 0 && f.kicks == 0);
}

static void test_producer_readback_mismatch_suppresses_kick(void)
{
	struct fake_mmio f;
	struct a1599_init_mmio_result result;

	init_fake(&f);
	f.ignore_producer_write = true;
	assert(a1599_init_mmio_send_once(&fake_ops, &f, 0x80cc, 460, 0, 0,
		       expected, words, &result) == -EIO);
	assert(f.payload_writes == 5 && f.producer_writes == 1 && f.kicks == 0);
}

static void test_consumer_progress_after_single_kick(void)
{
	struct fake_mmio f;
	struct a1599_init_mmio_result result;
	a1599_ring_u32 observed = 0;

	init_fake(&f);
	assert(a1599_init_mmio_send_once(&fake_ops, &f, 0x80cc, 460, 0, 0,
		       expected, words, &result) == 0);
	assert(result.kicked && f.kicks == 1 && f.payload_writes == 5);
	assert(a1599_init_mmio_wait_consumed(&fake_ops, &f, 460, 5,
		       &observed) == 0);
	assert(observed == 5 && f.sleeps == 3 && f.kicks == 1);
}

static void test_consumer_timeout_is_bounded_single_shot(void)
{
	struct fake_mmio f;
	struct a1599_init_mmio_result result;
	a1599_ring_u32 observed = 0;

	init_fake(&f);
	f.advance_after_sleeps = 3000;
	assert(a1599_init_mmio_send_once(&fake_ops, &f, 0x80cc, 460, 0, 0,
		       expected, words, &result) == 0);
	assert(a1599_init_mmio_wait_consumed(&fake_ops, &f, 460, 5,
		       &observed) == -ETIMEDOUT);
	assert(f.now_ns == 2000000000ULL && f.kicks == 1 && f.payload_writes == 5);
}

int main(void)
{
	test_changed_descriptor_rejects_before_writes();
	test_producer_readback_mismatch_suppresses_kick();
	test_consumer_progress_after_single_kick();
	test_consumer_timeout_is_bounded_single_shot();
	puts("production MMIO send path fake-backend checks passed (4 scenarios)");
	return 0;
}
