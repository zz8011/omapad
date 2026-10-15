// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../a1599_fw_dma_mask.h"

struct fake_masks {
	a1599_dma_u64 streaming;
	a1599_dma_u64 coherent;
	unsigned int pair_sets;
	unsigned int stream_sets;
	unsigned int coherent_sets;
	bool fail_pair_after_stream;
};

static int fake_set_pair(void *ctx, a1599_dma_u64 mask)
{
	struct fake_masks *f = ctx;
	f->pair_sets++;
	f->streaming = mask;
	if (f->fail_pair_after_stream)
		return -EIO;
	f->coherent = mask;
	return 0;
}

static int fake_set_stream(void *ctx, a1599_dma_u64 mask)
{
	struct fake_masks *f = ctx;
	f->stream_sets++;
	f->streaming = mask;
	return 0;
}

static int fake_set_coherent(void *ctx, a1599_dma_u64 mask)
{
	struct fake_masks *f = ctx;
	f->coherent_sets++;
	f->coherent = mask;
	return 0;
}

static const struct a1599_fw_dma_mask_ops fake_ops = {
	.set_pair = fake_set_pair,
	.set_streaming = fake_set_stream,
	.set_coherent = fake_set_coherent,
};

int main(void)
{
	struct a1599_fw_dma_probe_snapshot probe = {
		.streaming_mask = A1599_FW_DMA_DEFAULT_MASK,
		.coherent_mask = A1599_FW_DMA_DEFAULT_MASK,
		.device_coherent = false,
		.has_dma_range_map = false,
		.has_iommu_domain = false,
	};
	struct a1599_fw_dma_mask_snapshot saved;
	struct fake_masks f = {
		.streaming = A1599_FW_DMA_DEFAULT_MASK,
		.coherent = A1599_FW_DMA_DEFAULT_MASK,
	};
	bool restore_failed = false;

	assert(a1599_fw_dma_probe_validate(&probe) == 0);
	probe.streaming_mask = A1599_FW_DMA_ALLOC_MASK;
	assert(a1599_fw_dma_probe_validate(&probe) == -EOPNOTSUPP);
	probe.streaming_mask = A1599_FW_DMA_DEFAULT_MASK;
	probe.device_coherent = true;
	assert(a1599_fw_dma_probe_validate(&probe) == -EOPNOTSUPP);
	probe.device_coherent = false;
	probe.has_dma_range_map = true;
	assert(a1599_fw_dma_probe_validate(&probe) == -EOPNOTSUPP);
	probe.has_dma_range_map = false;
	probe.has_iommu_domain = true;
	assert(a1599_fw_dma_probe_validate(&probe) == -EOPNOTSUPP);
	probe.has_iommu_domain = false;

	assert(a1599_fw_dma_mask_begin(&fake_ops, &f, f.streaming, f.coherent,
		A1599_FW_DMA_ALLOC_MASK, &saved, &restore_failed) == 0);
	assert(!restore_failed && f.streaming == A1599_FW_DMA_ALLOC_MASK &&
	       f.coherent == A1599_FW_DMA_ALLOC_MASK);
	assert(a1599_fw_dma_mask_restore(&fake_ops, &f, &saved) == 0);
	assert(f.streaming == A1599_FW_DMA_DEFAULT_MASK &&
	       f.coherent == A1599_FW_DMA_DEFAULT_MASK);

	f = (struct fake_masks) {
		.streaming = A1599_FW_DMA_DEFAULT_MASK,
		.coherent = A1599_FW_DMA_DEFAULT_MASK,
		.fail_pair_after_stream = true,
	};
	restore_failed = false;
	assert(a1599_fw_dma_mask_begin(&fake_ops, &f, f.streaming, f.coherent,
		A1599_FW_DMA_ALLOC_MASK, &saved, &restore_failed) == -EIO);
	assert(!restore_failed && f.streaming == A1599_FW_DMA_DEFAULT_MASK &&
	       f.coherent == A1599_FW_DMA_DEFAULT_MASK);
	puts("DMA probe and mask transaction tests: PASS (32-bit defaults, 36-bit allocation, restore, partial-set rollback)");
	return 0;
}
