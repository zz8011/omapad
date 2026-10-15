// SPDX-License-Identifier: GPL-2.0-only
/* Parameterized fixed VXD page-table construction; never accesses hardware. */
#include <linux/crc32.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/overflow.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "a1599_fw_first_ready_candidate.h"

#define A1599_PTE_ADDRESS_MASK GENMASK_ULL(39, 12)
#define A1599_PTE_ENTRIES 1024U
#define A1599_PTE_LEAF_BYTES SZ_4K
#define A1599_PTE_ROOT_ALIGN SZ_64K
#define A1599_TABLE_WORDS \
	(A1599_FW_STATIC_ROOT_ENTRIES * (1 + A1599_FW_STATIC_LEAF_PAGES))

static int a1599_validate_map_page(const struct a1599_fw_static_route *route,
				   u64 dma, bool allow_experimental)
{
	u64 last;

	if (!route ||
	    (route->dma_route != A1599_FW_EVIDENCE_REVIEWED &&
	     !(allow_experimental && route->dma_route == A1599_FW_EVIDENCE_EXPERIMENTAL_ASSUMPTION)) ||
	    (route->pte_dma_domain != A1599_FW_EVIDENCE_REVIEWED &&
	     !(allow_experimental && route->pte_dma_domain == A1599_FW_EVIDENCE_EXPERIMENTAL_ASSUMPTION)) ||
	    !route->dma_addr_mask)
		return -EOPNOTSUPP;
	if (!IS_ALIGNED(dma, SZ_4K) ||
	    check_add_overflow(dma, (u64)SZ_4K - 1, &last) ||
	    dma > route->dma_addr_mask || last > route->dma_addr_mask ||
	    last > (A1599_PTE_ADDRESS_MASK | (SZ_4K - 1)))
		return -ERANGE;
	return 0;
}

static u32 a1599_encode_dma_page(dma_addr_t dma)
{
	return (u32)(((u64)dma >> 8) & 0xfffffff0U) | 1U;
}

static u32 a1599_tables_crc(const struct a1599_fw_static_boot_memory *m)
{
	u32 crc = ~0U;
	unsigned int i;

	crc = crc32_le(crc, m->root.cpu, A1599_FW_STATIC_ROOT_BYTES);
	for (i = 0; i < A1599_FW_STATIC_LEAF_PAGES; i++)
		crc = crc32_le(crc, m->leaf[i].cpu, A1599_PTE_LEAF_BYTES);
	crc = crc32_le(crc, (const u8 *)&m->staging_vxd_iova,
		       sizeof(m->staging_vxd_iova));
	crc = crc32_le(crc, (const u8 *)&m->extra_vxd_iova,
		       sizeof(m->extra_vxd_iova));
	crc = crc32_le(crc, (const u8 *)&m->context_large_vxd_iova,
		       sizeof(m->context_large_vxd_iova));
	crc = crc32_le(crc, (const u8 *)&m->context_medium_vxd_iova,
		       sizeof(m->context_medium_vxd_iova));
	crc = crc32_le(crc, (const u8 *)&m->context_small_vxd_iova,
		       sizeof(m->context_small_vxd_iova));
	crc = crc32_le(crc, (const u8 *)m->decode_vxd_iova,
		       sizeof(m->decode_vxd_iova));
	crc = crc32_le(crc, (const u8 *)m->leaf_root_index,
		       sizeof(m->leaf_root_index));
	return crc ^ ~0U;
}

static void a1599_invalidate_tables(struct a1599_fw_static_boot_memory *m)
{
	unsigned int i;

	if (!m)
		return;
	m->tables_sealed = false;
	m->staging_vxd_iova = 0;
	m->extra_vxd_iova = 0;
	m->context_large_vxd_iova = 0;
	m->context_medium_vxd_iova = 0;
	m->context_small_vxd_iova = 0;
	m->tables_crc32 = 0;
	for (i = 0; i < A1599_FW_STATIC_LEAF_PAGES; i++) {
		m->leaf_root_index[i] = U16_MAX;
		if (m->leaf[i].cpu && m->leaf[i].bytes >= A1599_PTE_LEAF_BYTES)
			memset(m->leaf[i].cpu, 0, A1599_PTE_LEAF_BYTES);
	}
	if (m->root.cpu && m->root.bytes >= A1599_FW_STATIC_ROOT_BYTES)
		memset(m->root.cpu, 0, A1599_FW_STATIC_ROOT_BYTES);
	dma_wmb();
}

static int a1599_validate_maps(const struct a1599_fw_static_route *route,
			       struct a1599_fw_static_boot_memory *m,
			       const struct a1599_fw_fixed_mapping *maps,
			       unsigned int map_count,
			       u16 root_for_leaf[A1599_FW_STATIC_LEAF_PAGES],
			       unsigned int *leaf_used,
			       u32 *staging_iova,
			       bool allow_experimental)
{
	unsigned int map_idx;
	bool found_staging = false;

	*leaf_used = 0;
	*staging_iova = 0;
	for (map_idx = 0; map_idx < A1599_FW_STATIC_LEAF_PAGES; map_idx++)
		root_for_leaf[map_idx] = U16_MAX;

	for (map_idx = 0; map_idx < map_count; map_idx++) {
		const struct a1599_fw_fixed_mapping *map = &maps[map_idx];
		u64 virtual_end, dma_end, dma_limit, staging_limit;
		size_t offset;
		unsigned int prior;

		if (!map->bytes || !IS_ALIGNED(map->vxd_iova, SZ_4K) ||
		    !IS_ALIGNED((u64)map->system_dma_addr, SZ_4K) ||
		    !IS_ALIGNED(map->bytes, SZ_4K) ||
		    check_add_overflow((u64)map->vxd_iova,
				       (u64)map->bytes - 1, &virtual_end) ||
		    virtual_end > U32_MAX ||
		    check_add_overflow((u64)map->system_dma_addr,
				       (u64)map->bytes - 1, &dma_end))
			return -ERANGE;
		if (check_add_overflow((u64)map->system_dma_addr,
				       (u64)map->bytes, &dma_limit) ||
		    check_add_overflow((u64)m->staging.dma,
				       (u64)m->staging.bytes, &staging_limit))
			return -ERANGE;

		for (prior = 0; prior < map_idx; prior++) {
			u64 prior_end, prior_dma_end;

			if (check_add_overflow((u64)maps[prior].vxd_iova,
					       (u64)maps[prior].bytes,
					       &prior_end) ||
			    check_add_overflow((u64)maps[prior].system_dma_addr,
					       (u64)maps[prior].bytes,
					       &prior_dma_end))
				return -ERANGE;
			if ((u64)map->vxd_iova < prior_end &&
			    (u64)maps[prior].vxd_iova < (u64)map->vxd_iova + map->bytes)
				return -EADDRINUSE;
			/* One physical DMA interval must not gain multiple VXD aliases. */
			if ((u64)map->system_dma_addr < prior_dma_end &&
			    (u64)maps[prior].system_dma_addr < dma_limit)
				return -EADDRINUSE;
		}

		if ((u64)map->system_dma_addr < staging_limit &&
		    (u64)m->staging.dma < dma_limit) {
			if (found_staging || map->system_dma_addr != m->staging.dma ||
			    map->bytes != A1599_FW_STATIC_STAGING_BYTES)
				return -EINVAL;
			found_staging = true;
			*staging_iova = map->vxd_iova;
		}

		for (offset = 0; offset < map->bytes; offset += SZ_4K) {
			u64 va = (u64)map->vxd_iova + offset;
			u64 dma = (u64)map->system_dma_addr + offset;
			unsigned int root_index = (unsigned int)(va >> 22);
			unsigned int i;
			bool known_root = false;
			int ret;

			if (root_index >= A1599_FW_STATIC_ROOT_ENTRIES)
				return -ERANGE;
			ret = a1599_validate_map_page(route, dma, allow_experimental);
			if (ret)
				return ret;
			for (i = 0; i < *leaf_used; i++) {
				if (root_for_leaf[i] == root_index) {
					known_root = true;
					break;
				}
			}
			if (!known_root) {
				if (*leaf_used >= A1599_FW_STATIC_LEAF_PAGES)
					return -ENOSPC;
				root_for_leaf[(*leaf_used)++] = root_index;
			}
		}
	}

	if (!found_staging || !IS_ALIGNED(*staging_iova, SZ_4K))
		return -EINVAL;
	for (map_idx = 0; map_idx < *leaf_used; map_idx++) {
		int ret = a1599_validate_map_page(route, m->leaf[map_idx].dma,
						  allow_experimental);

		if (ret)
			return ret;
	}
	return a1599_validate_map_page(route, m->root.dma, allow_experimental);
}

static int a1599_validate_required_maps(
		const struct a1599_fw_static_boot_memory *m,
		const struct a1599_fw_fixed_mapping *maps, unsigned int map_count)
{
	const struct a1599_fw_static_allocation *allocations[] = {
		&m->staging, &m->extra, &m->context_large,
		&m->context_medium, &m->context_small,
	};
	const u32 iovas[] = {
		A1599_FW_STATIC_STAGING_IOVA, A1599_FW_STATIC_EXTRA_IOVA,
		A1599_FW_STATIC_CONTEXT_LARGE_IOVA,
		A1599_FW_STATIC_CONTEXT_MEDIUM_IOVA,
		A1599_FW_STATIC_CONTEXT_SMALL_IOVA,
	};
	unsigned int i;

	static const size_t decode_sizes[A1599_DECODE_RESOURCE_COUNT] = {
		[A1599_DECODE_PRELOAD] = A1599_FW_DECODE_PRELOAD_BYTES,
		[A1599_DECODE_VLC] = A1599_FW_DECODE_VLC_BYTES,
		[A1599_DECODE_BITSTREAM] = A1599_FW_DECODE_BITSTREAM_BYTES,
		[A1599_DECODE_CURRENT_PICTURE] = A1599_FW_DECODE_CURRENT_PICTURE_BYTES,
		[A1599_DECODE_INTRA] = A1599_FW_DECODE_INTRA_BYTES,
		[A1599_DECODE_OUTPUT] = A1599_FW_DECODE_OUTPUT_BYTES,
		[A1599_DECODE_COMMAND] = A1599_FW_DECODE_COMMAND_BYTES,
	};
	if (map_count != A1599_FW_STATIC_TOTAL_REQUIRED_MAPS)
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(allocations); i++) {
		size_t bytes = i == 4 ? A1599_FW_STATIC_CONTEXT_SMALL_BACKING :
			allocations[i]->bytes;

		if (!allocations[i]->cpu || !allocations[i]->bytes ||
		    maps[i].vxd_iova != iovas[i] ||
		    maps[i].system_dma_addr != allocations[i]->dma ||
		    maps[i].bytes != bytes)
			return -EINVAL;
	}
	for (i = 0; i < A1599_DECODE_RESOURCE_COUNT; i++) {
		const struct a1599_fw_static_allocation *allocation = &m->decode[i];
		const struct a1599_fw_fixed_mapping *map =
			&maps[A1599_FW_STATIC_REQUIRED_MAPS + i];

		if (!allocation->cpu || allocation->bytes != decode_sizes[i] ||
		    !m->decode_vxd_iova[i] ||
		    !IS_ALIGNED(m->decode_vxd_iova[i], SZ_4K) ||
		    map->vxd_iova != m->decode_vxd_iova[i] ||
		    map->system_dma_addr != allocation->dma ||
		    map->bytes != allocation->bytes)
			return -EINVAL;
	}
	return 0;
}

static int a1599_validate_one_mapping_pages(
		const struct a1599_fw_static_boot_memory *m, u32 iova,
		dma_addr_t dma, size_t bytes)
{
	const __le32 *root = m->root.cpu;
	size_t offset;

	if (!bytes || !IS_ALIGNED(iova, SZ_4K) ||
	    !IS_ALIGNED((u64)dma, SZ_4K) || !IS_ALIGNED(bytes, SZ_4K))
		return -EKEYREJECTED;
	for (offset = 0; offset < bytes; offset += SZ_4K) {
		u64 va = (u64)iova + offset;
		unsigned int root_index = (unsigned int)(va >> 22);
		unsigned int leaf_index = (unsigned int)((va >> 12) & 0x3ff);
		unsigned int i;
		bool found = false;

		if (root_index >= A1599_FW_STATIC_ROOT_ENTRIES)
			return -ERANGE;
		for (i = 0; i < A1599_FW_STATIC_LEAF_PAGES; i++) {
			const __le32 *leaf;

			if (m->leaf_root_index[i] != root_index)
				continue;
			if (le32_to_cpu(READ_ONCE(root[root_index])) !=
			    a1599_encode_dma_page(m->leaf[i].dma))
				return -EUCLEAN;
			leaf = m->leaf[i].cpu;
			if (le32_to_cpu(READ_ONCE(leaf[leaf_index])) !=
			    a1599_encode_dma_page(dma + offset))
				return -EUCLEAN;
			found = true;
			break;
		}
		if (!found)
			return -EUCLEAN;
	}
	return 0;
}

static int a1599_validate_staging_pages(
		const struct a1599_fw_static_boot_memory *m)
{
	const __le32 *root = m->root.cpu;
	unsigned int page;

	for (page = 0; page < A1599_FW_STATIC_STAGING_PAGES; page++) {
		u64 va = (u64)m->staging_vxd_iova + (u64)page * SZ_4K;
		unsigned int root_index = (unsigned int)(va >> 22);
		unsigned int leaf_index = (unsigned int)((va >> 12) & 0x3ff);
		unsigned int i;
		bool found = false;

		for (i = 0; i < A1599_FW_STATIC_LEAF_PAGES; i++) {
			const __le32 *leaf;

			if (m->leaf_root_index[i] != root_index)
				continue;
			if (le32_to_cpu(READ_ONCE(root[root_index])) !=
			    a1599_encode_dma_page(m->leaf[i].dma))
				return -EUCLEAN;
			leaf = m->leaf[i].cpu;
			if (le32_to_cpu(READ_ONCE(leaf[leaf_index])) !=
			    a1599_encode_dma_page(m->staging.dma + (dma_addr_t)page * SZ_4K))
				return -EUCLEAN;
			found = true;
			break;
		}
		if (!found)
			return -EUCLEAN;
	}
	return 0;
}

int a1599_fw_static_boot_memory_validate_tables(
		const struct a1599_fw_static_boot_memory *m)
{
	unsigned int i;
	int ret;

	if (!m || !m->allocated || !m->tables_sealed || m->backend_unavailable ||
	    m->decode_lifetime.allocated_count != A1599_DECODE_RESOURCE_COUNT ||
	    !m->staging.cpu || m->staging.bytes != A1599_FW_STATIC_STAGING_BYTES ||
	    !m->extra.cpu || m->extra.bytes != A1599_FW_STATIC_EXTRA_BYTES ||
	    !m->context_large.cpu ||
	    m->context_large.bytes != A1599_FW_STATIC_CONTEXT_LARGE ||
	    !m->context_medium.cpu ||
	    m->context_medium.bytes != A1599_FW_STATIC_CONTEXT_MEDIUM ||
	    !m->context_small.cpu ||
	    m->context_small.bytes != A1599_FW_STATIC_CONTEXT_SMALL_BACKING ||
	    !IS_ALIGNED(m->staging_vxd_iova, SZ_4K) ||
	    !m->root.cpu || m->root.bytes != A1599_FW_STATIC_ROOT_BYTES ||
	    !IS_ALIGNED((u64)m->root.dma, A1599_PTE_ROOT_ALIGN))
		return -EKEYREJECTED;
	for (i = 0; i < A1599_DECODE_RESOURCE_COUNT; i++) {
		if (!m->decode[i].cpu || !m->decode[i].bytes ||
		    !IS_ALIGNED(m->decode_vxd_iova[i], SZ_4K))
			return -EKEYREJECTED;
	}
	for (i = 0; i < A1599_FW_STATIC_LEAF_PAGES; i++)
		if (!m->leaf[i].cpu || m->leaf[i].bytes != A1599_PTE_LEAF_BYTES)
			return -EKEYREJECTED;
	if (a1599_tables_crc(m) != m->tables_crc32)
		return -EUCLEAN;
	if (m->extra_vxd_iova != A1599_FW_STATIC_EXTRA_IOVA ||
	    m->context_large_vxd_iova != A1599_FW_STATIC_CONTEXT_LARGE_IOVA ||
	    m->context_medium_vxd_iova != A1599_FW_STATIC_CONTEXT_MEDIUM_IOVA ||
	    m->context_small_vxd_iova != A1599_FW_STATIC_CONTEXT_SMALL_IOVA)
		return -EKEYREJECTED;
	ret = a1599_validate_staging_pages(m);
	if (ret)
		return ret;
	ret = a1599_validate_one_mapping_pages(m, m->extra_vxd_iova,
		m->extra.dma, m->extra.bytes);
	if (ret)
		return ret;
	ret = a1599_validate_one_mapping_pages(m, m->context_large_vxd_iova,
		m->context_large.dma, m->context_large.bytes);
	if (ret)
		return ret;
	ret = a1599_validate_one_mapping_pages(m, m->context_medium_vxd_iova,
		m->context_medium.dma, m->context_medium.bytes);
	if (ret)
		return ret;
	ret = a1599_validate_one_mapping_pages(m, m->context_small_vxd_iova,
		m->context_small.dma, m->context_small.bytes);
	if (ret)
		return ret;
	for (i = 0; i < A1599_DECODE_RESOURCE_COUNT; i++) {
		ret = a1599_validate_one_mapping_pages(m, m->decode_vxd_iova[i],
			m->decode[i].dma, m->decode[i].bytes);
		if (ret)
			return ret;
	}
	return 0;
}
EXPORT_SYMBOL_GPL(a1599_fw_static_boot_memory_validate_tables);

/*
 * Preflight every mapping, then build complete temporary tables, then publish
 * them atomically to the retained coherent allocations and seal their bytes.
 */
static int build_tables_common(
		const struct a1599_fw_static_route *route,
		struct a1599_fw_static_boot_memory *memory,
		const struct a1599_fw_fixed_mapping *maps, unsigned int map_count,
		bool allow_experimental)
{
	u16 root_for_leaf[A1599_FW_STATIC_LEAF_PAGES];
	unsigned int leaf_used = 0, map_idx;
	u32 staging_iova = 0;
	__le32 *scratch, *root;
	int ret;

	if (!memory)
		return -EINVAL;
	if (memory->hardware_started || memory->backend_unavailable)
		return -EBUSY;
	a1599_invalidate_tables(memory);
	if (!memory->allocated || !maps || !map_count || !memory->root.cpu ||
	    memory->root.bytes != A1599_FW_STATIC_ROOT_BYTES ||
	    !IS_ALIGNED((u64)memory->root.dma, A1599_PTE_ROOT_ALIGN) ||
	    !memory->staging.cpu || memory->staging.bytes != A1599_FW_STATIC_STAGING_BYTES)
		return -EINVAL;
	if (!route ||
	    (route->dma_route != A1599_FW_EVIDENCE_REVIEWED &&
	     !(allow_experimental && route->dma_route == A1599_FW_EVIDENCE_EXPERIMENTAL_ASSUMPTION)) ||
	    (route->pte_dma_domain != A1599_FW_EVIDENCE_REVIEWED &&
	     !(allow_experimental && route->pte_dma_domain == A1599_FW_EVIDENCE_EXPERIMENTAL_ASSUMPTION)))
		return -EOPNOTSUPP;
	ret = a1599_validate_required_maps(memory, maps, map_count);
	if (ret)
		return ret;
	for (map_idx = 0; map_idx < A1599_FW_STATIC_LEAF_PAGES; map_idx++)
		if (!memory->leaf[map_idx].cpu ||
		    memory->leaf[map_idx].bytes != A1599_PTE_LEAF_BYTES)
			return -EINVAL;

	ret = a1599_validate_maps(route, memory, maps, map_count, root_for_leaf,
				  &leaf_used, &staging_iova, allow_experimental);
	if (ret)
		return ret;
	scratch = kcalloc(A1599_TABLE_WORDS, sizeof(*scratch), GFP_KERNEL);
	if (!scratch)
		return -ENOMEM;
	root = scratch;

	for (map_idx = 0; map_idx < map_count; map_idx++) {
		const struct a1599_fw_fixed_mapping *map = &maps[map_idx];
		size_t offset;

		for (offset = 0; offset < map->bytes; offset += SZ_4K) {
			u64 va = (u64)map->vxd_iova + offset;
			unsigned int root_index = (unsigned int)(va >> 22);
			unsigned int pte_index = (unsigned int)((va >> 12) & 0x3ff);
			unsigned int leaf_slot;

			for (leaf_slot = 0; leaf_slot < leaf_used; leaf_slot++)
				if (root_for_leaf[leaf_slot] == root_index)
					break;
			if (leaf_slot == leaf_used) {
				ret = -EUCLEAN;
				goto out_fail;
			}
			{
				u32 pde = a1599_encode_dma_page(
					memory->leaf[leaf_slot].dma);
				u32 old_pde = le32_to_cpu(root[root_index]);

				if (old_pde && old_pde != pde) {
					ret = -EADDRINUSE;
					goto out_fail;
				}
				root[root_index] = cpu_to_le32(pde);
			}
			{
				__le32 *leaf = scratch + A1599_FW_STATIC_ROOT_ENTRIES +
					leaf_slot * A1599_FW_STATIC_LEAF_ENTRIES;
				if (le32_to_cpu(leaf[pte_index])) {
					ret = -EADDRINUSE;
					goto out_fail;
				}
				leaf[pte_index] = cpu_to_le32(a1599_encode_dma_page(
					(dma_addr_t)((u64)map->system_dma_addr + offset)));
			}
		}
	}

	memcpy(memory->root.cpu, root, A1599_FW_STATIC_ROOT_BYTES);
	for (map_idx = 0; map_idx < A1599_FW_STATIC_LEAF_PAGES; map_idx++) {
		const __le32 *src = scratch + A1599_FW_STATIC_ROOT_ENTRIES +
			map_idx * A1599_FW_STATIC_LEAF_ENTRIES;
		memcpy(memory->leaf[map_idx].cpu, src, A1599_PTE_LEAF_BYTES);
		memory->leaf_root_index[map_idx] = map_idx < leaf_used ?
			root_for_leaf[map_idx] : U16_MAX;
	}
	memory->staging_vxd_iova = staging_iova;
	memory->extra_vxd_iova = maps[1].vxd_iova;
	memory->context_large_vxd_iova = maps[2].vxd_iova;
	memory->context_medium_vxd_iova = maps[3].vxd_iova;
	memory->context_small_vxd_iova = maps[4].vxd_iova;
	dma_wmb();
	memory->tables_crc32 = a1599_tables_crc(memory);
	memory->tables_sealed = true;
	ret = a1599_fw_static_boot_memory_validate_tables(memory);
	if (ret)
		a1599_invalidate_tables(memory);
	kfree(scratch);
	return ret;

out_fail:
	kfree(scratch);
	a1599_invalidate_tables(memory);
	return ret;
}

int a1599_fw_first_ready_build_tables(
		const struct a1599_fw_static_route *route,
		struct a1599_fw_static_boot_memory *memory,
		const struct a1599_fw_fixed_mapping *maps, unsigned int map_count)
{
	return build_tables_common(route, memory, maps, map_count, false);
}

int a1599_fw_first_ready_build_tables_experimental(
		const struct a1599_fw_static_route *route,
		struct a1599_fw_static_boot_memory *memory,
		const struct a1599_fw_fixed_mapping *maps, unsigned int map_count)
{
	return build_tables_common(route, memory, maps, map_count, true);
}
EXPORT_SYMBOL_GPL(a1599_fw_first_ready_build_tables);
