// SPDX-License-Identifier: GPL-2.0-only
/* Static one-shot boot memory builder. Never performs device I/O. */
#include <linux/build_bug.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/overflow.h>
#include <linux/sizes.h>
#include <linux/string.h>

#include "a1599_fw_dma_mask.h"
#include "a1599_fw_static_boot_memory.h"

#define VXD_PTE_ADDR_MASK GENMASK_ULL(39, 12)
#define VXD_ROOT_ALLOC_BYTES (2 * A1599_FW_STATIC_ROOT_ALIGN)

static bool evidence_allowed(enum a1599_fw_evidence_state state, bool allow_experimental)
{
	return state == A1599_FW_EVIDENCE_REVIEWED ||
		(allow_experimental && state == A1599_FW_EVIDENCE_EXPERIMENTAL_ASSUMPTION);
}

static int validate_route(const struct a1599_fw_static_route *route,
			  bool allow_experimental)
{
	u64 mask;

	if (!route || !evidence_allowed(route->dma_route, allow_experimental) ||
	    !evidence_allowed(route->pte_dma_domain, allow_experimental))
		return -EOPNOTSUPP;
	mask = route->dma_addr_mask;
	if (!mask || mask > (VXD_PTE_ADDR_MASK | (SZ_4K - 1)) ||
	    (mask & (mask + 1)))
		return -ERANGE;
	return 0;
}

static int validate_span(const struct a1599_fw_static_route *route,
			 dma_addr_t dma, size_t bytes, size_t alignment,
			 bool allow_experimental)
{
	u64 end;
	int ret;

	ret = validate_route(route, allow_experimental);
	if (ret)
		return ret;
	if (!bytes || !is_power_of_2(alignment) || !IS_ALIGNED((u64)dma, alignment))
		return -EINVAL;
	if (check_add_overflow((u64)dma, (u64)bytes - 1, &end) ||
	    end > route->dma_addr_mask || end > VXD_PTE_ADDR_MASK + SZ_4K - 1)
		return -ERANGE;
	return 0;
}

static int alloc_span(struct device *dev,
		      const struct a1599_fw_static_route *route,
		      size_t bytes, size_t alignment,
		      struct a1599_fw_static_allocation *a,
		      bool allow_experimental)
{
	int ret;

	memset(a, 0, sizeof(*a));
	a->dev = dev;
	a->alloc_bytes = bytes;
	a->alloc_cpu = dma_alloc_coherent(dev, bytes, &a->alloc_dma, GFP_KERNEL);
	if (!a->alloc_cpu)
		return -ENOMEM;
	ret = validate_span(route, a->alloc_dma, bytes, alignment,
			    allow_experimental);
	if (ret)
		return ret;
	a->cpu = a->alloc_cpu;
	a->dma = a->alloc_dma;
	a->bytes = bytes;
	return 0;
}

static void free_one(struct a1599_fw_static_allocation *a)
{
	if (a->dev && a->alloc_cpu)
		dma_free_coherent(a->dev, a->alloc_bytes,
				  a->alloc_cpu, a->alloc_dma);
	memset(a, 0, sizeof(*a));
}

struct a1599_decode_alloc_context {
	struct device *dev;
	const struct a1599_fw_static_route *route;
	struct a1599_fw_static_boot_memory *memory;
	bool allow_experimental;
};

static int decode_dma_alloc_callback(void *context, unsigned int index,
				     size_t bytes,
				     struct a1599_decode_dma_view *view)
{
	struct a1599_decode_alloc_context *ctx = context;
	struct a1599_fw_static_allocation *allocation;
	int ret;

	if (!ctx || !ctx->memory || index >= A1599_DECODE_RESOURCE_COUNT)
		return -EINVAL;
	allocation = &ctx->memory->decode[index];
	ret = alloc_span(ctx->dev, ctx->route, bytes, SZ_4K, allocation,
			 ctx->allow_experimental);
	if (ret) {
		free_one(allocation);
		return ret;
	}
	view->cpu = allocation->cpu;
	view->dma = allocation->dma;
	view->bytes = allocation->bytes;
	return 0;
}

static void decode_dma_free_callback(void *context, unsigned int index,
				     struct a1599_decode_dma_view *view)
{
	struct a1599_decode_alloc_context *ctx = context;

	(void)view;
	if (ctx && ctx->memory && index < A1599_DECODE_RESOURCE_COUNT)
		free_one(&ctx->memory->decode[index]);
}

static const struct a1599_decode_dma_ops a1599_decode_dma_ops = {
	.alloc = decode_dma_alloc_callback,
	.free = decode_dma_free_callback,
};

static int release_all(struct a1599_fw_static_boot_memory *m)
{
	int decode_ret;
	int i;

	if (!m)
		return -EINVAL;
	decode_ret = a1599_decode_dma_release(&m->decode_lifetime,
			&a1599_decode_dma_ops, &(struct a1599_decode_alloc_context) {
				.memory = m,
			});
	if (decode_ret)
		return decode_ret;
	for (i = A1599_FW_STATIC_LEAF_PAGES - 1; i >= 0; i--)
		free_one(&m->leaf[i]);
	free_one(&m->root);
	for (i = A1599_DECODE_RESOURCE_COUNT - 1; i >= 0; i--)
		free_one(&m->decode[i]);
	free_one(&m->context_small);
	free_one(&m->context_medium);
	free_one(&m->context_large);
	free_one(&m->extra);
	free_one(&m->staging);
	memset(m, 0, sizeof(*m));
	return 0;
}

static int dma_mask_set_pair(void *context, a1599_dma_u64 mask)
{
	return dma_set_mask_and_coherent(context, mask);
}

static int dma_mask_set_streaming(void *context, a1599_dma_u64 mask)
{
	return dma_set_mask(context, mask);
}

static int dma_mask_set_coherent(void *context, a1599_dma_u64 mask)
{
	return dma_set_coherent_mask(context, mask);
}

static const struct a1599_fw_dma_mask_ops a1599_dma_mask_ops = {
	.set_pair = dma_mask_set_pair,
	.set_streaming = dma_mask_set_streaming,
	.set_coherent = dma_mask_set_coherent,
};

static int restore_device_masks(struct device *dev,
				struct a1599_fw_static_boot_memory *m,
				const struct a1599_fw_dma_mask_snapshot *snapshot)
{
	int ret;

	ret = a1599_fw_dma_mask_restore(&a1599_dma_mask_ops, dev, snapshot);
	return a1599_fw_static_boot_memory_restore_status(m, ret, 0);
}

static int alloc_root_aligned(struct device *dev,
			      const struct a1599_fw_static_route *route,
			      struct a1599_fw_static_allocation *root,
			      bool allow_experimental)
{
	dma_addr_t aligned;
	size_t offset;
	int ret;

	ret = alloc_span(dev, route, VXD_ROOT_ALLOC_BYTES, SZ_4K, root,
			 allow_experimental);
	if (ret)
		return ret;
	if (check_add_overflow(root->alloc_dma,
			       (dma_addr_t)A1599_FW_STATIC_ROOT_ALIGN - 1,
			       &aligned))
		return -ERANGE;
	aligned &= ~((dma_addr_t)A1599_FW_STATIC_ROOT_ALIGN - 1);
	offset = (size_t)(aligned - root->alloc_dma);
	if (offset > root->alloc_bytes - A1599_FW_STATIC_ROOT_BYTES)
		return -ERANGE;
	ret = validate_span(route, aligned, A1599_FW_STATIC_ROOT_BYTES,
			    A1599_FW_STATIC_ROOT_ALIGN, allow_experimental);
	if (ret)
		return ret;
	root->cpu = (u8 *)root->alloc_cpu + offset;
	root->dma = aligned;
	root->bytes = A1599_FW_STATIC_ROOT_BYTES;
	return 0;
}


static int validate_allocations_disjoint(
		const struct a1599_fw_static_boot_memory *m)
{
	const struct a1599_fw_static_allocation *a[] = {
		&m->staging, &m->extra, &m->context_large, &m->context_medium,
		&m->context_small,
		&m->decode[A1599_DECODE_PRELOAD], &m->decode[A1599_DECODE_VLC],
		&m->decode[A1599_DECODE_BITSTREAM],
		&m->decode[A1599_DECODE_CURRENT_PICTURE],
		&m->decode[A1599_DECODE_INTRA], &m->decode[A1599_DECODE_OUTPUT],
		&m->decode[A1599_DECODE_COMMAND], &m->root,
		&m->leaf[0], &m->leaf[1], &m->leaf[2], &m->leaf[3],
	};
	unsigned int i, j;

	for (i = 0; i < ARRAY_SIZE(a); i++) {
		u64 a_end;

		if (!a[i]->alloc_cpu || !a[i]->alloc_bytes ||
		    check_add_overflow((u64)a[i]->alloc_dma,
				       (u64)a[i]->alloc_bytes - 1, &a_end))
			return -EINVAL;
		for (j = i + 1; j < ARRAY_SIZE(a); j++) {
			u64 b_end;

			if (!a[j]->alloc_cpu || !a[j]->alloc_bytes ||
			    check_add_overflow((u64)a[j]->alloc_dma,
					       (u64)a[j]->alloc_bytes - 1,
					       &b_end))
				return -EINVAL;
			if ((u64)a[i]->alloc_dma <= b_end &&
			    (u64)a[j]->alloc_dma <= a_end)
				return -EADDRINUSE;
		}
	}
	return 0;
}

static bool allocation_has_state(
		const struct a1599_fw_static_allocation *a)
{
	return a->dev || a->alloc_cpu || a->alloc_dma || a->alloc_bytes ||
		a->cpu || a->dma || a->bytes;
}

static bool boot_memory_has_live_state(
		const struct a1599_fw_static_boot_memory *m)
{
	unsigned int i;

	if (m->allocated || m->hardware_started || m->tables_sealed ||
	    allocation_has_state(&m->staging) ||
	    allocation_has_state(&m->extra) ||
	    allocation_has_state(&m->context_large) ||
	    allocation_has_state(&m->context_medium) ||
	    allocation_has_state(&m->context_small) ||
	    allocation_has_state(&m->root))
		return true;
	if (m->decode_lifetime.allocated_count || m->decode_lifetime.started)
		return true;
	for (i = 0; i < A1599_DECODE_RESOURCE_COUNT; i++)
		if (allocation_has_state(&m->decode[i]))
			return true;
	for (i = 0; i < A1599_FW_STATIC_LEAF_PAGES; i++)
		if (allocation_has_state(&m->leaf[i]))
			return true;
	return false;
}

static int boot_memory_alloc_common(struct device *dev,
		const struct a1599_fw_static_route *route,
		struct a1599_fw_static_boot_memory *m,
		bool allow_experimental)
{
	struct a1599_decode_alloc_context decode_ctx = {
		.dev = dev,
		.route = route,
		.memory = m,
		.allow_experimental = allow_experimental,
	};
	struct a1599_fw_dma_mask_snapshot dma_masks;
	bool restore_failed;
	int i, ret;

	BUILD_BUG_ON(PAGE_SIZE != SZ_4K);
	if (!dev || !m)
		return -EINVAL;
	/* Caller must preserve this object after a mask-restore failure. */
	if (m->backend_unavailable)
		return -EIO;
	/* Never overwrite live handles: doing so leaks DMA allocations and can
	 * erase the started lifetime pin. Call release explicitly before reuse.
	 */
	if (boot_memory_has_live_state(m))
		return -EBUSY;
	memset(m, 0, sizeof(*m));
	ret = validate_route(route, allow_experimental);
	if (ret)
		return ret;
	if (!dev->dma_mask || !*dev->dma_mask || !dev->coherent_dma_mask)
		return -EINVAL;
	ret = a1599_fw_dma_mask_begin(&a1599_dma_mask_ops, dev,
			*dev->dma_mask, dev->coherent_dma_mask,
		route->dma_addr_mask, &dma_masks, &restore_failed);
	if (ret) {
		if (restore_failed)
			a1599_fw_static_boot_memory_restore_status(m, ret, 0);
		return ret;
	}

	ret = alloc_span(dev, route, A1599_FW_STATIC_STAGING_BYTES, SZ_4K,
			 &m->staging, allow_experimental);
	if (ret)
		goto fail;
	ret = alloc_span(dev, route, A1599_FW_STATIC_EXTRA_BYTES, SZ_4K,
			 &m->extra, allow_experimental);
	if (ret)
		goto fail;
	ret = alloc_span(dev, route, A1599_FW_STATIC_CONTEXT_LARGE, SZ_4K,
			 &m->context_large, allow_experimental);
	if (ret)
		goto fail;
	ret = alloc_span(dev, route, A1599_FW_STATIC_CONTEXT_MEDIUM, SZ_4K,
			 &m->context_medium, allow_experimental);
	if (ret)
		goto fail;
	/* Back the 0x400-byte object with one DMA page for 4-KiB PTE mapping. */
	ret = alloc_span(dev, route, SZ_4K, SZ_4K, &m->context_small,
			 allow_experimental);
	if (ret)
		goto fail;
	ret = a1599_decode_dma_allocate(&m->decode_lifetime,
			&a1599_decode_dma_ops, &decode_ctx);
	if (ret)
		goto fail;
	ret = alloc_root_aligned(dev, route, &m->root, allow_experimental);
	if (ret)
		goto fail;
	memset(m->root.cpu, 0, m->root.bytes);
	for (i = 0; i < A1599_FW_STATIC_LEAF_PAGES; i++) {
		ret = alloc_span(dev, route, SZ_4K, SZ_4K, &m->leaf[i],
				 allow_experimental);
		if (ret)
			goto fail;
		memset(m->leaf[i].cpu, 0, SZ_4K);
	}
	ret = validate_allocations_disjoint(m);
	if (ret)
		goto fail;
	/*
	 * Root stays zero until the caller supplies reviewed VXD IOVAs and
	 * installs PDE/PTE entries for the exact static layout. Never guess
	 * virtual root indices or map unrelated fallback pages as valid.
	 */
	memset(m->staging.cpu, 0, m->staging.bytes);
	memset(m->extra.cpu, 0, m->extra.bytes);
	memset(m->context_large.cpu, 0, m->context_large.bytes);
	memset(m->context_medium.cpu, 0, m->context_medium.bytes);
	memset(m->context_small.cpu, 0, A1599_FW_STATIC_CONTEXT_SMALL);
	m->allocated = true;
	ret = restore_device_masks(dev, m, &dma_masks);
	if (ret) {
		if (release_all(m))
			m->backend_unavailable = true;
		m->backend_unavailable = true;
		return ret;
	}
	return 0;

fail:
	if (release_all(m)) {
		m->backend_unavailable = true;
		return -EIO;
	}
	{
		int restore_ret = restore_device_masks(dev, m, &dma_masks);

		if (restore_ret) {
			m->backend_unavailable = true;
			return restore_ret;
		}
	}
	return ret;
}

int a1599_fw_static_boot_memory_alloc(struct device *dev,
		const struct a1599_fw_static_route *route,
		struct a1599_fw_static_boot_memory *m)
{
	return boot_memory_alloc_common(dev, route, m, false);
}

int a1599_fw_static_boot_memory_alloc_experimental(struct device *dev,
		const struct a1599_fw_static_route *route,
		struct a1599_fw_static_boot_memory *m)
{
	return boot_memory_alloc_common(dev, route, m, true);
}

int a1599_fw_static_boot_memory_mark_started(
		struct a1599_fw_static_boot_memory *m)
{
	int ret;

	if (!m || !m->allocated || m->hardware_started || m->backend_unavailable)
		return -EINVAL;
	ret = a1599_fw_static_boot_memory_validate_tables(m);
	if (ret)
		return ret;
	ret = a1599_decode_dma_mark_started(&m->decode_lifetime);
	if (ret)
		return ret;
	m->hardware_started = true;
	return 0;
}

int a1599_fw_static_boot_memory_release(
		struct a1599_fw_static_boot_memory *m)
{
	if (!m)
		return -EINVAL;
	if (m->backend_unavailable)
		return -EIO;
	if (m->hardware_started)
		return -EBUSY;
	return release_all(m);
}
