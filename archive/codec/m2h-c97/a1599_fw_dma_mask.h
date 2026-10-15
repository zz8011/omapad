/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef A1599_FW_DMA_MASK_H
#define A1599_FW_DMA_MASK_H

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/types.h>
typedef u64 a1599_dma_u64;
#else
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
typedef uint64_t a1599_dma_u64;
#endif

#define A1599_FW_DMA_DEFAULT_MASK 0xffffffffULL
#define A1599_FW_DMA_ALLOC_MASK 0xfffffffffULL

struct a1599_fw_dma_probe_snapshot {
	a1599_dma_u64 streaming_mask;
	a1599_dma_u64 coherent_mask;
	bool device_coherent;
	bool has_dma_range_map;
	bool has_iommu_domain;
};

struct a1599_fw_dma_mask_snapshot {
	a1599_dma_u64 streaming_mask;
	a1599_dma_u64 coherent_mask;
};

struct a1599_fw_dma_mask_ops {
	int (*set_pair)(void *context, a1599_dma_u64 mask);
	int (*set_streaming)(void *context, a1599_dma_u64 mask);
	int (*set_coherent)(void *context, a1599_dma_u64 mask);
};

static inline int a1599_fw_dma_probe_validate(
		const struct a1599_fw_dma_probe_snapshot *snapshot)
{
	if (!snapshot ||
	    snapshot->streaming_mask != A1599_FW_DMA_DEFAULT_MASK ||
	    snapshot->coherent_mask != A1599_FW_DMA_DEFAULT_MASK ||
	    snapshot->device_coherent || snapshot->has_dma_range_map ||
	    snapshot->has_iommu_domain)
		return -EOPNOTSUPP;
	return 0;
}

static inline int a1599_fw_dma_mask_restore(
		const struct a1599_fw_dma_mask_ops *ops, void *context,
		const struct a1599_fw_dma_mask_snapshot *snapshot)
{
	int streaming_ret, coherent_ret;

	if (!ops || !ops->set_streaming || !ops->set_coherent || !snapshot)
		return -EINVAL;
	streaming_ret = ops->set_streaming(context, snapshot->streaming_mask);
	coherent_ret = ops->set_coherent(context, snapshot->coherent_mask);
	return streaming_ret ? streaming_ret : coherent_ret;
}

/* Temporarily widen only the DMA API allocation mask. Restore a partially
 * changed pair if the platform's set-pair operation fails.
 */
static inline int a1599_fw_dma_mask_begin(
		const struct a1599_fw_dma_mask_ops *ops, void *context,
		a1599_dma_u64 streaming_mask, a1599_dma_u64 coherent_mask,
		a1599_dma_u64 allocation_mask,
		struct a1599_fw_dma_mask_snapshot *snapshot,
		bool *restore_failed)
{
	int ret, restore_ret;

	if (!ops || !ops->set_pair || !ops->set_streaming ||
	    !ops->set_coherent || !snapshot || !restore_failed ||
	    !streaming_mask || !coherent_mask || !allocation_mask)
		return -EINVAL;
	snapshot->streaming_mask = streaming_mask;
	snapshot->coherent_mask = coherent_mask;
	*restore_failed = false;
	ret = ops->set_pair(context, allocation_mask);
	if (!ret)
		return 0;
	restore_ret = a1599_fw_dma_mask_restore(ops, context, snapshot);
	if (restore_ret) {
		*restore_failed = true;
		return restore_ret;
	}
	return ret;
}

#endif
