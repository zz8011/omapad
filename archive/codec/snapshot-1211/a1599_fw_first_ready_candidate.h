/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef A1599_FW_FIRST_READY_CANDIDATE_H
#define A1599_FW_FIRST_READY_CANDIDATE_H

#include "a1599_fw_linux_backend_skeleton.h"
#include "a1599_fw_static_boot_memory.h"

struct a1599_fw_fixed_mapping {
	u32 vxd_iova;
	dma_addr_t system_dma_addr;
	size_t bytes;
};

struct a1599_fw_ring_snapshot {
	u32 h2m_descriptor;
	u32 m2h_descriptor;
	u32 h2m_read_index;
	u32 h2m_write_index;
	u32 m2h_read_index;
	u32 m2h_write_index;
	u32 h2m_offset_bytes;
	u32 h2m_capacity_words;
	u32 m2h_offset_bytes;
	u32 m2h_capacity_words;
	u64 h2m_start;
	u64 h2m_end;
	u64 m2h_start;
	u64 m2h_end;
};

struct a1599_fw_first_ready_layout {
	const void *firmware_image;
	size_t firmware_image_bytes;
	u32 upload_words;
	struct a1599_fw_backend_evidence evidence;
	struct a1599_fw_static_boot_memory *memory;
	u32 failed_logical_offset;
	int failed_errno;
	/* Cleanup failure is kept separate from the first operation error. */
	int selector_cleanup_errno;
	u32 selector_cleanup_logical_offset;
	/* Bus quiescence stays unknown after any selector transaction failure. */
	bool selector_bus_state_unknown;
};

int a1599_fw_first_ready_build_tables(
		const struct a1599_fw_static_route *route,
		struct a1599_fw_static_boot_memory *memory,
		const struct a1599_fw_fixed_mapping *maps, unsigned int map_count);
/* Accepts explicit experimental DMA-route assumptions; never used by default. */
int a1599_fw_first_ready_build_tables_experimental(
		const struct a1599_fw_static_route *route,
		struct a1599_fw_static_boot_memory *memory,
		const struct a1599_fw_fixed_mapping *maps, unsigned int map_count);

int a1599_fw_first_ready_candidate_run(
		struct a1599_fw_reg_window *window,
		struct a1599_fw_first_ready_layout *layout);

/* Separate opt-in experiment entry; returns -EOPNOTSUPP unless explicitly
 * built with CONFIG_A1599_FW_EXPLICIT_EXPERIMENT after platform review.
 */
int a1599_fw_first_ready_experiment_run(
		struct a1599_fw_reg_window *window,
		struct a1599_fw_first_ready_layout *layout);

/* Side-effect-free checks for the explicitly enabled experiment path. */
int a1599_fw_first_ready_experiment_validate(
		const struct a1599_fw_first_ready_layout *layout);

#endif
