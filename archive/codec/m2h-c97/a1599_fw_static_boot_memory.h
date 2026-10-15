/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef A1599_FW_STATIC_BOOT_MEMORY_H
#define A1599_FW_STATIC_BOOT_MEMORY_H

#include <linux/dma-mapping.h>
#include <linux/types.h>

#include "a1599_fw_decode_layout.h"
#include "a1599_fw_decode_lifetime.h"

#define A1599_FW_STATIC_STAGING_BYTES 0xe000U
#define A1599_FW_STATIC_EXTRA_BYTES  0x2000U
#define A1599_FW_STATIC_CONTEXT_LARGE 0x100000U
#define A1599_FW_STATIC_CONTEXT_MEDIUM 0x040000U
#define A1599_FW_STATIC_CONTEXT_SMALL 0x000400U
#define A1599_FW_STATIC_CONTEXT_SMALL_BACKING 0x001000U
#define A1599_FW_STATIC_STAGING_IOVA 0x40000000U
#define A1599_FW_STATIC_EXTRA_IOVA 0x40010000U
#define A1599_FW_STATIC_CONTEXT_LARGE_IOVA 0x40100000U
#define A1599_FW_STATIC_CONTEXT_MEDIUM_IOVA 0x40300000U
#define A1599_FW_STATIC_CONTEXT_SMALL_IOVA 0x40340000U
#define A1599_FW_STATIC_REQUIRED_MAPS 5U
#define A1599_FW_STATIC_TOTAL_REQUIRED_MAPS \
	(A1599_FW_STATIC_REQUIRED_MAPS + A1599_DECODE_RESOURCE_COUNT)
#define A1599_FW_STATIC_ROOT_BYTES   0x1000U
#define A1599_FW_STATIC_ROOT_ALIGN   0x10000U
#define A1599_FW_STATIC_PAGE_BYTES   0x1000U
#define A1599_FW_STATIC_ROOT_ENTRIES 1024U
#define A1599_FW_STATIC_LEAF_ENTRIES 1024U
#define A1599_FW_STATIC_LEAF_PAGES   4U
#define A1599_FW_DECODE_PRELOAD_BYTES 0x1000U
#define A1599_FW_DECODE_VLC_BYTES 0x1000U
#define A1599_FW_DECODE_BITSTREAM_BYTES 0x1000U
#define A1599_FW_DECODE_CURRENT_PICTURE_BYTES 0x4000U
#define A1599_FW_DECODE_INTRA_BYTES 0x10000U
#define A1599_FW_DECODE_OUTPUT_BYTES 0x1000U
#define A1599_FW_DECODE_COMMAND_BYTES 0x1000U
#define A1599_FW_STATIC_STAGING_PAGES \
	(A1599_FW_STATIC_STAGING_BYTES / A1599_FW_STATIC_PAGE_BYTES)
#define A1599_FW_STATIC_RESERVED_BYTES \
	(A1599_FW_STATIC_STAGING_BYTES + A1599_FW_STATIC_EXTRA_BYTES + \
	 A1599_FW_STATIC_CONTEXT_LARGE + A1599_FW_STATIC_CONTEXT_MEDIUM + \
	 A1599_FW_STATIC_PAGE_BYTES + \
	 A1599_FW_DECODE_PRELOAD_BYTES + A1599_FW_DECODE_VLC_BYTES + \
	 A1599_FW_DECODE_BITSTREAM_BYTES + \
	 A1599_FW_DECODE_CURRENT_PICTURE_BYTES + A1599_FW_DECODE_INTRA_BYTES + \
	 A1599_FW_DECODE_OUTPUT_BYTES + A1599_FW_DECODE_COMMAND_BYTES + \
	 (2 * A1599_FW_STATIC_ROOT_ALIGN) + \
	 (A1599_FW_STATIC_LEAF_PAGES * A1599_FW_STATIC_PAGE_BYTES))

enum a1599_fw_evidence_state {
	A1599_FW_EVIDENCE_UNKNOWN = 0,
	A1599_FW_EVIDENCE_REVIEWED,
	/* Explicitly accepted only by the separate experimental entry point. */
	A1599_FW_EVIDENCE_EXPERIMENTAL_ASSUMPTION,
};

struct a1599_fw_static_route {
	enum a1599_fw_evidence_state dma_route;
	u64 dma_addr_mask;
	/* Evidence that encoded VXD PTEs address these DMA API addresses. */
	enum a1599_fw_evidence_state pte_dma_domain;
};

struct a1599_fw_static_allocation {
	struct device *dev;
	void *alloc_cpu;
	dma_addr_t alloc_dma;
	size_t alloc_bytes;
	void *cpu;
	dma_addr_t dma;
	size_t bytes;
};

struct a1599_fw_static_boot_memory {
	struct a1599_fw_static_allocation staging;
	struct a1599_fw_static_allocation extra;
	struct a1599_fw_static_allocation context_large;
	struct a1599_fw_static_allocation context_medium;
	struct a1599_fw_static_allocation context_small;
	struct a1599_fw_static_allocation decode[A1599_DECODE_RESOURCE_COUNT];
	struct a1599_decode_dma_set decode_lifetime;
	struct a1599_fw_static_allocation root;
	struct a1599_fw_static_allocation leaf[A1599_FW_STATIC_LEAF_PAGES];
	u16 leaf_root_index[A1599_FW_STATIC_LEAF_PAGES];
	u32 staging_vxd_iova;
	u32 extra_vxd_iova;
	u32 context_large_vxd_iova;
	u32 context_medium_vxd_iova;
	u32 context_small_vxd_iova;
	u32 decode_vxd_iova[A1599_DECODE_RESOURCE_COUNT];
	u32 tables_crc32;
	bool tables_sealed;
	bool allocated;
	bool hardware_started;
	bool backend_unavailable;
};

static inline int a1599_fw_static_boot_memory_restore_status(
		struct a1599_fw_static_boot_memory *memory,
		int streaming_restore_error, int coherent_restore_error)
{
	int ret = streaming_restore_error ? streaming_restore_error :
		coherent_restore_error;

	if (ret)
		memory->backend_unavailable = true;
	return ret;
}

/*
 * CPU-side allocation/zeroing only. The VXD root remains invalid/zero until
 * the caller supplies the full five INIT maps plus all seven decode maps and
 * builds the sealed page tables. No MMIO or start is performed here.
 */
int a1599_fw_static_boot_memory_alloc(struct device *dev,
		const struct a1599_fw_static_route *route,
		struct a1599_fw_static_boot_memory *memory);
/* Only for the separately gated one-shot experiment build. */
int a1599_fw_static_boot_memory_alloc_experimental(struct device *dev,
		const struct a1599_fw_static_route *route,
		struct a1599_fw_static_boot_memory *memory);
int a1599_fw_static_boot_memory_mark_started(
		struct a1599_fw_static_boot_memory *memory);
int a1599_fw_static_boot_memory_release(
		struct a1599_fw_static_boot_memory *memory);
int a1599_fw_static_boot_memory_validate_tables(
		const struct a1599_fw_static_boot_memory *memory);

#endif
