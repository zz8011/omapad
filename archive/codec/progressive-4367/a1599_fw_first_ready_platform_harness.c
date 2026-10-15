// SPDX-License-Identifier: GPL-2.0-only
/*
 * RAM-only, single-device harness for the isolated first-ready experiment.
 * Loading without run_once=1 fails closed. The module is never loaded by this
 * workspace. A start attempt pins DMA memory and keeps the OF device bound;
 * after a started failure, reboot the RAM image instead of retrying/removing
 * the device.
 */
#include <crypto/hash.h>
#include <crypto/algapi.h>
#include <crypto/sha2.h>
#include <linux/atomic.h>
#include <linux/unaligned.h>
#include <linux/delay.h>
#include <linux/dma-map-ops.h>
#include <linux/err.h>
#include <linux/firmware.h>
#include <linux/iommu.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/of.h>
#include <linux/pm_domain.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "a1599_fw_first_ready_candidate.h"
#include "a1599_fw_dma_mask.h"
#include "a1599_fw_decode_stream.h"
#include "a1599_fw_decode_submit.h"
#include "a1599_fw_decode_diagnostics.h"
#include "a1599_fw_decode_layout.h"
#include "a1599_fw_ring_capacity.h"
#include "a1599_fw_vlc_table.h"

#define A1599_VXD_RESOURCE_START 0x207f00000ULL
#define A1599_VXD_RESOURCE_BYTES 0x00100000ULL
#define A1599_FW_BYTES 0xe000U
#define A1599_FW_UPLOAD_WORDS 0x3800U
#define A1599_VERIFIED_COMM_RAM_START 0U /* zero means no write-capable range is verified */
#define A1599_VERIFIED_COMM_RAM_END 0U
#define A1599_INIT_CONSUME_TIMEOUT_MS 2000U
#define A1599_DECODE_RESPONSE_TIMEOUT_MS 2000U
#define A1599_DECODE_OUTPUT_SETTLE_MS 50U
#define A1599_FW_NAME "a1599/vxd-staging-12.5.8-16H88.bin"
#define A1599_FW_SHA256 "6b274c576a7952d7a4f22f65adf5b1041c4329c3dfc7ce761de53668cfd4a41e"
#define A1599_PREFLIGHT_BUDGET_MS 10000U
#define A1599_VDEC_PATH "/soc/power-management@20e000000/power-controller@201f0"
#define A1599_MEDIA_PATH "/soc/power-management@20e000000/power-controller@201d8"
#define A1599_VXD_PATH "/soc/vxd-first-ready@207f00000"

static bool run_once;
module_param(run_once, bool, 0400);
MODULE_PARM_DESC(run_once, "required explicit opt-in; one first-ready attempt only");
static bool snapshot_once;
module_param(snapshot_once, bool, 0400);
MODULE_PARM_DESC(snapshot_once, "after first-ready, read six ring descriptor/index registers once");
static bool init_once;
module_param(init_once, bool, 0400);
MODULE_PARM_DESC(init_once, "after first-ready, attempt one INIT; default off; requires explicit firmware-ring experiment gate or independent range proof");
static bool allow_fw_abi_ring_init;
module_param(allow_fw_abi_ring_init, bool, 0400);
MODULE_PARM_DESC(allow_fw_abi_ring_init,
		 "EXPERIMENTAL: authorize one INIT using firmware-declared live ring intervals within the VXD resource; independent silicon RAM extent remains unproven");
static bool allow_decode_submission;
module_param(allow_decode_submission, bool, 0400);
MODULE_PARM_DESC(allow_decode_submission,
		 "default-off explicit gate for the one-shot ID81 sysfs entry after INIT transport consumption");
static bool allow_output_export;
module_param(allow_output_export, bool, 0400);
MODULE_PARM_DESC(allow_output_export,
		 "default-off read-only export of only the fixed 4-KiB decode output allocation after one decode attempt");
static ushort decode_flags = 0x4015;
module_param(decode_flags, ushort, 0400);
MODULE_PARM_DESC(decode_flags,
		 "source-derived single-IDR type-1 ID81 flags word 0x4015");
static int id81_byte0_bit7;
module_param(id81_byte0_bit7, int, 0400);
MODULE_PARM_DESC(id81_byte0_bit7,
		 "source-derived ID81 byte-zero bit 7; zero produces byte 0x14");
enum a1599_harness_state {
	A1599_HARNESS_IDLE,
	A1599_HARNESS_PREFLIGHT,
	A1599_HARNESS_STARTED,
	A1599_HARNESS_DONE,
	A1599_HARNESS_FAILED,
};

static atomic_t a1599_state = ATOMIC_INIT(A1599_HARNESS_IDLE);
static atomic_t a1599_init_consumed = ATOMIC_INIT(0);
static atomic_t a1599_init_transport_consumed = ATOMIC_INIT(0);
static atomic_t a1599_decode_consumed = ATOMIC_INIT(0);
static atomic_t a1599_m2h_raw_consumed = ATOMIC_INIT(0);
static bool a1599_decode_result_valid;
static bool a1599_decode_response_zero_status;
static bool a1599_decode_output_snapshot_ready;
static u8 a1599_decode_output_snapshot[A1599_FW_DECODE_OUTPUT_BYTES];
static DEFINE_MUTEX(a1599_ring_lock);
static struct a1599_fw_static_boot_memory *a1599_memory;
static struct a1599_fw_reg_window a1599_live_window;
static struct device *a1599_live_device;
static bool a1599_first_ready_live;
static int a1599_overlay_id;
static struct device_node *a1599_vdec_node;
static bool a1599_genpd_attached;
static bool a1599_runtime_pm_enabled;
static bool a1599_pm_reference_held;
static bool a1599_keep_instance;
static const unsigned char a1599_first_ready_dtbo[] = {
#include "a1599_vxd_first_ready_dtbo.inc"
};

static const u32 a1599_decode_resource_bytes[A1599_DECODE_RESOURCE_COUNT] = {
	[A1599_DECODE_PRELOAD] = A1599_FW_DECODE_PRELOAD_BYTES,
	[A1599_DECODE_VLC] = A1599_FW_DECODE_VLC_BYTES,
	[A1599_DECODE_BITSTREAM] = A1599_FW_DECODE_BITSTREAM_BYTES,
	[A1599_DECODE_CURRENT_PICTURE] = A1599_FW_DECODE_CURRENT_PICTURE_BYTES,
	[A1599_DECODE_INTRA] = A1599_FW_DECODE_INTRA_BYTES,
	[A1599_DECODE_OUTPUT] = A1599_FW_DECODE_OUTPUT_BYTES,
	[A1599_DECODE_COMMAND] = A1599_FW_DECODE_COMMAND_BYTES,
};

static const u8 a1599_fixture_idr_nal[21] = {
	0x65, 0x88, 0x84, 0x3a, 0x11, 0x8a, 0x00, 0x02,
	0x18, 0xf1, 0xc0, 0x00, 0x40, 0xf6, 0x38, 0x00,
	0x08, 0x79, 0x49, 0xd7, 0x5e,
};

/* Assign codec IOVAs only inside the already reviewed free interval in the
 * existing 4-MiB page-table region, then bind each to its actual DMA handle.
 * The helper rejects any incomplete, duplicated or altered INIT map set.
 */
static int a1599_prepare_decode_map_entries(
		struct a1599_fw_static_boot_memory *memory,
		struct a1599_fw_fixed_mapping maps[A1599_FW_STATIC_TOTAL_REQUIRED_MAPS])
{
	struct a1599_decode_iova_map init_maps[A1599_DECODE_INIT_MAPS];
	unsigned int i;
	int ret;

	for (i = 0; i < A1599_DECODE_INIT_MAPS; i++) {
		init_maps[i].iova = maps[i].vxd_iova;
		init_maps[i].bytes = maps[i].bytes;
	}
	ret = a1599_decode_assign_iovas(init_maps, ARRAY_SIZE(init_maps),
		a1599_decode_resource_bytes, ARRAY_SIZE(a1599_decode_resource_bytes),
		memory->decode_vxd_iova);
	if (ret)
		return ret;
	for (i = 0; i < A1599_DECODE_RESOURCE_COUNT; i++) {
		maps[A1599_FW_STATIC_REQUIRED_MAPS + i] =
			(struct a1599_fw_fixed_mapping) {
				.vxd_iova = memory->decode_vxd_iova[i],
				.system_dma_addr = memory->decode[i].dma,
				.bytes = memory->decode[i].bytes,
			};
	}
	return 0;
}

/* Called only before first hardware start. It fills the retained DMA buffers
 * and serializes command words from the exact IOVAs that were page-table
 * mapped. The ID81 submission gate is deliberately absent in this build.
 */
static int a1599_populate_decode_buffers(
		struct device *dev, struct a1599_fw_static_boot_memory *memory,
		const struct a1599_fw_fixed_mapping *maps)
{
	struct a1599_decode_span init_maps[A1599_DECODE_INIT_MAPS];
	struct a1599_decode_span *span;
	struct a1599_decode_resources resources = {};
	struct a1599_fw_static_allocation *a = memory->decode;
	u8 *output = a[A1599_DECODE_OUTPUT].cpu;
	unsigned int i;
	int ret;

	BUILD_BUG_ON(A1599_FW_DECODE_OUTPUT_BYTES < 3072U);
	memcpy(a[A1599_DECODE_VLC].cpu, a1599_fw_vlc_table,
	       sizeof(a1599_fw_vlc_table));
	memcpy(a[A1599_DECODE_BITSTREAM].cpu, a1599_fixture_idr_nal,
	       sizeof(a1599_fixture_idr_nal));
	/* Clear candidate image planes and retain a distinct tail guard. */
	memset(output, 0xa5, A1599_FW_DECODE_OUTPUT_BYTES);
	memset(output, 0, 2048U);
	memset(output + 2048U, 0, 1024U);

	resources.preload = (struct a1599_decode_span) {
		maps[5 + A1599_DECODE_PRELOAD].vxd_iova,
		maps[5 + A1599_DECODE_PRELOAD].bytes,
		maps[5 + A1599_DECODE_PRELOAD].vxd_iova,
	};
	resources.vlc = (struct a1599_decode_span) {
		maps[5 + A1599_DECODE_VLC].vxd_iova,
		maps[5 + A1599_DECODE_VLC].bytes,
		maps[5 + A1599_DECODE_VLC].vxd_iova,
	};
	resources.bitstream = (struct a1599_decode_span) {
		maps[5 + A1599_DECODE_BITSTREAM].vxd_iova,
		maps[5 + A1599_DECODE_BITSTREAM].bytes,
		maps[5 + A1599_DECODE_BITSTREAM].vxd_iova + 4U,
	};
	resources.current_picture = (struct a1599_decode_span) {
		maps[5 + A1599_DECODE_CURRENT_PICTURE].vxd_iova,
		maps[5 + A1599_DECODE_CURRENT_PICTURE].bytes,
		maps[5 + A1599_DECODE_CURRENT_PICTURE].vxd_iova,
	};
	resources.intra = (struct a1599_decode_span) {
		maps[5 + A1599_DECODE_INTRA].vxd_iova,
		maps[5 + A1599_DECODE_INTRA].bytes,
		maps[5 + A1599_DECODE_INTRA].vxd_iova,
	};
	resources.output = (struct a1599_decode_span) {
		maps[5 + A1599_DECODE_OUTPUT].vxd_iova,
		maps[5 + A1599_DECODE_OUTPUT].bytes,
		maps[5 + A1599_DECODE_OUTPUT].vxd_iova,
	};
	resources.command = (struct a1599_decode_span) {
		maps[5 + A1599_DECODE_COMMAND].vxd_iova,
		maps[5 + A1599_DECODE_COMMAND].bytes,
		maps[5 + A1599_DECODE_COMMAND].vxd_iova,
	};
	for (i = 0; i < A1599_DECODE_INIT_MAPS; i++) {
		init_maps[i] = (struct a1599_decode_span) {
			maps[i].vxd_iova, maps[i].bytes, maps[i].vxd_iova,
		};
	}
	resources.existing_iova_maps = init_maps;
	resources.existing_iova_map_count = ARRAY_SIZE(init_maps);
	resources.plane0_offset = 0;
	resources.plane1_offset = 2048;
	span = &resources.command;
	ret = a1599_build_fixture_decode_stream(
		a[A1599_DECODE_COMMAND].cpu,
		a[A1599_DECODE_COMMAND].bytes, &resources);
	if (ret < 0)
		return ret;
	if (ret != A1599_DECODE_STREAM_BYTES)
		return -EIO;
	dma_wmb();
	dev_info(dev, "DECODE_RESOURCES_READY allocations=7 maps=12 command_words=%u command_bytes=%u iovas=(preload:%#x vlc:%#x bitstream:%#x current_picture:%#x intra:%#x output:%#x command:%#x) bitstream_payload_offset=4 output_planes=(0,2048) output_guard=[3072,4096) submission_gate=%s\n",
		 A1599_DECODE_STREAM_WORDS, A1599_DECODE_STREAM_BYTES,
		 resources.preload.base_iova, resources.vlc.base_iova,
		 resources.bitstream.codec_iova, resources.current_picture.base_iova,
		 resources.intra.base_iova, resources.output.base_iova,
		 span->base_iova,
		 allow_decode_submission ? "open_one_shot_only" : "closed_default");
	return 0;
}

static bool a1599_intervals_overlap(u64 a_start, u64 a_end,
				   u64 b_start, u64 b_end)
{
	return a_start < b_end && b_start < a_end;
}

static int a1599_ring_read_reg32(struct a1599_fw_reg_window *window,
				u32 offset, u32 *value)
{
	if (!window || !window->base || !value || !IS_ALIGNED(offset, sizeof(u32)) ||
	    offset > window->bytes || window->bytes - offset < sizeof(u32))
		return -EINVAL;
	*value = readl(window->base + offset);
	return 0;
}

/* Reads exactly the two descriptor and four index registers from the static
 * contract. This does not touch the selector/request ports or ring payload.
 */
static int a1599_ring_snapshot_locked(struct a1599_fw_reg_window *window,
				      struct a1599_fw_ring_snapshot *s)
{
	static const u32 regs[] = { 0x2ff0, 0x2fe4, 0x2ff4, 0x2ffc,
				     0x2fe8, 0x2fec };
	static const u32 controls[] = { 0x0000, 0x0050, 0x0080, 0x0208,
					0x0600, 0x0610, 0x0620, 0x0680,
					0x06d0, 0x086c, 0x2fe0, 0x2ff8 };
	u32 *values[] = { &s->h2m_descriptor, &s->m2h_descriptor,
			  &s->h2m_read_index, &s->h2m_write_index,
			  &s->m2h_read_index, &s->m2h_write_index };
	u64 h2m_bytes, m2h_bytes;
	unsigned int i;
	int ret;

	if (!window || !s)
		return -EINVAL;
	memset(s, 0, sizeof(*s));
	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		ret = a1599_ring_read_reg32(window, regs[i], values[i]);
		if (ret)
			return ret;
	}

	s->h2m_offset_bytes = s->h2m_descriptor >> 16;
	s->h2m_capacity_words = s->h2m_descriptor & 0xffff;
	s->m2h_offset_bytes = s->m2h_descriptor >> 16;
	s->m2h_capacity_words = s->m2h_descriptor & 0xffff;
	if (!s->h2m_capacity_words || !s->m2h_capacity_words ||
	    s->h2m_read_index >= s->h2m_capacity_words ||
	    s->h2m_write_index >= s->h2m_capacity_words ||
	    s->m2h_read_index >= s->m2h_capacity_words ||
	    s->m2h_write_index >= s->m2h_capacity_words ||
	    !IS_ALIGNED(s->h2m_offset_bytes, sizeof(u32)) ||
	    !IS_ALIGNED(s->m2h_offset_bytes, sizeof(u32)) ||
	    check_mul_overflow((u64)s->h2m_capacity_words, (u64)sizeof(u32),
			       &h2m_bytes) ||
	    check_mul_overflow((u64)s->m2h_capacity_words, (u64)sizeof(u32),
			       &m2h_bytes) ||
	    check_add_overflow((u64)0x2000, (u64)s->h2m_offset_bytes,
			       &s->h2m_start) ||
	    check_add_overflow(s->h2m_start, h2m_bytes, &s->h2m_end) ||
	    check_add_overflow((u64)0x2000, (u64)s->m2h_offset_bytes,
			       &s->m2h_start) ||
	    check_add_overflow(s->m2h_start, m2h_bytes, &s->m2h_end))
		return -ERANGE;
	if (s->h2m_start < 0x2000 || s->m2h_start < 0x2000 ||
	    s->h2m_end > window->bytes || s->m2h_end > window->bytes ||
	    a1599_intervals_overlap(s->h2m_start, s->h2m_end,
				    s->m2h_start, s->m2h_end))
		return -ERANGE;
	/* Reject overlap with known command/status registers, descriptor/index
	 * words, the ready signature, firmware config and the kick register.
	 */
	for (i = 0; i < ARRAY_SIZE(regs); i++)
		if (a1599_intervals_overlap(s->h2m_start, s->h2m_end,
					    regs[i], regs[i] + sizeof(u32)) ||
		    a1599_intervals_overlap(s->m2h_start, s->m2h_end,
					    regs[i], regs[i] + sizeof(u32)))
			return -ERANGE;
	for (i = 0; i < ARRAY_SIZE(controls); i++)
		if (a1599_intervals_overlap(s->h2m_start, s->h2m_end,
					    controls[i], controls[i] + sizeof(u32)) ||
		    a1599_intervals_overlap(s->m2h_start, s->m2h_end,
					    controls[i], controls[i] + sizeof(u32)))
			return -ERANGE;
	return 0;
}

/* Callers needing a coherent host-side view take the same mutex as all ring
 * transactions and invoke the locked snapshot helper directly. */
static int a1599_ring_snapshot(struct a1599_fw_reg_window *window,
			       struct a1599_fw_ring_snapshot *s)
{
	int ret;

	mutex_lock(&a1599_ring_lock);
	ret = a1599_ring_snapshot_locked(window, s);
	mutex_unlock(&a1599_ring_lock);
	return ret;
}

static bool a1599_comm_ram_range_verified(
		const struct a1599_fw_ring_snapshot *s);

static bool a1599_ring_resource_bounded(
		const struct a1599_fw_reg_window *window,
		const struct a1599_fw_ring_snapshot *s)
{
	return window && s && s->h2m_start >= 0x2000 &&
		s->m2h_start >= 0x2000 && s->h2m_start < s->h2m_end &&
		s->m2h_start < s->m2h_end &&
		s->h2m_end <= window->bytes && s->m2h_end <= window->bytes;
}

static void a1599_log_ring_snapshot(struct device *dev,
				    const struct a1599_fw_reg_window *window,
				    const struct a1599_fw_ring_snapshot *s,
				    int validation_ret, const char *phase)
{
	dev_info(dev, "RING_SNAPSHOT_%s raw_h2m_desc=%#08x raw_m2h_desc=%#08x h2m_offset_bytes=%#x h2m_capacity_words=%u h2m_read_word=%u h2m_write_word=%u m2h_offset_bytes=%#x m2h_capacity_words=%u m2h_read_word=%u m2h_write_word=%u bounds_h2m=[%#llx,%#llx) bounds_m2h=[%#llx,%#llx) validation=%d resource_bounded=%d comm_ram_proven=%d async_payload_captured=0\n",
		phase, s->h2m_descriptor, s->m2h_descriptor,
		s->h2m_offset_bytes, s->h2m_capacity_words,
		s->h2m_read_index, s->h2m_write_index,
		s->m2h_offset_bytes, s->m2h_capacity_words,
		s->m2h_read_index, s->m2h_write_index,
		(unsigned long long)s->h2m_start,
		(unsigned long long)s->h2m_end,
		(unsigned long long)s->m2h_start,
		(unsigned long long)s->m2h_end, validation_ret,
		a1599_ring_resource_bounded(window, s),
		a1599_comm_ram_range_verified(s));
}

static bool a1599_comm_ram_range_verified(
		const struct a1599_fw_ring_snapshot *s)
{
#if A1599_VERIFIED_COMM_RAM_END <= A1599_VERIFIED_COMM_RAM_START
	(void)s;
	return false;
#else
	return s->h2m_start >= A1599_VERIFIED_COMM_RAM_START &&
		s->h2m_end <= A1599_VERIFIED_COMM_RAM_END &&
		s->m2h_start >= A1599_VERIFIED_COMM_RAM_START &&
		s->m2h_end <= A1599_VERIFIED_COMM_RAM_END;
#endif
}

/* The experimental authority is only the exact firmware-declared ring pair
 * observed on this firmware/device, bounded by this driver's VXD resource.
 * It is not independent evidence of the silicon's communication-RAM extent.
 */
static bool a1599_fw_abi_ring_experiment_allowed(
		const struct a1599_fw_reg_window *window,
		const struct a1599_fw_ring_snapshot *s)
{
	if (!allow_fw_abi_ring_init || !window || !s)
		return false;

	return a1599_fw_abi_ring_snapshot_matches(allow_fw_abi_ring_init,
			s->h2m_descriptor, s->m2h_descriptor,
			s->h2m_read_index, s->h2m_write_index,
			s->m2h_read_index, s->m2h_write_index,
			s->h2m_start, s->h2m_end,
			s->m2h_start, s->m2h_end,
			window->bytes);
}

/* One INIT only: exact observed firmware descriptors, explicit experiment
 * gate, empty ring, no-wrap space, and expected fixed context IOVAs are all
 * required before the first payload write.
 */

static int a1599_mmio_read32(void *context, u32 offset, u32 *value)
{
	struct a1599_fw_reg_window *window = context;

	return a1599_ring_read_reg32(window, offset, value);
}

static void a1599_mmio_write32(void *context, u32 offset, u32 value)
{
	struct a1599_fw_reg_window *window = context;

	iowrite32(value, window->base + offset);
}

static void a1599_mmio_publish_barrier(void *context)
{
	(void)context;
	/* Publish the payload before updating the firmware-visible producer index. */
	wmb();
}

static void a1599_mmio_read_barrier(void *context)
{
	(void)context;
	dma_rmb();
}

static u64 a1599_mmio_time_ns(void *context)
{
	(void)context;
	return ktime_get_ns();
}

static void a1599_mmio_sleep(void *context)
{
	(void)context;
	usleep_range(1000, 1500);
}

static const struct a1599_init_mmio_ops a1599_production_mmio_ops = {
	.read32 = a1599_mmio_read32,
	.write32 = a1599_mmio_write32,
	.publish_barrier = a1599_mmio_publish_barrier,
	.read_barrier = a1599_mmio_read_barrier,
	.time_ns = a1599_mmio_time_ns,
	.sleep_1_1_5_ms = a1599_mmio_sleep,
};

static int a1599_submit_init_once(struct device *dev,
				  struct a1599_fw_reg_window *window,
				  struct a1599_fw_static_boot_memory *memory,
				  const struct a1599_fw_ring_snapshot *before)
{
	u32 words[5], expected_regs[6], read_idx, write_idx;
	u32 observed_read = 0;
	u32 capacity;
	u64 poll_start_ns, poll_elapsed_ms;
	struct a1599_fw_ring_snapshot after;
	struct a1599_init_mmio_result mmio_result = { };
	int after_ret, poll_ret;

	if (!a1599_comm_ram_range_verified(before)) {
		if (!allow_fw_abi_ring_init)
			return -EOPNOTSUPP;
		if (!a1599_fw_abi_ring_experiment_allowed(window, before))
			return -ESTALE;
		dev_warn(dev, "INIT_RANGE_AUTHORITY=firmware-ABI-live-ring-descriptors h2m=[%#llx,%#llx) m2h=[%#llx,%#llx) resource_bounded=1 independent_silicon_comm_ram_extent=unproven experimental_gate=1\n",
			 (unsigned long long)before->h2m_start,
			 (unsigned long long)before->h2m_end,
			 (unsigned long long)before->m2h_start,
			 (unsigned long long)before->m2h_end);
	}
	if (!memory || !memory->hardware_started ||
	    a1599_fw_static_boot_memory_validate_tables(memory))
		return -EKEYREJECTED;
	if (before->h2m_read_index != before->h2m_write_index)
		return -EBUSY;
	capacity = before->h2m_capacity_words;
	read_idx = before->h2m_read_index;
	write_idx = before->h2m_write_index;
	if (!a1599_empty_ring_has_contiguous_space(capacity, read_idx,
						  write_idx,
						  ARRAY_SIZE(words)))
		return -ENOSPC;
	if (memory->context_large_vxd_iova > U32_MAX ||
	    memory->context_medium_vxd_iova > U32_MAX ||
	    memory->context_small_vxd_iova > U32_MAX)
		return -ERANGE;
	if (memory->context_large_vxd_iova != 0x40100000 ||
	    memory->context_medium_vxd_iova != 0x40300000 ||
	    memory->context_small_vxd_iova != 0x40340000)
		return -ESTALE;
	words[0] = 0x00008014;
	words[1] = memory->context_large_vxd_iova;
	words[2] = memory->context_medium_vxd_iova;
	words[3] = 0x00400100;
	words[4] = memory->context_small_vxd_iova;
	expected_regs[0] = before->h2m_descriptor;
	expected_regs[1] = before->m2h_descriptor;
	expected_regs[2] = before->h2m_read_index;
	expected_regs[3] = before->h2m_write_index;
	expected_regs[4] = before->m2h_read_index;
	expected_regs[5] = before->m2h_write_index;
	mutex_lock(&a1599_ring_lock);
	poll_ret = a1599_init_mmio_send_once(&a1599_production_mmio_ops,
			window, before->h2m_start, capacity, read_idx,
			write_idx, expected_regs, words, &mmio_result);
	mutex_unlock(&a1599_ring_lock);
	if (poll_ret) {
		if (poll_ret == -ESTALE)
			dev_warn(dev, "INIT_PREWRITE_RECHECK_MISMATCH; no ring payload/index/kick writes; no retry\n");
		else if (poll_ret == -EIO)
			pr_err("a1599-first-ready: INIT_PRODUCER_READBACK_MISMATCH expected=%u observed=%u; message was published, kick suppressed, no retry\n",
			       write_idx + (u32)ARRAY_SIZE(words),
			       mmio_result.producer_readback);
		return poll_ret;
	}
	poll_start_ns = ktime_get_ns();
	poll_ret = a1599_init_mmio_wait_consumed(&a1599_production_mmio_ops,
			window, capacity, write_idx + ARRAY_SIZE(words),
			&observed_read);
	poll_elapsed_ms = div_u64(ktime_get_ns() - poll_start_ns,
				  NSEC_PER_MSEC);
	after_ret = a1599_ring_snapshot(window, &after);
	if (!after_ret)
		a1599_log_ring_snapshot(dev, window, &after, 0, "AFTER_INIT_KICK");
	dev_info(dev, "INIT_WIRE_SEND=published_and_kicked_once producer_write_word=%u producer_readback=%u producer_readback_match=1 h2m_consumer_before_kick=%u h2m_consumer_after_poll=%u consume_poll_ret=%d consume_poll_elapsed_ms=%llu poll_timeout_ms=%u m2h_sender_readback=(%u,%u) m2h_before=(%u,%u) after_kick_snapshot=%s semantic_init_complete=unproven dedicated_ack=not_required retry=0\n",
		write_idx + (u32)ARRAY_SIZE(words), mmio_result.producer_readback,
		read_idx, observed_read, poll_ret,
		(unsigned long long)poll_elapsed_ms, A1599_INIT_CONSUME_TIMEOUT_MS,
		mmio_result.m2h_readback, mmio_result.m2h_writeback,
		before->m2h_read_index, before->m2h_write_index,
		!after_ret ? "see-RING_SNAPSHOT_AFTER_INIT_KICK" : "unavailable");
	if (poll_ret)
		dev_warn(dev, "INIT_CONSUME_UNCONFIRMED ret=%d observed_h2m_read=%u; message was already published and kicked once; no retry/reset/unload; resources remain pinned\n",
			 poll_ret, observed_read);
	if (after_ret)
		dev_warn(dev, "INIT_POST_KICK_SNAPSHOT_FAILED ret=%d; no retry/reset/unload; resources remain pinned\n",
			 after_ret);
	if (!after_ret)
		dev_info(dev, "INIT_INDEX_OBSERVATIONS h2m_consumer_advanced=%d h2m_read_before=%u h2m_read_after=%u m2h_indices_changed=%d m2h_read_before=%u m2h_write_before=%u m2h_read_after=%u m2h_write_after=%u; these are observations only\n",
			after.h2m_read_index != before->h2m_read_index,
			before->h2m_read_index, after.h2m_read_index,
			(after.m2h_read_index != before->m2h_read_index) ||
			(after.m2h_write_index != before->m2h_write_index),
			before->m2h_read_index, before->m2h_write_index,
			after.m2h_read_index, after.m2h_write_index);
	return poll_ret ?: after_ret;
}

static ssize_t ring_snapshot_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct a1599_fw_ring_snapshot s;
	int ret, len;

	if (!READ_ONCE(a1599_first_ready_live) || dev != READ_ONCE(a1599_live_device))
		return -EAGAIN;
	ret = a1599_ring_snapshot(&a1599_live_window, &s);
	a1599_log_ring_snapshot(dev, &a1599_live_window, &s, ret, "SYSFS_READ");
	len = sysfs_emit(buf, "validation=%d h2m_desc=%#08x m2h_desc=%#08x\n",
			 ret, s.h2m_descriptor, s.m2h_descriptor);
	len += sysfs_emit_at(buf, len,
		"h2m_offset_bytes=%#x h2m_capacity_words=%u h2m_read_word=%u h2m_write_word=%u\n",
		s.h2m_offset_bytes, s.h2m_capacity_words,
		s.h2m_read_index, s.h2m_write_index);
	len += sysfs_emit_at(buf, len,
		"m2h_offset_bytes=%#x m2h_capacity_words=%u m2h_read_word=%u m2h_write_word=%u\n",
		s.m2h_offset_bytes, s.m2h_capacity_words,
		s.m2h_read_index, s.m2h_write_index);
	return len + sysfs_emit_at(buf, len,
		"bounds_h2m=[%#llx,%#llx) bounds_m2h=[%#llx,%#llx) comm_ram_proven=%d async_payload_captured=0\n",
		(unsigned long long)s.h2m_start, (unsigned long long)s.h2m_end,
		(unsigned long long)s.m2h_start, (unsigned long long)s.m2h_end,
		a1599_comm_ram_range_verified(&s));
}

static ssize_t init_once_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct a1599_fw_ring_snapshot s;
	int ret;

	if (!sysfs_streq(buf, "1"))
		return -EINVAL;
	if (!READ_ONCE(a1599_first_ready_live) || dev != READ_ONCE(a1599_live_device))
		return -EAGAIN;
	if (atomic_cmpxchg(&a1599_init_consumed, 0, 1))
		return -EALREADY;
	ret = a1599_ring_snapshot(&a1599_live_window, &s);
	a1599_log_ring_snapshot(dev, &a1599_live_window, &s, ret, "INIT_TRIGGER");
	if (ret)
		return ret;
	ret = a1599_submit_init_once(dev, &a1599_live_window,
				    a1599_memory, &s);
	if (ret) {
		dev_warn(dev, "INIT_ONCE_STOPPED ret=%d; one-shot consumed, no retry or decode; resources stay pinned\n",
			 ret);
		return ret;
	}
	/* The H2M consumer advanced through INIT; no semantic ACK is established. */
	atomic_set(&a1599_init_transport_consumed, 1);
	dev_info(dev, "INIT_TRANSPORT_CONSUMED=1 semantic_init_complete=unproven\n");
	return count;
}

static void a1599_log_decode_iova_map(struct device *dev, unsigned int index,
		const char *name, u32 iova, size_t bytes, dma_addr_t *system_dma)
{
	dev_info(dev, "ID81_RESOURCE_MAP index=%u name=%s iova=%#x bytes=%zu system_dma=%pad\n",
		 index, name, iova, bytes, system_dma);
}

static void a1599_log_decode_resources(struct device *dev,
				       struct a1599_fw_static_boot_memory *memory)
{
	static const char * const decode_names[A1599_DECODE_RESOURCE_COUNT] = {
		"preload", "vlc", "bitstream", "current_picture",
		"intra", "output", "command",
	};
	unsigned int i;

	a1599_log_decode_iova_map(dev, 0, "staging",
		memory->staging_vxd_iova, memory->staging.bytes,
		&memory->staging.dma);
	a1599_log_decode_iova_map(dev, 1, "extra",
		memory->extra_vxd_iova, memory->extra.bytes,
		&memory->extra.dma);
	a1599_log_decode_iova_map(dev, 2, "context_large",
		memory->context_large_vxd_iova, memory->context_large.bytes,
		&memory->context_large.dma);
	a1599_log_decode_iova_map(dev, 3, "context_medium",
		memory->context_medium_vxd_iova, memory->context_medium.bytes,
		&memory->context_medium.dma);
	a1599_log_decode_iova_map(dev, 4, "context_small",
		memory->context_small_vxd_iova, memory->context_small.bytes,
		&memory->context_small.dma);
	for (i = 0; i < A1599_DECODE_RESOURCE_COUNT; i++)
		a1599_log_decode_iova_map(dev, 5 + i, decode_names[i],
			memory->decode_vxd_iova[i], memory->decode[i].bytes,
			&memory->decode[i].dma);
	dev_info(dev, "ID81_PAGE_TABLE root_dma=%pad root_bytes=%zu tables_crc32=%#x\n",
		 &memory->root.dma, memory->root.bytes, memory->tables_crc32);
	for (i = 0; i < A1599_FW_STATIC_LEAF_PAGES; i++)
		dev_info(dev, "ID81_PAGE_TABLE_LEAF index=%u root_index=%u dma=%pad bytes=%zu\n",
			 i, memory->leaf_root_index[i], &memory->leaf[i].dma,
			 memory->leaf[i].bytes);
}

static void a1599_log_decode_command(struct device *dev,
				     struct a1599_fw_static_boot_memory *memory)
{
	struct a1599_fw_static_allocation *command =
		&memory->decode[A1599_DECODE_COMMAND];

	dev_info(dev, "ID81_COMMAND_BACKING bytes=%u words=%u iova=%#x dump_bytes=%u\n",
		 A1599_DECODE_STREAM_BYTES, A1599_DECODE_STREAM_WORDS,
		 memory->decode_vxd_iova[A1599_DECODE_COMMAND],
		 A1599_DECODE_STREAM_BYTES);
	print_hex_dump(KERN_INFO, "a1599-first-ready ID81_COMMAND ",
		       DUMP_PREFIX_OFFSET, 16, 1, command->cpu,
		       A1599_DECODE_STREAM_BYTES, false);
}

static void a1599_log_c0_capture(struct device *dev,
				 const struct a1599_decode_submit_result *result,
				 const struct a1599_decode_diagnostic *diagnostic,
				 bool guard_checked)
{
	dev_info(dev, "C0_CAPTURE actual_length=%u raw_length=%zu response_received=%d record_valid=%d message_read=%u message_write=%u last_mb_present=%d last_mb_offset=+4 last_mb=%#x flags_present=%d flags_offset=+8 flags=%#x detail_present=%d detail_offset=+12 detail=%#x parsed_detail_present=%u guard_checked=%d guard_state=%s\n",
		 result->message.length, diagnostic->raw_length,
		 result->response_received,
		 result->response_record_valid, result->message.read_index,
		 result->message.write_index, diagnostic->last_mb_present,
		 diagnostic->last_mb, diagnostic->status_present,
		 diagnostic->status, diagnostic->detail_present,
		 diagnostic->detail, result->response.has_detail,
		 guard_checked,
		 a1599_guard_state_name(diagnostic->guard_state));
	if (diagnostic->raw_length)
		print_hex_dump(KERN_INFO, "a1599-first-ready C0_RAW ",
			       DUMP_PREFIX_OFFSET, 16, 1,
			       diagnostic->raw, diagnostic->raw_length, false);
}

static ssize_t decode_once_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct a1599_fw_ring_snapshot before_decode;
	struct a1599_fw_ring_snapshot after_decode;
	struct a1599_decode_ring_snapshot snapshot;
	struct a1599_decode_submit_result result;
	struct a1599_decode_diagnostic diagnostic;
	struct id81_pack_input pack;
	u8 *output;
	bool guard_checked = false, guard_intact = false;
	bool output_stability_checked = false, output_stable = false;
	int ret, snap_ret, after_ret;

	if (!sysfs_streq(buf, "1"))
		return -EINVAL;
	if (!allow_decode_submission)
		return -EACCES;
	/* Single-IDR type-1 producer: R16=1, last group 0x4004, mappingdirty 0x10. */
	if (decode_flags != 0x4015U)
		return -EINVAL;
	if (id81_byte0_bit7 != 0)
		return -EINVAL;
	if (!READ_ONCE(a1599_first_ready_live) || dev != READ_ONCE(a1599_live_device))
		return -EAGAIN;
	if (!atomic_read(&a1599_init_transport_consumed))
		return -EPERM;
	if (!a1599_memory || !a1599_memory->hardware_started ||
	    a1599_fw_static_boot_memory_validate_tables(a1599_memory))
		return -EKEYREJECTED;
	snap_ret = a1599_ring_snapshot(&a1599_live_window, &before_decode);
	if (snap_ret)
		return snap_ret;
	if (before_decode.h2m_descriptor != 0x60cc01cc ||
	    before_decode.m2h_descriptor != 0x60000033 ||
	    before_decode.h2m_start != 0x80cc || before_decode.h2m_end != 0x87fc ||
	    before_decode.m2h_start != 0x8000 || before_decode.m2h_end != 0x80cc ||
	    before_decode.h2m_read_index != before_decode.h2m_write_index ||
	    before_decode.m2h_read_index != before_decode.m2h_write_index)
		return -ESTALE;
	if (atomic_cmpxchg(&a1599_decode_consumed, 0, 1))
		return -EALREADY;
	a1599_log_ring_snapshot(dev, &a1599_live_window, &before_decode, 0,
				"BEFORE_ID81");

	snapshot = (struct a1599_decode_ring_snapshot) {
		.h2m_descriptor = before_decode.h2m_descriptor,
		.m2h_descriptor = before_decode.m2h_descriptor,
		.h2m_read_index = before_decode.h2m_read_index,
		.h2m_write_index = before_decode.h2m_write_index,
		.m2h_read_index = before_decode.m2h_read_index,
		.m2h_write_index = before_decode.m2h_write_index,
		.h2m_capacity_words = before_decode.h2m_capacity_words,
		.m2h_capacity_words = before_decode.m2h_capacity_words,
		.h2m_start = before_decode.h2m_start,
		.m2h_start = before_decode.m2h_start,
	};
	pack = (struct id81_pack_input) {
		.preserve_byte0_bit7 = id81_byte0_bit7,
		.group_sequence = 1,
		.flags = decode_flags,
		.command_iova = a1599_memory->decode_vxd_iova[A1599_DECODE_COMMAND],
		.command_bytes = A1599_DECODE_STREAM_BYTES,
		.mapping_iova = a1599_memory->decode_vxd_iova[A1599_DECODE_COMMAND],
		.mapping_bytes = a1599_memory->decode[A1599_DECODE_COMMAND].bytes,
		.root_device_address = a1599_memory->root.dma,
		.selector = 0,
		.opaque = 0x10211000,
	};
	a1599_log_decode_resources(dev, a1599_memory);
	a1599_log_decode_command(dev, a1599_memory);
	/* Publish all coherent command/source writes before the producer update. */
	dma_wmb();
	mutex_lock(&a1599_ring_lock);
	ret = a1599_decode_submit_once(&a1599_production_mmio_ops,
		&a1599_live_window, &snapshot, &pack, 1,
		A1599_DECODE_RESPONSE_TIMEOUT_MS, &result);
	mutex_unlock(&a1599_ring_lock);
	dev_info(dev, "ID81_ENVELOPE published=%d bytes=%u\n",
		 result.published, ID81_MESSAGE_BYTES);
	print_hex_dump(KERN_INFO, "a1599-first-ready ID81_ENVELOPE ",
		       DUMP_PREFIX_OFFSET, 16, 1, result.envelope,
		       ID81_MESSAGE_BYTES, false);
	after_ret = a1599_ring_snapshot(&a1599_live_window, &after_decode);
	if (after_ret)
		dev_warn(dev, "RING_SNAPSHOT_AFTER_ID81 ret=%d\n", after_ret);
	else
		a1599_log_ring_snapshot(dev, &a1599_live_window, &after_decode, 0,
					"AFTER_ID81");
	output = a1599_memory->decode[A1599_DECODE_OUTPUT].cpu;
	if (result.response_received && result.response_record_valid &&
	    result.response.status == 0) {
		guard_checked = true;
		guard_intact = !memchr_inv(output + 3072U, 0xa5, 1024U);
	}
	a1599_decode_diagnostic_capture_c0(&diagnostic,
		result.message.bytes, result.message.length,
		result.response_record_valid, guard_checked, guard_intact);
	WRITE_ONCE(a1599_decode_output_snapshot_ready, false);
	if (!ret && diagnostic.guard_state == A1599_GUARD_INTACT &&
	    allow_output_export) {
		/* Compare two complete backing reads 50 ms apart, then export only
		 * the immutable kernel snapshot. This is bounded evidence of a
		 * quiescent interval, not proof the device will never DMA again.
		 */
		msleep(A1599_DECODE_OUTPUT_SETTLE_MS);
		dma_rmb();
		memcpy(a1599_decode_output_snapshot, output,
		       A1599_FW_DECODE_OUTPUT_BYTES);
		msleep(A1599_DECODE_OUTPUT_SETTLE_MS);
		dma_rmb();
		output_stability_checked = true;
		guard_intact = !memchr_inv(output + 3072U, 0xa5, 1024U);
		output_stable = guard_intact &&
			!memcmp(output, a1599_decode_output_snapshot,
				A1599_FW_DECODE_OUTPUT_BYTES);
		a1599_decode_diagnostic_capture_c0(&diagnostic,
			result.message.bytes, result.message.length,
			result.response_record_valid, guard_checked, guard_intact);
		if (output_stable)
			WRITE_ONCE(a1599_decode_output_snapshot_ready, true);
	}
	a1599_log_c0_capture(dev, &result, &diagnostic, guard_checked);
	WRITE_ONCE(a1599_decode_result_valid,
		   result.response_received && result.response_record_valid);
	WRITE_ONCE(a1599_decode_response_zero_status,
		   ret == 0 && result.response_received &&
		   result.response_record_valid &&
		   result.response.status == 0 && guard_intact &&
		   READ_ONCE(a1599_decode_output_snapshot_ready));
	dev_warn(dev, "DECODE_ONE_SHOT consumed=1 ret=%d published=%d h2m_consumed=%d response_received=%d fence=%u expected_fence=1 c0_record_valid=%d c0_status_class=%s c0_status_error=%d c0_status=%#x c0_detail_present=%u c0_detail=%#x guard_checked=%d guard_state=%s output_snapshot_ready=%d output_stability_checked=%d output_stable_50ms=%d pixels_compared=0 decode_success=unclaimed output_dma=%pad output_bytes=%zu response_ring_consumer_advanced=0 retry=0 init_transport_consumed=1 semantic_init_complete=unproven resources_pinned=1\n",
		 ret, result.published, result.consumed,
		 result.response_received, result.response.fence,
		 result.response_record_valid,
		 !result.response_record_valid ? "no-valid-record" :
		 result.status_class == A1599_DECODE_C0_STATUS_ERROR_DETAIL ?
		 "error-detail" : result.status_class ==
		 A1599_DECODE_C0_STATUS_NONZERO ? "nonzero-no-detail" : "zero",
		 result.response_error, result.response.status,
		 result.response.has_detail,
		 result.response.detail,
		 guard_checked, a1599_guard_state_name(diagnostic.guard_state),
		 READ_ONCE(a1599_decode_output_snapshot_ready),
		 output_stability_checked, output_stable,
		 &a1599_memory->decode[A1599_DECODE_OUTPUT].dma,
		 a1599_memory->decode[A1599_DECODE_OUTPUT].bytes);
	return ret ? ret : count;
}

/* Export only a fixed-size immutable snapshot of the output backing, never an
 * address or another resource. A full-frame read requires a zero-status C0,
 * an intact guard, and two equal DMA-buffer observations 50 ms apart. This
 * bounded stability check does not prove that later DMA cannot occur.
 */
static ssize_t decode_output_read(struct file *file, struct kobject *kobj,
				  const struct bin_attribute *attr, char *buf,
				  loff_t offset, size_t count)
{
	struct device *dev = kobj_to_dev(kobj);
	struct a1599_fw_static_allocation *output;
	size_t available;

	(void)file;
	(void)attr;
	if (!allow_output_export)
		return -EACCES;
	if (!READ_ONCE(a1599_first_ready_live) || dev != READ_ONCE(a1599_live_device) ||
	    !a1599_memory || !a1599_memory->hardware_started ||
	    !atomic_read(&a1599_decode_consumed))
		return -EAGAIN;
	if (!READ_ONCE(a1599_decode_result_valid) ||
	    !READ_ONCE(a1599_decode_response_zero_status) ||
	    !READ_ONCE(a1599_decode_output_snapshot_ready))
		return -ENODATA;
	if (offset < 0 || offset > A1599_FW_DECODE_OUTPUT_BYTES)
		return -EINVAL;
	if (!count || offset == A1599_FW_DECODE_OUTPUT_BYTES)
		return 0;
	output = &a1599_memory->decode[A1599_DECODE_OUTPUT];
	if (!output->cpu || output->bytes != A1599_FW_DECODE_OUTPUT_BYTES)
		return -EIO;
	if (memchr_inv((u8 *)output->cpu + 3072U, 0xa5, 1024U))
		return -EIO;
	dma_rmb();
	available = A1599_FW_DECODE_OUTPUT_BYTES - (size_t)offset;
	if (count > available)
		count = available;
	memcpy(buf, a1599_decode_output_snapshot + offset, count);
	return count;
}

static BIN_ATTR_RO(decode_output, A1599_FW_DECODE_OUTPUT_BYTES);

/* One-shot host-side diagnostic capture. The existing driver mutex excludes
 * concurrent CPU ring transactions through this driver. It cannot stop the
 * firmware from writing asynchronously; unchanged producer indexes are not a
 * proof that firmware has no in-flight payload store. */
static ssize_t m2h_ring_raw_once_read(struct file *file, struct kobject *kobj,
				      const struct bin_attribute *attr,
				      char *buf, loff_t offset, size_t count)
{
	struct device *dev = kobj_to_dev(kobj);
	struct a1599_fw_ring_snapshot before, after;
	u8 capture[32];
	unsigned int i;
	int ret, after_ret;

	(void)file;
	(void)attr;
	if (offset != 0 || count != sizeof(capture))
		return -EINVAL;
	if (!READ_ONCE(a1599_first_ready_live) ||
	    dev != READ_ONCE(a1599_live_device) ||
	    !atomic_read(&a1599_init_transport_consumed) ||
	    !atomic_read(&a1599_decode_consumed))
		return -EAGAIN;
	if (atomic_cmpxchg(&a1599_m2h_raw_consumed, 0, 1))
		return -EALREADY;

	mutex_lock(&a1599_ring_lock);
	ret = a1599_ring_snapshot_locked(&a1599_live_window, &before);
	if (ret)
		goto out_unlock;
	if (before.h2m_descriptor != 0x60cc01cc ||
	    before.m2h_descriptor != 0x60000033 ||
	    before.h2m_read_index != 10 || before.h2m_write_index != 10 ||
	    before.m2h_read_index != 0 || before.m2h_write_index != 8 ||
	    before.m2h_start != 0x8000 || before.m2h_end != 0x80cc) {
		ret = -ESTALE;
		goto out_unlock;
	}
	dma_rmb();
	for (i = 0; i < ARRAY_SIZE(capture) / sizeof(u32); i++) {
		u32 word = readl(a1599_live_window.base + 0x8000 + i * sizeof(u32));

		put_unaligned_le32(word, capture + i * sizeof(u32));
	}
	after_ret = a1599_ring_snapshot_locked(&a1599_live_window, &after);
	if (after_ret || before.h2m_descriptor != after.h2m_descriptor ||
	    before.m2h_descriptor != after.m2h_descriptor ||
	    before.h2m_read_index != after.h2m_read_index ||
	    before.h2m_write_index != after.h2m_write_index ||
	    before.m2h_read_index != after.m2h_read_index ||
	    before.m2h_write_index != after.m2h_write_index ||
	    before.m2h_start != after.m2h_start || before.m2h_end != after.m2h_end) {
		dev_warn(dev, "M2H_RAW_ONCE rejected before_ret=0 after_ret=%d producer_before=%u producer_after=%u consumer_before=%u consumer_after=%u retry=0 firmware_concurrency_excluded=0\n",
			 after_ret, before.m2h_write_index, after.m2h_write_index,
			 before.m2h_read_index, after.m2h_read_index);
		ret = after_ret ? after_ret : -ESTALE;
		goto out_unlock;
	}
	dev_info(dev, "M2H_RAW_ONCE captured_bytes=32 words=8 bounds=[0x8000,0x8020) producer_before=%u producer_after=%u consumer_before=%u consumer_after=%u host_driver_serialized=1 firmware_concurrency_excluded=0 consumer_advanced=0 retry=0\n",
		 before.m2h_write_index, after.m2h_write_index,
		 before.m2h_read_index, after.m2h_read_index);
	memcpy(buf, capture, sizeof(capture));
	ret = sizeof(capture);
out_unlock:
	mutex_unlock(&a1599_ring_lock);
	return ret;
}

static BIN_ATTR_RO(m2h_ring_raw_once, 32);

static DEVICE_ATTR_RO(ring_snapshot);
static DEVICE_ATTR_WO(init_once);
static DEVICE_ATTR_WO(decode_once);

static void a1599_install_runtime_controls(struct device *dev,
					  struct a1599_fw_reg_window *window)
{
	int snapshot_ret, init_ret, decode_ret;

	a1599_live_window = *window;
	WRITE_ONCE(a1599_live_device, dev);
	WRITE_ONCE(a1599_first_ready_live, true);
	snapshot_ret = device_create_file(dev, &dev_attr_ring_snapshot);
	if (snapshot_ret)
		dev_warn(dev, "ring_snapshot sysfs control unavailable ret=%d\n",
			 snapshot_ret);
	init_ret = device_create_file(dev, &dev_attr_init_once);
	if (init_ret)
		dev_warn(dev, "init_once sysfs control unavailable ret=%d\n",
			 init_ret);
	decode_ret = device_create_file(dev, &dev_attr_decode_once);
	if (decode_ret)
		dev_warn(dev, "decode_once sysfs control unavailable ret=%d\n",
			 decode_ret);
	decode_ret = sysfs_create_bin_file(&dev->kobj, &bin_attr_decode_output);
	if (decode_ret)
		dev_warn(dev, "decode_output sysfs export unavailable ret=%d\n",
			 decode_ret);
	decode_ret = sysfs_create_bin_file(&dev->kobj, &bin_attr_m2h_ring_raw_once);
	if (decode_ret)
		dev_warn(dev, "m2h_ring_raw_once sysfs control unavailable ret=%d\n",
			 decode_ret);
}

static bool a1599_blob_contains(const char *needle)
{
	size_t needle_len = strlen(needle);
	size_t i;

	if (!needle_len || needle_len > sizeof(a1599_first_ready_dtbo))
		return false;
	for (i = 0; i <= sizeof(a1599_first_ready_dtbo) - needle_len; i++)
		if (!memcmp(a1599_first_ready_dtbo + i, needle, needle_len))
			return true;
	return false;
}

static int a1599_validate_vdec_provider(struct device_node **out)
{
	struct device_node *node, *parent;
	struct of_phandle_args args = {};
	const char *label;
	u32 reg;
	int ret;

	if (!out)
		return -EINVAL;
	*out = NULL;
	node = of_find_node_by_path(A1599_VDEC_PATH);
	if (!node)
		return -ENOENT;
	if (!of_device_is_compatible(node, "apple,s5l8960x-pmgr-pwrstate") ||
	    of_property_read_string(node, "label", &label) ||
	    strcmp(label, "vdec") || of_property_read_u32(node, "reg", &reg) ||
	    reg != 0x201f0 || of_find_property(node, "phandle", NULL) ||
	    of_find_property(node, "linux,phandle", NULL)) {
		ret = -EINVAL;
		goto fail_node;
	}
	parent = of_find_node_by_path(A1599_MEDIA_PATH);
	if (!parent) {
		ret = -ENOENT;
		goto fail_node;
	}
	ret = of_parse_phandle_with_args(node, "power-domains",
					 "#power-domain-cells", 0, &args);
	if (ret || args.np != parent || args.args_count != 0) {
		if (!ret)
			of_node_put(args.np);
		of_node_put(parent);
		ret = -EINVAL;
		goto fail_node;
	}
	of_node_put(args.np);
	of_node_put(parent);
	*out = node;
	return 0;

fail_node:
	of_node_put(node);
	return ret;
}

static int a1599_live_tree_preflight(void)
{
	struct device_node *soc, *vxd, *symbols, *vdec = NULL;
	bool has_symbols;
	int ret;

	soc = of_find_node_by_path("/soc");
	if (!soc)
		return -ENOENT;
	of_node_put(soc);
	vxd = of_find_node_by_path(A1599_VXD_PATH);
	if (vxd) {
		of_node_put(vxd);
		return -EBUSY;
	}
	symbols = of_find_node_by_path("/__symbols__");
	has_symbols = !!symbols;
	if (symbols)
		of_node_put(symbols);
	/* The embedded overlay is target-path-only. It must never depend on the
	 * base DT's optional symbol export or external label fixups.
	 */
	if (a1599_blob_contains("__symbols__") ||
	    a1599_blob_contains("__fixups__") ||
	    a1599_blob_contains("__local_fixups__"))
		return -EOPNOTSUPP;
	ret = a1599_validate_vdec_provider(&vdec);
	if (ret)
		return ret;
	of_node_put(vdec);
	pr_info("a1599-first-ready: LIVE_DT_PREFLIGHT soc=present vxd_node=absent vdec_provider=verified-by-path provider_phandle=absent live_symbols=%s overlay_fixups=none\n",
		has_symbols ? "present" : "absent");
	return 0;
}

static int a1599_pm_attach_and_resume(struct device *dev)
{
	struct of_phandle_args args = {};
	int ret;

	if (!dev || dev->pm_domain || pm_runtime_enabled(dev) ||
	    !pm_runtime_status_suspended(dev) ||
	    atomic_read(&dev->power.usage_count))
		return -EUCLEAN;
	ret = a1599_validate_vdec_provider(&a1599_vdec_node);
	if (ret)
		return ret;
	args.np = a1599_vdec_node;
	args.args_count = 0;
	ret = of_genpd_add_device(&args, dev);
	if (ret) {
		of_node_put(a1599_vdec_node);
		a1599_vdec_node = NULL;
		return ret;
	}
	a1599_genpd_attached = true;
	if (!dev->pm_domain || dev_pm_genpd_is_on(dev) ||
	    !pm_runtime_status_suspended(dev) || pm_runtime_enabled(dev) ||
	    atomic_read(&dev->power.usage_count))
		return -EUCLEAN;
	pr_info("a1599-first-ready: VDEC_GENPD_ATTACHED provider=verified-live-node domain_on=0 runtime_pm=disabled usage=0\n");

	ret = pm_runtime_set_suspended(dev);
	if (ret)
		return ret;
	pm_runtime_enable(dev);
	a1599_runtime_pm_enabled = true;
	pr_info("a1599-first-ready: VDEC_RUNTIME_PM_ENABLED active=0 domain_on=%d usage=%d\n",
		dev_pm_genpd_is_on(dev), atomic_read(&dev->power.usage_count));
	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		return ret;
	a1599_pm_reference_held = true;
	if (!pm_runtime_active(dev) ||
	    atomic_read(&dev->power.usage_count) != 1 ||
	    !dev_pm_genpd_is_on(dev))
		return -EUCLEAN;
	pr_info("a1599-first-ready: VDEC_PM_RESUME_HELD ret=0 active=1 domain_on=1 usage=1\n");
	return 0;
}

static int a1599_pm_release_before_start(struct device *dev)
{
	int ret;

	if (a1599_pm_reference_held) {
		ret = pm_runtime_put_sync_suspend(dev);
		a1599_pm_reference_held = false;
		if (ret)
			return ret < 0 ? ret : -EIO;
	}
	if ((a1599_runtime_pm_enabled || a1599_genpd_attached) &&
	    (atomic_read(&dev->power.usage_count) ||
	     !pm_runtime_status_suspended(dev) || dev_pm_genpd_is_on(dev)))
		return -EBUSY;
	if (a1599_runtime_pm_enabled) {
		pm_runtime_disable(dev);
		a1599_runtime_pm_enabled = false;
		if (pm_runtime_enabled(dev))
			return -EIO;
	}
	if (a1599_genpd_attached) {
		ret = pm_genpd_remove_device(dev);
		if (ret)
			return ret;
		a1599_genpd_attached = false;
	}
	if (a1599_vdec_node) {
		of_node_put(a1599_vdec_node);
		a1599_vdec_node = NULL;
	}
	pr_info("a1599-first-ready: VDEC_PM_RELEASED active=0 domain_on=0 usage=0 genpd_detached=1\n");
	return 0;
}

static int a1599_sha256_matches(const struct firmware *fw)
{
	struct crypto_shash *tfm;
	struct shash_desc *desc;
	u8 digest[SHA256_DIGEST_SIZE];
	static const u8 expected[SHA256_DIGEST_SIZE] = {
		0x6b, 0x27, 0x4c, 0x57, 0x6a, 0x79, 0x52, 0xd7,
		0xa4, 0xf2, 0x2f, 0x65, 0xad, 0xf5, 0xb1, 0x04,
		0x1c, 0x43, 0x29, 0xc3, 0xdf, 0xc7, 0xce, 0x76,
		0x1d, 0xe5, 0x36, 0x68, 0xcf, 0xd4, 0xa4, 0x1e,
	};
	int ret;

	if (!fw || fw->size != A1599_FW_BYTES)
		return -EINVAL;
	tfm = crypto_alloc_shash("sha256", 0, 0);
	if (IS_ERR(tfm))
		return PTR_ERR(tfm);
	desc = kmalloc(sizeof(*desc) + crypto_shash_descsize(tfm), GFP_KERNEL);
	if (!desc) {
		crypto_free_shash(tfm);
		return -ENOMEM;
	}
	desc->tfm = tfm;
	ret = crypto_shash_digest(desc, fw->data, fw->size, digest);
	if (!ret && crypto_memneq(digest, expected, sizeof(expected)))
		ret = -EKEYREJECTED;
	kfree(desc);
	crypto_free_shash(tfm);
	return ret;
}

static void a1599_fill_experimental_evidence(
		struct a1599_fw_first_ready_layout *layout)
{
	layout->evidence.firmware_image = A1599_FW_EVIDENCE_REVIEWED;
	layout->evidence.target_version_delta = A1599_FW_EVIDENCE_REVIEWED;
	layout->evidence.vxd_resource = A1599_FW_EVIDENCE_REVIEWED;
	layout->evidence.register_access_contract = A1599_FW_EVIDENCE_REVIEWED;
	layout->evidence.io_ordering_contract = A1599_FW_EVIDENCE_EXPERIMENTAL_ASSUMPTION;
	layout->evidence.clock_reset_sequence = A1599_FW_EVIDENCE_EXPERIMENTAL_ASSUMPTION;
	layout->evidence.system_dma_route = A1599_FW_EVIDENCE_EXPERIMENTAL_ASSUMPTION;
	layout->evidence.vxd_internal_mmu_contract = A1599_FW_EVIDENCE_EXPERIMENTAL_ASSUMPTION;
	layout->evidence.vxd_static_table_visibility = A1599_FW_EVIDENCE_EXPERIMENTAL_ASSUMPTION;
	layout->evidence.vxd_initial_tlb_state = A1599_FW_EVIDENCE_EXPERIMENTAL_ASSUMPTION;
}

static int a1599_first_ready_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct a1599_fw_static_route route = {
		.dma_route = A1599_FW_EVIDENCE_EXPERIMENTAL_ASSUMPTION,
		.dma_addr_mask = A1599_FW_DMA_ALLOC_MASK,
		.pte_dma_domain = A1599_FW_EVIDENCE_EXPERIMENTAL_ASSUMPTION,
	};
	struct a1599_fw_fixed_mapping fixed_maps[A1599_FW_STATIC_TOTAL_REQUIRED_MAPS];
	struct a1599_fw_reg_window window = {};
	struct a1599_fw_first_ready_layout layout = {};
	const struct firmware *fw = NULL;
	struct resource *res;
	u64 mask, preflight_start_ns = ktime_get_ns(), operation_start_ns, elapsed_ms;
	int ret, pm_ret;

	if (atomic_cmpxchg(&a1599_state, A1599_HARNESS_IDLE,
			   A1599_HARNESS_PREFLIGHT) != A1599_HARNESS_IDLE)
		return -EALREADY;
	if (!run_once || !dev->of_node ||
	    !of_device_is_compatible(dev->of_node,
				     "apple,s5l8960x-vxd-first-ready-experiment")) {
		ret = -ENODEV;
		goto fail_before_start;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res || res->start != A1599_VXD_RESOURCE_START ||
	    resource_size(res) != A1599_VXD_RESOURCE_BYTES ||
	    platform_get_resource(pdev, IORESOURCE_MEM, 1)) {
		ret = -EINVAL;
		goto fail_before_start;
	}
	/* Claims the exact resource exclusively; no MMIO occurs during mapping. */
	window.base = devm_ioremap_resource(dev, res);
	if (IS_ERR(window.base)) {
		ret = PTR_ERR(window.base);
		window.base = NULL;
		goto fail_before_start;
	}
	window.dev = dev;
	window.bytes = resource_size(res);

	if (dev->pm_domain || pm_runtime_enabled(dev) ||
	    !pm_runtime_status_suspended(dev) ||
	    atomic_read(&dev->power.usage_count)) {
		ret = -EUCLEAN;
		goto fail_before_start;
	}
	mask = dma_get_mask(dev);
	dev_info(dev, "OF DMA preflight: mask=%#llx coherent_mask=%#llx coherent=%d dma_range_map=%s iommu_domain=%s pm_domain_unattached=1 runtime_suspended=1 resource=[%pa-%pa] exclusive=devm_ioremap_resource\n",
		 (unsigned long long)mask,
		 (unsigned long long)dev->coherent_dma_mask,
		 dev_is_dma_coherent(dev), dev->dma_range_map ? "present" : "absent",
		 iommu_get_domain_for_dev(dev) ? "present" : "absent",
		 &res->start, &res->end);
	/* Match the successful 590d baseline: require the observed OF defaults,
	 * then temporarily widen to the 36-bit allocation mask in the allocator.
	 */
	ret = a1599_fw_dma_probe_validate(&(struct a1599_fw_dma_probe_snapshot) {
		.streaming_mask = mask,
		.coherent_mask = dev->coherent_dma_mask,
		.device_coherent = dev_is_dma_coherent(dev),
		.has_dma_range_map = !!dev->dma_range_map,
		.has_iommu_domain = !!iommu_get_domain_for_dev(dev),
	});
	if (ret)
		goto fail_before_start;

	ret = request_firmware_direct(&fw, A1599_FW_NAME, dev);
	if (ret)
		goto fail_before_start;
	ret = a1599_sha256_matches(fw);
	if (ret)
		goto fail_before_start;

	a1599_memory = kzalloc_obj(*a1599_memory, GFP_KERNEL);
	if (!a1599_memory) {
		ret = -ENOMEM;
		goto fail_before_start;
	}
	ret = a1599_fw_static_boot_memory_alloc_experimental(dev, &route,
							     a1599_memory);
	if (ret)
		goto fail_before_start;
	fixed_maps[0] = (struct a1599_fw_fixed_mapping) {
		.vxd_iova = A1599_FW_STATIC_STAGING_IOVA,
		.system_dma_addr = a1599_memory->staging.dma,
		.bytes = a1599_memory->staging.bytes,
	};
	fixed_maps[1] = (struct a1599_fw_fixed_mapping) {
		.vxd_iova = A1599_FW_STATIC_EXTRA_IOVA,
		.system_dma_addr = a1599_memory->extra.dma,
		.bytes = a1599_memory->extra.bytes,
	};
	fixed_maps[2] = (struct a1599_fw_fixed_mapping) {
		.vxd_iova = A1599_FW_STATIC_CONTEXT_LARGE_IOVA,
		.system_dma_addr = a1599_memory->context_large.dma,
		.bytes = a1599_memory->context_large.bytes,
	};
	fixed_maps[3] = (struct a1599_fw_fixed_mapping) {
		.vxd_iova = A1599_FW_STATIC_CONTEXT_MEDIUM_IOVA,
		.system_dma_addr = a1599_memory->context_medium.dma,
		.bytes = a1599_memory->context_medium.bytes,
	};
	fixed_maps[4] = (struct a1599_fw_fixed_mapping) {
		.vxd_iova = A1599_FW_STATIC_CONTEXT_SMALL_IOVA,
		.system_dma_addr = a1599_memory->context_small.dma,
		.bytes = A1599_FW_STATIC_CONTEXT_SMALL_BACKING,
	};
	ret = a1599_prepare_decode_map_entries(a1599_memory, fixed_maps);
	if (ret)
		goto fail_before_start;
	ret = a1599_fw_first_ready_build_tables_experimental(&route,
							     a1599_memory,
							     fixed_maps,
							     ARRAY_SIZE(fixed_maps));
	if (ret)
		goto fail_before_start;
	ret = a1599_populate_decode_buffers(dev, a1599_memory, fixed_maps);
	if (ret)
		goto fail_before_start;
	elapsed_ms = div_u64(ktime_get_ns() - preflight_start_ns, NSEC_PER_MSEC);
	if (elapsed_ms > A1599_PREFLIGHT_BUDGET_MS) {
		ret = -ETIMEDOUT;
		goto fail_before_start;
	}

	layout.firmware_image = fw->data;
	layout.firmware_image_bytes = fw->size;
	layout.upload_words = A1599_FW_UPLOAD_WORDS;
	layout.memory = a1599_memory;
	a1599_fill_experimental_evidence(&layout);
	dev_info(dev, "DMA_ALLOCATED_EXPERIMENTAL mask=%#llx staging_dma=%pad staging_bytes=%zu root_dma=%pad root_bytes=%zu contexts=(%pad,%pad,%pad) leaves=(%pad,%pad,%pad,%pad)\n",
		 (unsigned long long)route.dma_addr_mask,
		 &a1599_memory->staging.dma, a1599_memory->staging.bytes,
		 &a1599_memory->root.dma, a1599_memory->root.bytes,
		 &a1599_memory->context_large.dma,
		 &a1599_memory->context_medium.dma,
		 &a1599_memory->context_small.dma,
		 &a1599_memory->leaf[0].dma, &a1599_memory->leaf[1].dma,
		 &a1599_memory->leaf[2].dma, &a1599_memory->leaf[3].dma);
	ret = a1599_fw_first_ready_experiment_validate(&layout);
	if (ret)
		goto fail_before_start;
	dev_info(dev, "PREFLIGHT_VALIDATED firmware_sha256=%s vxd_maps=(staging:%#x+%zx extra:%#x+%zx context_large:%#x+%zx context_medium:%#x+%zx context_small:%#x+%zx) dma_handles_are_separate=1 dma_mask_hypothesis=%#llx\n",
		 A1599_FW_SHA256,
		 a1599_memory->staging_vxd_iova, a1599_memory->staging.bytes,
		 a1599_memory->extra_vxd_iova, a1599_memory->extra.bytes,
		 a1599_memory->context_large_vxd_iova, a1599_memory->context_large.bytes,
		 a1599_memory->context_medium_vxd_iova, a1599_memory->context_medium.bytes,
		 a1599_memory->context_small_vxd_iova,
		 (size_t)A1599_FW_STATIC_CONTEXT_SMALL,
		 (unsigned long long)route.dma_addr_mask);
	ret = a1599_pm_attach_and_resume(dev);
	if (ret)
		goto fail_before_start;

	/* Backend bounds each poll; cumulative poll budget is about 5.74 seconds. */
	atomic_set(&a1599_state, A1599_HARNESS_STARTED);
	operation_start_ns = ktime_get_ns();
	ret = a1599_fw_first_ready_experiment_run(&window, &layout);
	elapsed_ms = div_u64(ktime_get_ns() - operation_start_ns, NSEC_PER_MSEC);
	release_firmware(fw);
	fw = NULL;
	if (ret) {
		dev_err(dev, "one-shot first-ready failed: ret=%d elapsed_ms=%llu logical_offset=%#x selector_cleanup_ret=%d selector_bus_unknown=%d hardware_started=%d; no retry and mappings stay pinned\n",
			ret, (unsigned long long)elapsed_ms,
			layout.failed_logical_offset,
			layout.selector_cleanup_errno,
			layout.selector_bus_state_unknown,
			a1599_memory->hardware_started);
		atomic_set(&a1599_state, A1599_HARNESS_FAILED);
		if (!a1599_memory->hardware_started) {
			ret = ret ?: -EIO;
			goto fail_before_start;
		}
		/* Preserve the bound device, MMIO mapping, and all DMA allocations. */
		return 0;
	}
	dev_info(dev, "FIRST_READY: signature=0xa5a5a5a5 at logical offset 0x2fe0 elapsed_ms=%llu; no INIT or decode message sent; DMA mappings remain pinned\n",
		 (unsigned long long)elapsed_ms);
	a1599_install_runtime_controls(dev, &window);
	if (snapshot_once && !init_once) {
		struct a1599_fw_ring_snapshot snapshot;
		int snapshot_ret;

		snapshot_ret = a1599_ring_snapshot(&window, &snapshot);
		a1599_log_ring_snapshot(dev, &window, &snapshot, snapshot_ret, "AFTER_READY");
	}
	if (init_once)
		init_once_store(dev, NULL, "1", 1);
	atomic_set(&a1599_state, A1599_HARNESS_DONE);
	return 0;

fail_before_start:
	if (fw)
		release_firmware(fw);
	if (a1599_genpd_attached || a1599_runtime_pm_enabled ||
	    a1599_pm_reference_held) {
		pm_ret = a1599_pm_release_before_start(dev);
		if (pm_ret) {
			dev_err(dev, "pre-MMIO PM cleanup uncertain: original_ret=%d pm_ret=%d; preserving bound device/domain/DMA state\n",
				ret, pm_ret);
			a1599_keep_instance = true;
			atomic_set(&a1599_state, A1599_HARNESS_FAILED);
			return 0;
		}
	}
	if (a1599_memory && a1599_memory->allocated &&
	    !a1599_memory->hardware_started)
		a1599_fw_static_boot_memory_release(a1599_memory);
	kfree(a1599_memory);
	a1599_memory = NULL;
		dev_err(dev, "one-shot preflight rejected before codec MMIO: ret=%d; safe rollback was attempted and this module instance will not retry\n", ret);
	atomic_set(&a1599_state, A1599_HARNESS_FAILED);
	return ret;
}

static const struct of_device_id a1599_first_ready_match[] = {
	{ .compatible = "apple,s5l8960x-vxd-first-ready-experiment" },
	{}
};
MODULE_DEVICE_TABLE(of, a1599_first_ready_match);

static struct platform_driver a1599_first_ready_driver = {
	.probe = a1599_first_ready_probe,
	.driver = {
		.name = "a1599-vxd-first-ready-experiment",
		.of_match_table = a1599_first_ready_match,
		.probe_type = PROBE_FORCE_SYNCHRONOUS,
	},
};

static int __init a1599_first_ready_harness_init(void)
{
	int ret, remove_ret;

	if (!run_once) {
		pr_err("a1599-first-ready: refused; load-time run_once=1 is required\n");
		return -EPERM;
	}
	ret = a1599_live_tree_preflight();
	if (ret) {
		pr_err("a1599-first-ready: LIVE_DT_PREFLIGHT failed ret=%d; no driver or overlay registered\n",
		       ret);
		return ret;
	}
	ret = platform_driver_register(&a1599_first_ready_driver);
	if (ret)
		return ret;
	ret = of_overlay_fdt_apply(a1599_first_ready_dtbo,
				   sizeof(a1599_first_ready_dtbo),
				   &a1599_overlay_id, NULL);
	if (ret) {
		platform_driver_unregister(&a1599_first_ready_driver);
		return ret;
	}
	pr_info("a1599-first-ready: symbol-independent overlay applied id=%d; genpd attach is deferred to preflight-complete probe\n",
		a1599_overlay_id);
	if (atomic_read(&a1599_state) == A1599_HARNESS_DONE ||
	    a1599_keep_instance ||
	    (atomic_read(&a1599_state) == A1599_HARNESS_FAILED &&
	     a1599_memory && a1599_memory->hardware_started))
		return 0;

	/* Pre-start failure: the probe detached PM before overlay rollback. */
	remove_ret = of_overlay_remove(&a1599_overlay_id);
	if (remove_ret) {
		pr_err("a1599-first-ready: pre-start failure=%d, overlay rollback=%d; module retained; no retry\n",
		       atomic_read(&a1599_state), remove_ret);
		return 0;
	}
	platform_driver_unregister(&a1599_first_ready_driver);
	return atomic_read(&a1599_state) == A1599_HARNESS_IDLE ? -ENODEV : -EIO;
}
module_init(a1599_first_ready_harness_init);

MODULE_DESCRIPTION("A1599 pre-start DMA and default-off ID81 one-shot decode experiment");
MODULE_LICENSE("GPL");
