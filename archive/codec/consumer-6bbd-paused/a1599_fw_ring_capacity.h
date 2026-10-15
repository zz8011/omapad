/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef A1599_FW_RING_CAPACITY_H
#define A1599_FW_RING_CAPACITY_H

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>
typedef u32 a1599_ring_u32;
typedef u64 a1599_ring_u64;
typedef u8 a1599_ring_u8;
#else
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
typedef uint32_t a1599_ring_u32;
typedef uint64_t a1599_ring_u64;
typedef uint8_t a1599_ring_u8;
#endif

struct a1599_init_mmio_ops {
	int (*read32)(void *context, a1599_ring_u32 offset,
		      a1599_ring_u32 *value);
	void (*write32)(void *context, a1599_ring_u32 offset,
			a1599_ring_u32 value);
	void (*publish_barrier)(void *context);
	void (*read_barrier)(void *context);
	a1599_ring_u64 (*time_ns)(void *context);

	void (*sleep_1_1_5_ms)(void *context);
};

struct a1599_ring_message {
	a1599_ring_u8 bytes[127];
	a1599_ring_u32 length;
	a1599_ring_u32 read_index;
	a1599_ring_u32 write_index;
};

struct a1599_init_mmio_result {
	a1599_ring_u32 producer_readback;
	a1599_ring_u32 consumer_readback;
	a1599_ring_u32 m2h_readback;
	a1599_ring_u32 m2h_writeback;
	bool kicked;
};

static inline bool a1599_empty_ring_has_contiguous_space(
	a1599_ring_u32 capacity, a1599_ring_u32 read_index,
	a1599_ring_u32 write_index, a1599_ring_u32 words);

/* This is the exact MMIO transaction used by the production candidate and
 * exercised with a host fake-MMIO backend. It performs no retry or reset.
 */
static inline int a1599_init_mmio_send_once(
	const struct a1599_init_mmio_ops *ops, void *context,
	a1599_ring_u32 ring_start, a1599_ring_u32 capacity,
	a1599_ring_u32 read_index, a1599_ring_u32 write_index,
	const a1599_ring_u32 expected_regs[6],
	const a1599_ring_u32 words[5],
	struct a1599_init_mmio_result *result)
{
	static const a1599_ring_u32 offsets[6] = {
		0x2ff0, 0x2fe4, 0x2ff4, 0x2ffc, 0x2fe8, 0x2fec,
	};
	a1599_ring_u32 observed[6], next;
	unsigned int i;
	int ret;

	if (!ops || !ops->read32 || !ops->write32 ||
	    !ops->publish_barrier || !expected_regs || !words || !result)
		return -EINVAL;
	result->kicked = false;
	for (i = 0; i < 6; i++) {
		ret = ops->read32(context, offsets[i], &observed[i]);
		if (ret)
			return ret;
		if (observed[i] != expected_regs[i])
			return -ESTALE;
	}
	if (read_index != write_index || write_index >= capacity ||
	    !a1599_empty_ring_has_contiguous_space(capacity, read_index,
						    write_index, 5))
		return -ENOSPC;
	for (i = 0; i < 5; i++)
		ops->write32(context, ring_start +
			     (write_index + i) * sizeof(a1599_ring_u32), words[i]);
	ops->publish_barrier(context);
	next = write_index + 5;
	if (next == capacity)
		next = 0;
	ops->write32(context, 0x2ffc, next);
	ret = ops->read32(context, 0x2ffc, &result->producer_readback);
	if (ret)
		return ret;
	if (result->producer_readback != next)
		return -EIO;
	ret = ops->read32(context, 0x2ff4, &result->consumer_readback);
	if (ret)
		return ret;
	ret = ops->read32(context, 0x2fe8, &result->m2h_readback);
	if (ret)
		return ret;
	ret = ops->read32(context, 0x2fec, &result->m2h_writeback);
	if (ret)
		return ret;
	ops->write32(context, 0x80, 1);
	result->kicked = true;
	return 0;
}

static inline int a1599_init_mmio_wait_consumed(
	const struct a1599_init_mmio_ops *ops, void *context,
	a1599_ring_u32 capacity, a1599_ring_u32 expected_read,
	a1599_ring_u32 *observed_read)
{
	a1599_ring_u64 start;
	int ret;

	if (!ops || !ops->read32 || !ops->time_ns ||
	    !ops->sleep_1_1_5_ms || !observed_read)
		return -EINVAL;
	start = ops->time_ns(context);
	for (;;) {
		ret = ops->read32(context, 0x2ff4, observed_read);
		if (ret)
			return ret;
		if (*observed_read >= capacity)
			return -ERANGE;
		if (*observed_read == expected_read)
			return 0;
		if (ops->time_ns(context) - start >= 2000000000ULL)
			return -ETIMEDOUT;
		ops->sleep_1_1_5_ms(context);
	}
}

/* Poll one already-published M2H wire message without changing its consumer
 * index. This is used for a single C0 observation; timeout/error preserves the
 * response ring and every DMA allocation for evidence collection.
 */
static inline int a1599_m2h_mmio_wait_message(
		const struct a1599_init_mmio_ops *ops, void *context,
		a1599_ring_u32 ring_start, a1599_ring_u32 capacity,
		a1599_ring_u32 read_index, a1599_ring_u32 timeout_ms,
		struct a1599_ring_message *message)
{
	a1599_ring_u64 start, budget_ns;
	a1599_ring_u32 write_index, available, first_word, declared, words;
	a1599_ring_u32 i;
	int ret;

	if (!ops || !ops->read32 || !ops->read_barrier || !ops->time_ns ||
	    !ops->sleep_1_1_5_ms || !message || !capacity ||
	    read_index >= capacity || !timeout_ms)
		return -EINVAL;
	budget_ns = (a1599_ring_u64)timeout_ms * 1000000ULL;
	start = ops->time_ns(context);
	for (;;) {
		ret = ops->read32(context, 0x2fec, &write_index);
		if (ret)
			return ret;
		if (write_index >= capacity)
			return -ERANGE;
		available = write_index >= read_index ? write_index - read_index :
			capacity - read_index + write_index;
		if (available) {
			ops->read_barrier(context);
			ret = ops->read32(context, ring_start +
				read_index * sizeof(a1599_ring_u32), &first_word);
			if (ret)
				return ret;
			declared = first_word & 0x7fU;
			if (declared < 2 || declared > sizeof(message->bytes))
				return -EPROTO;
			words = (declared + sizeof(a1599_ring_u32) - 1) /
				sizeof(a1599_ring_u32);
			if (words >= capacity)
				return -EMSGSIZE;
			if (available >= words) {
				memset(message, 0, sizeof(*message));
				message->length = declared;
				message->read_index = read_index;
				message->write_index = write_index;
				for (i = 0; i < words; i++) {
					a1599_ring_u32 word;
					a1599_ring_u32 slot = (read_index + i) % capacity;
					a1599_ring_u32 j;
					ret = ops->read32(context, ring_start +
						slot * sizeof(a1599_ring_u32), &word);
					if (ret)
						return ret;
					for (j = 0; j < sizeof(word) &&
					     i * sizeof(word) + j < declared; j++)
						message->bytes[i * sizeof(word) + j] =
							(word >> (8 * j)) & 0xffU;
				}
				return 0;
			}
		}
		if (ops->time_ns(context) - start >= budget_ns)
			return -ETIMEDOUT;
		ops->sleep_1_1_5_ms(context);
	}
}

/* Match only the reviewed firmware-declared A1599 ring layout. Resource
 * containment is checked here; silicon communication-RAM identity is not.
 */
static inline bool a1599_fw_abi_ring_snapshot_matches(
		bool experiment_enabled,
		a1599_ring_u32 h2m_descriptor,
		a1599_ring_u32 m2h_descriptor,
		a1599_ring_u32 h2m_read, a1599_ring_u32 h2m_write,
		a1599_ring_u32 m2h_read, a1599_ring_u32 m2h_write,
		a1599_ring_u64 h2m_start, a1599_ring_u64 h2m_end,
		a1599_ring_u64 m2h_start, a1599_ring_u64 m2h_end,
		a1599_ring_u64 resource_bytes)
{
	return experiment_enabled &&
		h2m_descriptor == 0x60cc01cc &&
		m2h_descriptor == 0x60000033 &&
		h2m_read == 0 && h2m_write == 0 &&
		m2h_read == 0 && m2h_write == 0 &&
		h2m_start == 0x80cc && h2m_end == 0x87fc &&
		m2h_start == 0x8000 && m2h_end == 0x80cc &&
		h2m_start < h2m_end && m2h_start < m2h_end &&
		h2m_end <= resource_bytes && m2h_end <= resource_bytes &&
		(h2m_end <= m2h_start || m2h_end <= h2m_start);
}

/*
 * Check a contiguous write into an empty ring while preserving one unused
 * slot to distinguish full from empty. This deliberately rejects wraparound;
 * callers may use the tail only and must not add padding or retry.
 */
static inline bool a1599_empty_ring_has_contiguous_space(
		a1599_ring_u32 capacity, a1599_ring_u32 read_index,
		a1599_ring_u32 write_index, a1599_ring_u32 words)
{
	if (!capacity || !words || read_index >= capacity ||
	    write_index >= capacity || read_index != write_index ||
	    words >= capacity)
		return false;

	return words <= capacity - write_index;
}

#endif
