// SPDX-License-Identifier: GPL-2.0-only
/*
 * Offline first-ready Linux backend candidate. Compile only; this file has
 * no module init/driver registration and is never called by this workspace.
 * The entry point fails before MMIO until every exact-target evidence gate
 * is explicitly supplied by the integrating driver.
 */
#include <linux/delay.h>
#include <linux/export.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/string.h>

#include "a1599_fw_linux_backend_skeleton.h"
#include "a1599_fw_first_ready_candidate.h"
#include "a1599_fw_static_boot_memory.h"

#define A1599_REG_WINDOW_BYTES       0x100000U
#define A1599_FW_STAGE_BYTES         0xe000U
#define A1599_FW_IMAGE_BYTES         A1599_FW_STAGE_BYTES
#define A1599_FW_UPLOAD_WORDS        0x3800U
#define A1599_FW_READY_OFFSET        0x2fe0U
#define A1599_FW_READY_SIGNATURE     0xa5a5a5a5U
#define A1599_MTX_START_PC           0x80900000U
#define A1599_MTX_SELECTOR_PC        5U
#define A1599_VXD_COMM_CONFIG        0x50U
#define A1599_VXD_COMM_VERSION       0xc7U

/* Bounds are candidate caps, not recovered Apple delay-unit semantics. */
#define A1599_POLL_INTERVAL_US       1000U
#define A1599_POLL_MAX_MS            2000U
#define A1599_POLL_MAX_ITERATIONS    2000U

static int a1599_read32(const struct a1599_fw_reg_window *w, u32 off, u32 *v)
{
	if (!w || !w->base || !v || !IS_ALIGNED(off, sizeof(u32)) ||
	    off > w->bytes || w->bytes - off < sizeof(u32))
		return -EINVAL;
	*v = readl(w->base + off);
	return 0;
}

static int a1599_write32(const struct a1599_fw_reg_window *w, u32 off, u32 v)
{
	if (!w || !w->base || !IS_ALIGNED(off, sizeof(u32)) ||
	    off > w->bytes || w->bytes - off < sizeof(u32))
		return -EINVAL;
	writel(v, w->base + off);
	return 0;
}

static int a1599_poll_mask(const struct a1599_fw_reg_window *w, u32 off,
			   u32 mask, u32 expected, unsigned int max_ms,
			   unsigned int interval_us, u32 *last)
{
	ktime_t deadline;
	u32 value = 0;
	unsigned int iterations = 0;
	int ret;

	if (!mask || !max_ms || max_ms > A1599_POLL_MAX_MS || !interval_us ||
	    interval_us > A1599_POLL_INTERVAL_US)
		return -EINVAL;
	deadline = ktime_add_ms(ktime_get(), max_ms);
	do {
		ret = a1599_read32(w, off, &value);
		if (ret)
			return ret;
		if ((value & mask) == expected) {
			if (last)
				*last = value;
			return 0;
		}
		if (++iterations >= A1599_POLL_MAX_ITERATIONS)
			break;
		usleep_range(interval_us, interval_us + 100);
	} while (ktime_before(ktime_get(), deadline));
	if (last)
		*last = value;
	return -ETIMEDOUT;
}

static int a1599_poll_exact(const struct a1599_fw_reg_window *w, u32 off,
			    u32 expected, unsigned int max_ms,
			    unsigned int interval_us)
{
	return a1599_poll_mask(w, off, U32_MAX, expected, max_ms,
			       interval_us, NULL);
}

static int a1599_fail(struct a1599_fw_first_ready_layout *layout,
		      u32 offset, int error)
{
	layout->failed_logical_offset = offset;
	layout->failed_errno = error;
	return error;
}

static bool a1599_evidence_reviewed(enum a1599_fw_evidence_state state)
{
	return state == A1599_FW_EVIDENCE_REVIEWED;
}

static bool a1599_evidence_experimentally_allowed(
		enum a1599_fw_evidence_state state)
{
	return state == A1599_FW_EVIDENCE_REVIEWED ||
		state == A1599_FW_EVIDENCE_EXPERIMENTAL_ASSUMPTION;
}

static int a1599_first_ready_evidence_check(
		const struct a1599_fw_first_ready_layout *l,
		bool allow_experimental_assumptions)
{
	if (!l || !l->memory || !l->memory->allocated ||
	    l->memory->backend_unavailable ||
	    !a1599_evidence_reviewed(l->evidence.firmware_image) ||
	    !a1599_evidence_reviewed(l->evidence.target_version_delta) ||
	    !a1599_evidence_reviewed(l->evidence.vxd_resource) ||
	    !a1599_evidence_reviewed(l->evidence.register_access_contract))
		return -EAGAIN;
	if (allow_experimental_assumptions) {
		if (!a1599_evidence_experimentally_allowed(
				l->evidence.io_ordering_contract) ||
		    !a1599_evidence_experimentally_allowed(
				l->evidence.clock_reset_sequence) ||
		    !a1599_evidence_experimentally_allowed(
				l->evidence.system_dma_route) ||
		    !a1599_evidence_experimentally_allowed(
				l->evidence.vxd_internal_mmu_contract) ||
		    !a1599_evidence_experimentally_allowed(
				l->evidence.vxd_static_table_visibility) ||
		    !a1599_evidence_experimentally_allowed(
				l->evidence.vxd_initial_tlb_state))
			return -EAGAIN;
	} else if (!a1599_evidence_reviewed(
			l->evidence.io_ordering_contract) ||
		   !a1599_evidence_reviewed(
			l->evidence.clock_reset_sequence) ||
		   !a1599_evidence_reviewed(l->evidence.system_dma_route) ||
		   !a1599_evidence_reviewed(l->evidence.vxd_internal_mmu_contract) ||
		   !a1599_evidence_reviewed(l->evidence.vxd_static_table_visibility) ||
		   !a1599_evidence_reviewed(l->evidence.vxd_initial_tlb_state)) {
		return -EAGAIN;
	}
	if (!l->firmware_image || l->firmware_image_bytes != A1599_FW_IMAGE_BYTES ||
	    l->memory->staging.bytes != A1599_FW_STAGE_BYTES ||
	    l->upload_words != A1599_FW_UPLOAD_WORDS ||
	    !IS_ALIGNED((u64)l->memory->root.dma, SZ_64K) ||
	    !l->memory->root.dma || !l->memory->staging.dma)
		return -EINVAL;
	return a1599_fw_static_boot_memory_validate_tables(l->memory);
}

static int a1599_reset(struct a1599_fw_reg_window *w,
		       struct a1599_fw_first_ready_layout *l)
{
	u32 v;
	int ret;

#define W(off, val) do { ret = a1599_write32(w, (off), (val)); \
	if (ret) return a1599_fail(l, (off), ret); } while (0)
#define P(off, mask, val, ms, us) do { \
	ret = a1599_poll_mask(w, (off), (mask), (val), (ms), (us), NULL); \
	if (ret) return a1599_fail(l, (off), ret); } while (0)

	/* Candidate translation of the reported core-0 reset ordering. */
	W(0x620, 1);
	ret = a1599_read32(w, 0x620, &v);
	if (ret || v != 1)
		return a1599_fail(l, 0x620, ret ? ret : -EIO);
	W(0x620, 0x7f);
	W(0x000, 0);
	P(0x000, BIT(0), 0, 20, 1000);
	W(0x680, 2);
	P(0x6d0, 0xff, 0, 100, 1000);
	/* No inherited outer retry / diagnostic-only reads on exhaustion. */
	ret = a1599_read32(w, 0x86c, &v);
	if (ret)
		return a1599_fail(l, 0x86c, ret);
	W(0x86c, v | BIT(27));
	W(0x600, 0x11310000);
	ret = a1599_read32(w, 0x600, &v);
	if (ret)
		return a1599_fail(l, 0x600, ret);
	W(0x600, 0);
	P(0x6d0, 0xff, 0, 100, 1000);
	W(0x600, 0x100);
	P(0x600, BIT(8), 0, 1000, 1000);
	W(0x600, 0);
	ret = a1599_read32(w, 0x600, &v);
	if (ret)
		return a1599_fail(l, 0x600, ret);
	W(0x610, 0);
	W(0x60c, U32_MAX);
	return 0;
#undef W
#undef P
}

static int a1599_mtx_selector_begin(struct a1599_fw_reg_window *w,
				   struct a1599_fw_first_ready_layout *l,
				   u32 *saved_108)
{
	int ret;

	ret = a1599_write32(w, 0x6f0, 6);
	if (ret) {
		l->selector_bus_state_unknown = true;
		return a1599_fail(l, 0x6f0, ret);
	}
	ret = a1599_poll_mask(w, 0x6f0, 0x18, 0, 100,
			      A1599_POLL_INTERVAL_US, NULL);
	if (ret) {
		int cleanup = a1599_write32(w, 0x6f0, 4);

		l->selector_bus_state_unknown = true;
		if (cleanup) {
			l->selector_cleanup_errno = cleanup;
			l->selector_cleanup_logical_offset = 0x6f0;
		}
		return a1599_fail(l, 0x6f0, ret);
	}
	ret = a1599_read32(w, 0x108, saved_108);
	if (ret) {
		int cleanup = a1599_write32(w, 0x6f0, 4);

		/* The selector is closed best-effort, but 0x108 was never saved. */
		l->selector_bus_state_unknown = true;
		if (cleanup) {
			l->selector_cleanup_errno = cleanup;
			l->selector_cleanup_logical_offset = 0x6f0;
		}
		return a1599_fail(l, 0x108, ret);
	}
	return 0;
}

static int a1599_mtx_selector_end(struct a1599_fw_reg_window *w,
				 struct a1599_fw_first_ready_layout *l,
				 u32 saved_108)
{
	int restore_ret, close_ret;

	restore_ret = a1599_write32(w, 0x108, saved_108);
	close_ret = a1599_write32(w, 0x6f0, 4);
	if (restore_ret || close_ret) {
		l->selector_cleanup_errno = restore_ret ? restore_ret : close_ret;
		l->selector_cleanup_logical_offset = restore_ret ? 0x108 : 0x6f0;
		l->selector_bus_state_unknown = true;
	}
	return restore_ret ? restore_ret : close_ret;
}

static int a1599_mtx_write_pc(struct a1599_fw_reg_window *w,
			      struct a1599_fw_first_ready_layout *l)
{
	u32 saved_108;
	int ret;

	ret = a1599_mtx_selector_begin(w, l, &saved_108);
	if (ret)
		return ret;
	ret = a1599_write32(w, 0x0f8, A1599_MTX_START_PC);
	if (ret) {
		int primary = a1599_fail(l, 0x0f8, ret);

		l->selector_bus_state_unknown = true;
		a1599_mtx_selector_end(w, l, saved_108);
		return primary;
	}
	ret = a1599_write32(w, 0x0fc, A1599_MTX_SELECTOR_PC);
	if (ret) {
		int primary = a1599_fail(l, 0x0fc, ret);

		l->selector_bus_state_unknown = true;
		a1599_mtx_selector_end(w, l, saved_108);
		return primary;
	}
	ret = a1599_poll_mask(w, 0x0fc, BIT(31), BIT(31), 100,
			      A1599_POLL_INTERVAL_US, NULL);
	if (ret) {
		int primary = a1599_fail(l, 0x0fc, ret);

		l->selector_bus_state_unknown = true;
		a1599_mtx_selector_end(w, l, saved_108);
		return primary;
	}
	ret = a1599_mtx_selector_end(w, l, saved_108);
	if (ret)
		return a1599_fail(l, l->selector_cleanup_logical_offset, ret);
	return 0;
}

static int a1599_mtx_read_pc(struct a1599_fw_reg_window *w,
			     struct a1599_fw_first_ready_layout *l,
			     u32 *pc)
{
	u32 saved_108;
	int ret;

	ret = a1599_mtx_selector_begin(w, l, &saved_108);
	if (ret)
		return ret;
	ret = a1599_write32(w, 0x0fc, BIT(16) | A1599_MTX_SELECTOR_PC);
	if (ret) {
		int primary = a1599_fail(l, 0x0fc, ret);

		l->selector_bus_state_unknown = true;
		a1599_mtx_selector_end(w, l, saved_108);
		return primary;
	}
	ret = a1599_poll_mask(w, 0x0fc, BIT(31), BIT(31), 100,
			      A1599_POLL_INTERVAL_US, NULL);
	if (ret) {
		int primary = a1599_fail(l, 0x0fc, ret);

		l->selector_bus_state_unknown = true;
		a1599_mtx_selector_end(w, l, saved_108);
		return primary;
	}
	ret = a1599_read32(w, 0x0f8, pc);
	if (ret) {
		int primary = a1599_fail(l, 0x0f8, ret);

		l->selector_bus_state_unknown = true;
		a1599_mtx_selector_end(w, l, saved_108);
		return primary;
	}
	ret = a1599_mtx_selector_end(w, l, saved_108);
	if (ret)
		return a1599_fail(l, l->selector_cleanup_logical_offset, ret);
	return 0;
}

/*
 * Future explicitly invoked experimental entry point. This is not wired to
 * module init or a platform driver in this workspace. No calls were made.
 * It reports first firmware signature only; no ring message/decode follows.
 */
static int a1599_fw_first_ready_experiment_execute(
		struct a1599_fw_reg_window *window,
		struct a1599_fw_first_ready_layout *layout,
		bool allow_experimental_assumptions)
{
	int ret;
	u32 v;

	if (layout) {
		layout->failed_logical_offset = 0;
		layout->failed_errno = 0;
		layout->selector_cleanup_errno = 0;
		layout->selector_cleanup_logical_offset = 0;
		layout->selector_bus_state_unknown = false;
	}

	ret = a1599_first_ready_evidence_check(layout,
					       allow_experimental_assumptions);
	if (ret)
		return ret;
	if (!window || !window->base || window->bytes < A1599_REG_WINDOW_BYTES)
		return -EINVAL;

	/* Copy the fully reconstructed 0xe000 staging image, including 0xcd fill. */
	memcpy(layout->memory->staging.cpu, layout->firmware_image,
	       layout->firmware_image_bytes);
	/* Final CPU writes to coherent staging/tables must precede publication. */
	dma_wmb();
	ret = a1599_fw_static_boot_memory_mark_started(layout->memory);
	if (ret)
		return ret;
	if (window->dev)
		dev_info(window->dev,
			 "FIRST_READY_COMMIT: DMA lifetime pinned before first codec MMIO\n");

	if (window->dev)
		dev_info(window->dev, "CORE_RESET_BEGIN\n");
	ret = a1599_reset(window, layout);
	if (ret) {
		if (window->dev)
			dev_err(window->dev,
				"CORE_RESET_FAILED ret=%d logical_offset=%#x\n",
				ret, layout->failed_logical_offset);
		return ret;
	}
	if (window->dev)
		dev_info(window->dev, "CORE_RESET_DONE\n");

	/* Reported start configuration before firmware init. */
#define W(off, val) do { ret = a1599_write32(window, (off), (val)); \
	if (ret) return a1599_fail(layout, (off), ret); } while (0)
#define P(off, mask, val, ms, us) do { \
	ret = a1599_poll_mask(window, (off), (mask), (val), (ms), (us), NULL); \
	if (ret) return a1599_fail(layout, (off), ret); } while (0)
	W(0x620, 1);
	ret = a1599_read32(window, 0x620, &v);
	if (ret || v != 1)
		return a1599_fail(layout, 0x620, ret ? ret : -EIO);
	W(0x620, 0x7f);
	W(0x60c, 0x4000);
	W(0x680, 0);
	W(0x6d4, 4);
	W(0x690, 4);
	W(0x2fdc, 0);
	W(0x200, 1);
	W(0x2fe0, 0);
	W(0x2fe8, 0);
	W(0x2fec, 0);
	W(0x2ff4, 0);
	W(0x2ffc, 0);
	W(0x2ff8, A1599_VXD_COMM_CONFIG);
	W(0x208, A1599_VXD_COMM_VERSION);

	/* Reported upload-preparation order, with fail-closed poll. */
	if (window->dev)
		dev_info(window->dev,
			 "FIRMWARE_UPLOAD_BEGIN bytes=%zu words=%#x staging_iova=%#x\n",
			 layout->firmware_image_bytes, layout->upload_words,
			 layout->memory->staging_vxd_iova);
	W(0x6f0, 6);
	P(0x6f0, 0x18, 0, 100, 100);
	W(0x344, 0);
	W(0x340, 0x10000 | A1599_FW_UPLOAD_WORDS);
	ret = a1599_read32(window, 0x600, &v);
	if (ret)
		return a1599_fail(layout, 0x600, ret);
	W(0x600, v & ~BIT(12));
	W(0x50c, 0);
	W(0x688, 0);

	/* Root and staging addresses are explicit, validated address domains. */
	W(0x694, (u32)((u64)layout->memory->root.dma >> 8));
	/* Candidate follows the reported descriptor B+16 then register-0x500 path. */
	W(0x500, layout->memory->staging_vxd_iova);
	W(0x504, A1599_FW_UPLOAD_WORDS);
	W(0x508, 0x01000000);
	W(0x514, 0x350);
	dma_wmb();
	W(0x504, 0x10000 | A1599_FW_UPLOAD_WORDS);
	P(0x50c, 0x20000, 0x20000, 2000, 1000);
	P(0x348, BIT(0), BIT(0), 20, 1000);
	if (window->dev)
		dev_info(window->dev, "FIRMWARE_UPLOAD_DONE\n");

	if (window->dev)
		dev_info(window->dev, "MTX_PC_WRITE_BEGIN pc=%#x selector=%u\n",
			 A1599_MTX_START_PC, A1599_MTX_SELECTOR_PC);
	ret = a1599_mtx_write_pc(window, layout);
	if (ret)
		return ret;
	if (window->dev)
		dev_info(window->dev, "MTX_PC_WRITE_DONE\n");
	if (window->dev)
		dev_info(window->dev, "MTX_PC_READ_BEGIN selector=%u\n",
			 A1599_MTX_SELECTOR_PC);
	ret = a1599_mtx_read_pc(window, layout, &v);
	if (ret)
		return ret;
	if (window->dev)
		dev_info(window->dev, "MTX_PC_READ_DONE pc=%#x\n", v);
	if (v != A1599_MTX_START_PC)
		return a1599_fail(layout, 0x0f8, -EIO);
	if (window->dev)
		dev_info(window->dev,
			 "MTX_START_WRITE_BEGIN offset=0x0 value=1\n");
	writel(1, window->base + 0x000);
	if (window->dev)
		dev_info(window->dev,
			 "READY_SIGNATURE_POLL_BEGIN offset=%#x expected=%#x timeout_ms=%u\n",
			 A1599_FW_READY_OFFSET, A1599_FW_READY_SIGNATURE,
			 A1599_POLL_MAX_MS);
	ret = a1599_poll_exact(window, A1599_FW_READY_OFFSET,
			       A1599_FW_READY_SIGNATURE,
			       A1599_POLL_MAX_MS, A1599_POLL_INTERVAL_US);
	if (ret)
		return a1599_fail(layout, A1599_FW_READY_OFFSET, ret);
	if (window->dev)
		dev_info(window->dev,
			 "READY_SIGNATURE_MATCH offset=%#x value=%#x\n",
			 A1599_FW_READY_OFFSET, A1599_FW_READY_SIGNATURE);
	return 0;
#undef W
#undef P
}

/* The pure candidate API is permanently fail-closed. */
int a1599_fw_first_ready_candidate_run(
		struct a1599_fw_reg_window *window,
		struct a1599_fw_first_ready_layout *layout)
{
	(void)window;
	(void)layout;
	return -EOPNOTSUPP;
}
EXPORT_SYMBOL_GPL(a1599_fw_first_ready_candidate_run);

#ifdef CONFIG_A1599_FW_EXPLICIT_EXPERIMENT
enum a1599_experiment_state {
	A1599_EXPERIMENT_DISARMED,
	A1599_EXPERIMENT_PREFLIGHT,
	A1599_EXPERIMENT_RUNNING,
	A1599_EXPERIMENT_COMPLETE,
	A1599_EXPERIMENT_FAILED,
};

/* One controlled attempt per module lifetime; state is private to this file. */
DEFINE_MUTEX(a1599_experiment_mutex);
static enum a1599_experiment_state a1599_experiment_state =
	A1599_EXPERIMENT_DISARMED;

int a1599_fw_first_ready_experiment_validate(
		const struct a1599_fw_first_ready_layout *layout)
{
	return a1599_first_ready_evidence_check(layout, true);
}
EXPORT_SYMBOL_GPL(a1599_fw_first_ready_experiment_validate);

int a1599_fw_first_ready_experiment_run(
		struct a1599_fw_reg_window *window,
		struct a1599_fw_first_ready_layout *layout)
{
	int ret;

	mutex_lock(&a1599_experiment_mutex);
	if (a1599_experiment_state != A1599_EXPERIMENT_DISARMED) {
		ret = -EBUSY;
		goto out;
	}
	a1599_experiment_state = A1599_EXPERIMENT_PREFLIGHT;
	ret = a1599_first_ready_evidence_check(layout, true);
	if (ret) {
		/* No MMIO was attempted; remain fail-closed for this module load. */
		a1599_experiment_state = A1599_EXPERIMENT_FAILED;
		goto out;
	}
	a1599_experiment_state = A1599_EXPERIMENT_RUNNING;
	ret = a1599_fw_first_ready_experiment_execute(window, layout, true);
	a1599_experiment_state = ret ? A1599_EXPERIMENT_FAILED :
		A1599_EXPERIMENT_COMPLETE;
out:
	mutex_unlock(&a1599_experiment_mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(a1599_fw_first_ready_experiment_run);
#else
int a1599_fw_first_ready_experiment_validate(
		const struct a1599_fw_first_ready_layout *layout)
{
	(void)layout;
	return -EOPNOTSUPP;
}
EXPORT_SYMBOL_GPL(a1599_fw_first_ready_experiment_validate);

int a1599_fw_first_ready_experiment_run(
		struct a1599_fw_reg_window *window,
		struct a1599_fw_first_ready_layout *layout)
{
	(void)window;
	(void)layout;
	return -EOPNOTSUPP;
}
EXPORT_SYMBOL_GPL(a1599_fw_first_ready_experiment_run);
#endif

MODULE_DESCRIPTION("A1599 VXD offline first-IDR decode serializer candidate");
MODULE_LICENSE("GPL");
