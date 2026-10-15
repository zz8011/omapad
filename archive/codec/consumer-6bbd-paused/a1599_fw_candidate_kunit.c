// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/slab.h>

#include "a1599_fw_first_ready_candidate.h"
#include "a1599_fw_ring_capacity.h"

struct a1599_fw_test_context {
	struct a1599_fw_static_route route;
	struct a1599_fw_static_boot_memory memory;
	struct a1599_fw_fixed_mapping maps[A1599_FW_STATIC_TOTAL_REQUIRED_MAPS];
	struct device dev;
};

static int a1599_fw_test_init(struct kunit *test)
{
	struct a1599_fw_test_context *ctx;
	unsigned int i;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	test->priv = ctx;
	ctx->route.dma_route = A1599_FW_EVIDENCE_REVIEWED;
	ctx->route.pte_dma_domain = A1599_FW_EVIDENCE_REVIEWED;
	ctx->route.dma_addr_mask = GENMASK_ULL(39, 0);
	ctx->memory.allocated = true;
	ctx->memory.staging.alloc_cpu = kunit_kzalloc(test,
		A1599_FW_STATIC_STAGING_BYTES, GFP_KERNEL);
	ctx->memory.staging.cpu = ctx->memory.staging.alloc_cpu;
	ctx->memory.staging.dma = 0x00100000;
	ctx->memory.staging.bytes = A1599_FW_STATIC_STAGING_BYTES;
	ctx->memory.extra.cpu = kunit_kzalloc(test, A1599_FW_STATIC_EXTRA_BYTES, GFP_KERNEL);
	ctx->memory.extra.dma = 0x00110000;
	ctx->memory.extra.bytes = A1599_FW_STATIC_EXTRA_BYTES;
	ctx->memory.context_large.cpu = kunit_kzalloc(test, A1599_FW_STATIC_CONTEXT_LARGE, GFP_KERNEL);
	ctx->memory.context_large.dma = 0x00200000;
	ctx->memory.context_large.bytes = A1599_FW_STATIC_CONTEXT_LARGE;
	ctx->memory.context_medium.cpu = kunit_kzalloc(test, A1599_FW_STATIC_CONTEXT_MEDIUM, GFP_KERNEL);
	ctx->memory.context_medium.dma = 0x00300000;
	ctx->memory.context_medium.bytes = A1599_FW_STATIC_CONTEXT_MEDIUM;
	ctx->memory.context_small.cpu = kunit_kzalloc(test, A1599_FW_STATIC_CONTEXT_SMALL_BACKING, GFP_KERNEL);
	ctx->memory.context_small.dma = 0x00340000;
	ctx->memory.context_small.bytes = A1599_FW_STATIC_CONTEXT_SMALL_BACKING;
	ctx->memory.root.alloc_cpu = kunit_kzalloc(test,
		A1599_FW_STATIC_ROOT_BYTES, GFP_KERNEL);
	ctx->memory.root.cpu = ctx->memory.root.alloc_cpu;
	ctx->memory.root.dma = 0x00400000;
	ctx->memory.root.bytes = A1599_FW_STATIC_ROOT_BYTES;
	for (i = 0; i < A1599_FW_STATIC_LEAF_PAGES; i++) {
		ctx->memory.leaf[i].alloc_cpu = kunit_kzalloc(test,
			SZ_4K, GFP_KERNEL);
		ctx->memory.leaf[i].cpu = ctx->memory.leaf[i].alloc_cpu;
		ctx->memory.leaf[i].dma = 0x00500000 + i * SZ_4K;
		ctx->memory.leaf[i].bytes = SZ_4K;
	}
	KUNIT_ASSERT_NOT_NULL(test, ctx->memory.staging.cpu);
	KUNIT_ASSERT_NOT_NULL(test, ctx->memory.extra.cpu);
	KUNIT_ASSERT_NOT_NULL(test, ctx->memory.context_large.cpu);
	KUNIT_ASSERT_NOT_NULL(test, ctx->memory.context_medium.cpu);
	KUNIT_ASSERT_NOT_NULL(test, ctx->memory.context_small.cpu);
	KUNIT_ASSERT_NOT_NULL(test, ctx->memory.root.cpu);
	for (i = 0; i < A1599_FW_STATIC_LEAF_PAGES; i++)
		KUNIT_ASSERT_NOT_NULL(test, ctx->memory.leaf[i].cpu);
	ctx->maps[0] = (struct a1599_fw_fixed_mapping) {
		.vxd_iova = A1599_FW_STATIC_STAGING_IOVA,
		.system_dma_addr = ctx->memory.staging.dma,
		.bytes = ctx->memory.staging.bytes,
	};
	ctx->maps[1] = (struct a1599_fw_fixed_mapping) {
		.vxd_iova = A1599_FW_STATIC_EXTRA_IOVA,
		.system_dma_addr = ctx->memory.extra.dma,
		.bytes = ctx->memory.extra.bytes,
	};
	ctx->maps[2] = (struct a1599_fw_fixed_mapping) {
		.vxd_iova = A1599_FW_STATIC_CONTEXT_LARGE_IOVA,
		.system_dma_addr = ctx->memory.context_large.dma,
		.bytes = ctx->memory.context_large.bytes,
	};
	ctx->maps[3] = (struct a1599_fw_fixed_mapping) {
		.vxd_iova = A1599_FW_STATIC_CONTEXT_MEDIUM_IOVA,
		.system_dma_addr = ctx->memory.context_medium.dma,
		.bytes = ctx->memory.context_medium.bytes,
	};
	ctx->maps[4] = (struct a1599_fw_fixed_mapping) {
		.vxd_iova = A1599_FW_STATIC_CONTEXT_SMALL_IOVA,
		.system_dma_addr = ctx->memory.context_small.dma,
		.bytes = A1599_FW_STATIC_CONTEXT_SMALL_BACKING,
	};
	{
		static const u32 decode_sizes[A1599_DECODE_RESOURCE_COUNT] = {
			A1599_FW_DECODE_PRELOAD_BYTES, A1599_FW_DECODE_VLC_BYTES,
			A1599_FW_DECODE_BITSTREAM_BYTES,
			A1599_FW_DECODE_CURRENT_PICTURE_BYTES,
			A1599_FW_DECODE_INTRA_BYTES, A1599_FW_DECODE_OUTPUT_BYTES,
			A1599_FW_DECODE_COMMAND_BYTES,
		};
		struct a1599_decode_iova_map init_maps[A1599_DECODE_INIT_MAPS];
		dma_addr_t decode_dma = 0x00600000;
		unsigned int j;

		for (j = 0; j < A1599_DECODE_INIT_MAPS; j++) {
			init_maps[j].iova = ctx->maps[j].vxd_iova;
			init_maps[j].bytes = ctx->maps[j].bytes;
		}
		KUNIT_ASSERT_EQ(test, a1599_decode_assign_iovas(init_maps,
			ARRAY_SIZE(init_maps), decode_sizes,
			ARRAY_SIZE(decode_sizes), ctx->memory.decode_vxd_iova), 0);
		for (j = 0; j < A1599_DECODE_RESOURCE_COUNT; j++) {
			ctx->memory.decode[j].cpu = kunit_kzalloc(test,
				decode_sizes[j], GFP_KERNEL);
			ctx->memory.decode[j].dma = decode_dma;
			ctx->memory.decode[j].bytes = decode_sizes[j];
			ctx->memory.decode_lifetime.buffers[j] =
				(struct a1599_decode_dma_view) {
					.cpu = ctx->memory.decode[j].cpu,
					.dma = decode_dma,
					.bytes = decode_sizes[j],
				};
			ctx->memory.decode_lifetime.allocated_count++;
			KUNIT_ASSERT_NOT_NULL(test, ctx->memory.decode[j].cpu);
			ctx->maps[A1599_FW_STATIC_REQUIRED_MAPS + j] =
				(struct a1599_fw_fixed_mapping) {
					.vxd_iova = ctx->memory.decode_vxd_iova[j],
					.system_dma_addr = decode_dma,
					.bytes = decode_sizes[j],
				};
			decode_dma += decode_sizes[j];
		}
	}
	return 0;
}

static void a1599_fw_test_all_decode_maps_sealed(struct kunit *test)
{
	struct a1599_fw_test_context *ctx = test->priv;
	__le32 *leaf;
	unsigned int i;

	KUNIT_ASSERT_EQ(test, a1599_fw_first_ready_build_tables(&ctx->route,
		&ctx->memory, ctx->maps, ARRAY_SIZE(ctx->maps)), 0);
	KUNIT_EXPECT_EQ(test, a1599_fw_static_boot_memory_validate_tables(
		&ctx->memory), 0);
	for (i = 0; i < A1599_DECODE_RESOURCE_COUNT; i++) {
		KUNIT_EXPECT_EQ(test, ctx->memory.decode_vxd_iova[i],
			0x40012000U + i * SZ_4K +
			(i > A1599_DECODE_CURRENT_PICTURE ?
			 A1599_FW_DECODE_CURRENT_PICTURE_BYTES - SZ_4K : 0) +
			(i > A1599_DECODE_INTRA ?
			 A1599_FW_DECODE_INTRA_BYTES - SZ_4K : 0));
	}
	leaf = ctx->memory.leaf[0].cpu;
	WRITE_ONCE(leaf[(ctx->memory.decode_vxd_iova[A1599_DECODE_COMMAND] >> 12) & 0x3ff], 0);
	KUNIT_EXPECT_EQ(test, a1599_fw_static_boot_memory_validate_tables(
		&ctx->memory), -EUCLEAN);
}

static void a1599_fw_test_seal_and_corruption(struct kunit *test)
{
	struct a1599_fw_test_context *ctx = test->priv;
	__le32 *root;

	KUNIT_ASSERT_EQ(test, a1599_fw_first_ready_build_tables(&ctx->route,
		&ctx->memory, ctx->maps, ARRAY_SIZE(ctx->maps)), 0);
	KUNIT_EXPECT_EQ(test, ctx->memory.staging_vxd_iova,
		ctx->maps[0].vxd_iova);
	KUNIT_EXPECT_EQ(test, a1599_fw_static_boot_memory_validate_tables(
		&ctx->memory), 0);
	root = ctx->memory.root.cpu;
	WRITE_ONCE(root[ctx->maps[0].vxd_iova >> 22], 0);
	KUNIT_EXPECT_EQ(test, a1599_fw_static_boot_memory_validate_tables(
		&ctx->memory), -EUCLEAN);
	KUNIT_ASSERT_EQ(test, a1599_fw_first_ready_build_tables(&ctx->route,
		&ctx->memory, ctx->maps, ARRAY_SIZE(ctx->maps)), 0);
	ctx->memory.context_medium_vxd_iova += SZ_4K;
	KUNIT_EXPECT_EQ(test, a1599_fw_static_boot_memory_validate_tables(
		&ctx->memory), -EUCLEAN);
}

static void a1599_fw_test_fake_iova_dma_rejected(struct kunit *test)
{
	struct a1599_fw_test_context *ctx = test->priv;
	struct a1599_fw_fixed_mapping maps[A1599_FW_STATIC_TOTAL_REQUIRED_MAPS];

	memcpy(maps, ctx->maps, sizeof(maps));
	maps[0].system_dma_addr += SZ_4K;
	KUNIT_EXPECT_EQ(test, a1599_fw_first_ready_build_tables(&ctx->route,
		&ctx->memory, maps, ARRAY_SIZE(maps)), -EINVAL);
	KUNIT_EXPECT_FALSE(test, ctx->memory.tables_sealed);
	KUNIT_EXPECT_EQ(test, ctx->memory.staging_vxd_iova, 0U);
}

static void a1599_fw_test_failed_preflight_clears_tables(struct kunit *test)
{
	struct a1599_fw_test_context *ctx = test->priv;
	struct a1599_fw_fixed_mapping maps[A1599_FW_STATIC_TOTAL_REQUIRED_MAPS];
	unsigned int i;
	__le32 *root = ctx->memory.root.cpu;

	memcpy(maps, ctx->maps, sizeof(maps));
	maps[1].vxd_iova += SZ_4K;
	KUNIT_EXPECT_EQ(test, a1599_fw_first_ready_build_tables(&ctx->route,
		&ctx->memory, maps, ARRAY_SIZE(maps)), -EINVAL);
	KUNIT_EXPECT_FALSE(test, ctx->memory.tables_sealed);
	KUNIT_EXPECT_EQ(test, ctx->memory.staging_vxd_iova, 0U);
	for (i = 0; i < A1599_FW_STATIC_ROOT_ENTRIES; i++)
		KUNIT_EXPECT_EQ(test, le32_to_cpu(root[i]), 0U);
}

static void a1599_fw_test_started_memory_cannot_be_released(struct kunit *test)
{
	struct a1599_fw_test_context *ctx = test->priv;
	void *staging = ctx->memory.staging.cpu;

	KUNIT_ASSERT_EQ(test, a1599_fw_first_ready_build_tables(&ctx->route,
		&ctx->memory, ctx->maps, ARRAY_SIZE(ctx->maps)), 0);
	KUNIT_ASSERT_EQ(test, a1599_fw_static_boot_memory_mark_started(
		&ctx->memory), 0);
	KUNIT_EXPECT_EQ(test, a1599_fw_static_boot_memory_release(
		&ctx->memory), -EBUSY);
	KUNIT_EXPECT_TRUE(test, ctx->memory.allocated);
	KUNIT_EXPECT_PTR_EQ(test, ctx->memory.staging.cpu, staging);
}

static void a1599_fw_test_mask_restore_failure_poison(struct kunit *test)
{
	struct a1599_fw_test_context *ctx = test->priv;

	KUNIT_EXPECT_EQ(test, a1599_fw_static_boot_memory_restore_status(
		&ctx->memory, -EINVAL, 0), -EINVAL);
	KUNIT_EXPECT_TRUE(test, ctx->memory.backend_unavailable);
	ctx->memory.backend_unavailable = false;
	KUNIT_EXPECT_EQ(test, a1599_fw_static_boot_memory_restore_status(
		&ctx->memory, 0, -EIO), -EIO);
	KUNIT_EXPECT_TRUE(test, ctx->memory.backend_unavailable);
	KUNIT_EXPECT_EQ(test, a1599_fw_static_boot_memory_alloc(&ctx->dev,
		&ctx->route, &ctx->memory), -EIO);
}

static void a1599_fw_test_empty_ring_capacity_boundaries(struct kunit *test)
{
	/* Five slots cannot hold five words with the one-empty-slot rule. */
	KUNIT_EXPECT_FALSE(test,
		a1599_empty_ring_has_contiguous_space(5, 0, 0, 5));
	/* Six slots have exactly five usable words when empty at index zero. */
	KUNIT_EXPECT_TRUE(test,
		a1599_empty_ring_has_contiguous_space(6, 0, 0, 5));
	/* Empty at index one still has a five-word contiguous tail. */
	KUNIT_EXPECT_TRUE(test,
		a1599_empty_ring_has_contiguous_space(6, 1, 1, 5));
	/* A short tail must fail; this path never wraps or pads. */
	KUNIT_EXPECT_FALSE(test,
		a1599_empty_ring_has_contiguous_space(6, 2, 2, 5));
	/* Non-empty or malformed indices are not eligible for this path. */
	KUNIT_EXPECT_FALSE(test,
		a1599_empty_ring_has_contiguous_space(6, 0, 1, 5));
	KUNIT_EXPECT_FALSE(test,
		a1599_empty_ring_has_contiguous_space(6, 6, 6, 5));
}

static void a1599_fw_test_fw_abi_ring_experiment_gate(struct kunit *test)
{
	KUNIT_EXPECT_FALSE(test,
		a1599_fw_abi_ring_snapshot_matches(false,
			0x60cc01cc, 0x60000033, 0, 0, 0, 0,
			0x80cc, 0x87fc, 0x8000, 0x80cc, 0x100000));
	KUNIT_EXPECT_TRUE(test,
		a1599_fw_abi_ring_snapshot_matches(true,
			0x60cc01cc, 0x60000033, 0, 0, 0, 0,
			0x80cc, 0x87fc, 0x8000, 0x80cc, 0x100000));
	KUNIT_EXPECT_FALSE(test,
		a1599_fw_abi_ring_snapshot_matches(true,
			0x60cc01cd, 0x60000033, 0, 0, 0, 0,
			0x80cc, 0x87fc, 0x8000, 0x80cc, 0x100000));
	KUNIT_EXPECT_FALSE(test,
		a1599_fw_abi_ring_snapshot_matches(true,
			0x60cc01cc, 0x60000033, 0, 1, 0, 0,
			0x80cc, 0x87fc, 0x8000, 0x80cc, 0x100000));
	KUNIT_EXPECT_FALSE(test,
		a1599_fw_abi_ring_snapshot_matches(true,
			0x60cc01cc, 0x60000033, 0, 0, 0, 0,
			0x80cc, 0x87fc, 0x8000, 0x80cc, 0x8000));
}

static struct kunit_case a1599_fw_candidate_cases[] = {
	KUNIT_CASE(a1599_fw_test_seal_and_corruption),
	KUNIT_CASE(a1599_fw_test_all_decode_maps_sealed),
	KUNIT_CASE(a1599_fw_test_fake_iova_dma_rejected),
	KUNIT_CASE(a1599_fw_test_failed_preflight_clears_tables),
	KUNIT_CASE(a1599_fw_test_started_memory_cannot_be_released),
	KUNIT_CASE(a1599_fw_test_mask_restore_failure_poison),
	KUNIT_CASE(a1599_fw_test_empty_ring_capacity_boundaries),
	KUNIT_CASE(a1599_fw_test_fw_abi_ring_experiment_gate),
	{}
};

static struct kunit_suite a1599_fw_candidate_suite = {
	.name = "a1599-fw-candidate-init-ready",
	.init = a1599_fw_test_init,
	.test_cases = a1599_fw_candidate_cases,
};
kunit_test_suite(a1599_fw_candidate_suite);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("A1599 firmware-ABI ring INIT KUnit regressions");
