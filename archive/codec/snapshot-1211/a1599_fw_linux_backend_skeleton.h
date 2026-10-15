/* SPDX-License-Identifier: MIT */
#ifndef A1599_FW_LINUX_BACKEND_SKELETON_H
#define A1599_FW_LINUX_BACKEND_SKELETON_H

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/types.h>
#include "a1599_fw_static_boot_memory.h"

/* These are distinct address spaces. Never alias them by casting. */
struct a1599_fw_buffer_ref {
	void *cpu_va;
	dma_addr_t system_dma_addr;
	u64 vxd_iova;
	size_t bytes;
};

/* Review state is explicit: UNKNOWN is never promoted by a true-like flag. */
/* Review inputs are not runtime hardware discoveries. */
struct a1599_fw_backend_evidence {
	enum a1599_fw_evidence_state firmware_image;
	enum a1599_fw_evidence_state target_version_delta;
	enum a1599_fw_evidence_state vxd_resource;
	enum a1599_fw_evidence_state register_access_contract;
	enum a1599_fw_evidence_state io_ordering_contract;
	enum a1599_fw_evidence_state clock_reset_sequence;
	enum a1599_fw_evidence_state system_dma_route;
	enum a1599_fw_evidence_state vxd_internal_mmu_contract;
	/* Initial one-shot table write visibility; separate from later changes. */
	enum a1599_fw_evidence_state vxd_static_table_visibility;
	/* Reset/boot guarantees a clean initial TLB, or a verified initial flush. */
	enum a1599_fw_evidence_state vxd_initial_tlb_state;
};

/* Resource-0 mapping owned by one backend instance, never a global base. */
struct a1599_fw_reg_window {
	struct device *dev; /* optional diagnostic context for the experiment */
	void __iomem *base;
	resource_size_t bytes;
};

struct a1599_fw_linux_backend_ops {
	int (*platform_ready)(struct device *dev);
	int (*dma_mmu_ready)(struct device *dev,
			     struct a1599_fw_buffer_ref *firmware);
	/* Mechanism is path-conditional and completion semantics are unresolved. */
	int (*sync_vxd_mmu)(struct device *dev);
	int (*reset)(struct device *dev);
	int (*upload)(struct device *dev,
		      const struct a1599_fw_buffer_ref *firmware,
		      u32 upload_words);
	int (*mtx_start)(struct device *dev);
	int (*wait_signature)(struct device *dev,
			      const struct a1599_fw_buffer_ref *firmware,
			      u32 logical_offset, u32 expected,
			      unsigned int timeout_ms);
};

int a1599_fw_linux_reg_read32_skeleton(
		const struct a1599_fw_reg_window *window,
		u32 logical_offset, u32 *value);
int a1599_fw_linux_reg_write32_skeleton(
		const struct a1599_fw_reg_window *window,
		u32 logical_offset, u32 value);
const struct a1599_fw_linux_backend_ops *
a1599_fw_linux_backend_skeleton_ops(void);
int a1599_fw_linux_backend_check_evidence(
		const struct a1599_fw_backend_evidence *evidence);

#endif /* A1599_FW_LINUX_BACKEND_SKELETON_H */
