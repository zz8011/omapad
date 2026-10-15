// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../a1599_fw_decode_lifetime.h"

struct fake_dma {
	void *cpu[A1599_DECODE_RESOURCE_COUNT];
	unsigned long long dma[A1599_DECODE_RESOURCE_COUNT];
	unsigned int live;
	unsigned int freed;
	int fail_at;
	int overlap_at;
};

static int fake_alloc(void *context, unsigned int index, size_t bytes,
		      struct a1599_decode_dma_view *view)
{
	struct fake_dma *fake = context;
	unsigned int i;

	if ((int)index == fake->fail_at)
		return -ENOMEM;
	view->cpu = malloc(bytes);
	if (!view->cpu)
		return -ENOMEM;
	memset(view->cpu, 0xa5, bytes);
	view->dma = (int)index == fake->overlap_at ? 0x100000 :
		0x100000 + (unsigned long long)index * 0x20000;
	view->bytes = bytes;
	for (i = 0; i < A1599_DECODE_RESOURCE_COUNT; i++) {
		if (fake->cpu[i])
			continue;
		fake->cpu[i] = view->cpu;
		fake->dma[i] = view->dma;
		fake->live++;
		break;
	}
	return 0;
}

static void fake_free(void *context, unsigned int index,
		      struct a1599_decode_dma_view *view)
{
	struct fake_dma *fake = context;

	(void)index;
	if (!view->cpu)
		return;
	free(view->cpu);
	view->cpu = NULL;
	fake->live--;
	fake->freed++;
}

static const struct a1599_decode_dma_ops fake_ops = {
	.alloc = fake_alloc,
	.free = fake_free,
};

int main(void)
{
	struct a1599_decode_dma_set set = {};
	struct fake_dma fake = {.fail_at = -1, .overlap_at = -1};
	unsigned int i;

	assert(a1599_decode_dma_allocate(&set, &fake_ops, &fake) == 0);
	assert(set.allocated_count == A1599_DECODE_RESOURCE_COUNT);
	assert(fake.live == A1599_DECODE_RESOURCE_COUNT);
	for (i = 0; i < set.allocated_count; i++) {
		unsigned char *p = set.buffers[i].cpu;
		size_t j;
		assert(set.buffers[i].bytes == a1599_decode_dma_sizes[i]);
		for (j = 0; j < set.buffers[i].bytes; j++)
			assert(p[j] == 0);
	}
	assert(a1599_decode_dma_mark_started(&set) == 0);
	assert(a1599_decode_dma_release(&set, &fake_ops, &fake) == -EBUSY);
	assert(fake.live == A1599_DECODE_RESOURCE_COUNT);
	for (i = 0; i < set.allocated_count; i++)
		fake_free(&fake, i, &set.buffers[i]);
	set.allocated_count = 0;

	memset(&set, 0, sizeof(set));
	memset(&fake, 0, sizeof(fake));
	fake.fail_at = -1;
	fake.overlap_at = -1;
	assert(a1599_decode_dma_allocate(&set, &fake_ops, &fake) == 0);
	assert(a1599_decode_dma_release(&set, &fake_ops, &fake) == 0);
	assert(fake.live == 0 && fake.freed == A1599_DECODE_RESOURCE_COUNT);

	memset(&set, 0, sizeof(set));
	memset(&fake, 0, sizeof(fake));
	fake.fail_at = 3;
	fake.overlap_at = -1;
	assert(a1599_decode_dma_allocate(&set, &fake_ops, &fake) == -ENOMEM);
	assert(set.allocated_count == 0 && fake.live == 0);
	assert(fake.freed == 3);

	memset(&set, 0, sizeof(set));
	memset(&fake, 0, sizeof(fake));
	fake.fail_at = -1;
	fake.overlap_at = 1;
	assert(a1599_decode_dma_allocate(&set, &fake_ops, &fake) == -EADDRINUSE);
	assert(set.allocated_count == 0 && fake.live == 0);
	assert(fake.freed == 2);

	puts("decode DMA lifetime mock tests: PASS");
	return 0;
}
