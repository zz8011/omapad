/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef A1599_FW_DECODE_LIFETIME_H
#define A1599_FW_DECODE_LIFETIME_H

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>
typedef u32 a1599_lifetime_u32;
#else
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef uint32_t a1599_lifetime_u32;
#endif

#include "a1599_fw_decode_layout.h"

struct a1599_decode_dma_view {
	void *cpu;
	unsigned long long dma;
	size_t bytes;
};

struct a1599_decode_dma_ops {
	int (*alloc)(void *context, unsigned int index, size_t bytes,
		     struct a1599_decode_dma_view *view);
	void (*free)(void *context, unsigned int index,
		     struct a1599_decode_dma_view *view);
};

struct a1599_decode_dma_set {
	struct a1599_decode_dma_view buffers[A1599_DECODE_RESOURCE_COUNT];
	unsigned int allocated_count;
	int started;
};

static const a1599_lifetime_u32 a1599_decode_dma_sizes[
		A1599_DECODE_RESOURCE_COUNT] = {
	0x1000U, 0x1000U, 0x1000U, 0x4000U, 0x10000U, 0x1000U, 0x1000U,
};

static inline int a1599_decode_dma_allocate(
		struct a1599_decode_dma_set *set,
		const struct a1599_decode_dma_ops *ops, void *context)
{
	unsigned int i, j;
	int ret;

	if (!set || !ops || !ops->alloc || !ops->free || set->started ||
	    set->allocated_count)
		return -EINVAL;
	memset(set, 0, sizeof(*set));
	for (i = 0; i < A1599_DECODE_RESOURCE_COUNT; i++) {
		struct a1599_decode_dma_view view = {};
		unsigned long long end;

		ret = ops->alloc(context, i, a1599_decode_dma_sizes[i], &view);
		if (ret) {
			/* The backend may have acquired a partial current allocation. */
			ops->free(context, i, &view);
			goto rollback;
		}
		if (!view.cpu || view.bytes != a1599_decode_dma_sizes[i] ||
		    (view.dma & (A1599_DECODE_PAGE_BYTES - 1)) ||
		    view.dma + view.bytes < view.dma) {
			ops->free(context, i, &view);
			ret = -ERANGE;
			goto rollback;
		}
		end = view.dma + view.bytes;
		for (j = 0; j < set->allocated_count; j++) {
			unsigned long long prior = set->buffers[j].dma;
			unsigned long long prior_end = prior + set->buffers[j].bytes;
			if (view.dma < prior_end && prior < end) {
				ops->free(context, i, &view);
				ret = -EADDRINUSE;
				goto rollback;
			}
		}
		memset(view.cpu, 0, view.bytes);
		set->buffers[i] = view;
		set->allocated_count++;
	}
	return 0;

rollback:
	while (set->allocated_count) {
		i = --set->allocated_count;
		ops->free(context, i, &set->buffers[i]);
		memset(&set->buffers[i], 0, sizeof(set->buffers[i]));
	}
	return ret;
}

static inline int a1599_decode_dma_release(
		struct a1599_decode_dma_set *set,
		const struct a1599_decode_dma_ops *ops, void *context)
{
	unsigned int i;

	if (!set || !ops || !ops->free)
		return -EINVAL;
	if (set->started)
		return -EBUSY;
	while (set->allocated_count) {
		i = --set->allocated_count;
		ops->free(context, i, &set->buffers[i]);
		memset(&set->buffers[i], 0, sizeof(set->buffers[i]));
	}
	return 0;
}

static inline int a1599_decode_dma_mark_started(
		struct a1599_decode_dma_set *set)
{
	if (!set || set->started ||
	    set->allocated_count != A1599_DECODE_RESOURCE_COUNT)
		return -EINVAL;
	set->started = 1;
	return 0;
}

#endif
