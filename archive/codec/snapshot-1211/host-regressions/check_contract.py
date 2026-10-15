#!/usr/bin/env python3
"""Static host regressions for the offline A1599 INIT-ready candidate."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
MEMORY_H = (ROOT / "a1599_fw_static_boot_memory.h").read_text()
TABLES_C = (ROOT / "a1599_fw_first_ready_tables.c").read_text()
HARNESS_C = (ROOT / "a1599_fw_first_ready_platform_harness.c").read_text()
RING_H = (ROOT / "a1599_fw_ring_capacity.h").read_text()
DECODE_HARNESS_C = (ROOT / "a1599_fw_first_ready_platform_harness.c").read_text()
DECODE_STREAM_H = (ROOT / "a1599_fw_decode_stream.h").read_text()
DECODE_STREAM_C = (ROOT / "a1599_fw_decode_stream.c").read_text()


def macro(name):
    m = re.search(rf"^#define {name}\s+(0x[0-9a-fA-F]+|[0-9]+)U?$", MEMORY_H, re.M)
    if not m:
        raise AssertionError(f"missing macro {name}")
    return int(m.group(1), 0)


class CandidateContract(unittest.TestCase):
    def test_real_resource_path_is_preallocated_and_submission_is_default_closed(self):
        self.assertNotIn("decode_once_dryrun", DECODE_HARNESS_C)
        self.assertNotIn("synthetic_iovas=1", DECODE_HARNESS_C)
        self.assertIn("a1599_prepare_decode_map_entries", DECODE_HARNESS_C)
        self.assertIn("a1599_populate_decode_buffers", DECODE_HARNESS_C)
        self.assertIn("a1599_fw_first_ready_build_tables_experimental", DECODE_HARNESS_C)
        self.assertIn("dma_wmb();", DECODE_HARNESS_C)
        self.assertIn("submission_gate=%s", DECODE_HARNESS_C)
        self.assertIn('allow_decode_submission ? "open_one_shot_only" : "closed_default"', DECODE_HARNESS_C)
        self.assertIn("static bool allow_decode_submission;", DECODE_HARNESS_C)
        self.assertIn("module_param(allow_decode_submission, bool, 0400);", DECODE_HARNESS_C)
        self.assertIn("if (!allow_decode_submission)", DECODE_HARNESS_C)
        self.assertIn("a1599_decode_submit_once", DECODE_HARNESS_C)
        self.assertIn("A1599_DECODE_RESPONSE_TIMEOUT_MS 2000U", DECODE_HARNESS_C)
        self.assertIn("pixels_compared=0 decode_success=unclaimed", DECODE_HARNESS_C)
        self.assertIn("response_record_valid", DECODE_HARNESS_C)
        self.assertIn("A1599_DECODE_STREAM_WORDS 136U", DECODE_STREAM_H)
        self.assertIn("A1599_DECODE_STREAM_BYTES (A1599_DECODE_STREAM_WORDS * 4U)", DECODE_STREAM_H)
        self.assertIn("never submits the stream", DECODE_STREAM_C)
        self.assertIn("a1599_build_fixture_decode_stream(", DECODE_HARNESS_C)

    def test_all_backing_ranges_are_page_aligned_and_nonoverlapping(self):
        maps = [
            (macro("A1599_FW_STATIC_STAGING_IOVA"), macro("A1599_FW_STATIC_STAGING_BYTES")),
            (macro("A1599_FW_STATIC_EXTRA_IOVA"), macro("A1599_FW_STATIC_EXTRA_BYTES")),
            (macro("A1599_FW_STATIC_CONTEXT_LARGE_IOVA"), macro("A1599_FW_STATIC_CONTEXT_LARGE")),
            (macro("A1599_FW_STATIC_CONTEXT_MEDIUM_IOVA"), macro("A1599_FW_STATIC_CONTEXT_MEDIUM")),
            (macro("A1599_FW_STATIC_CONTEXT_SMALL_IOVA"), macro("A1599_FW_STATIC_CONTEXT_SMALL_BACKING")),
        ]
        self.assertEqual(macro("A1599_FW_STATIC_REQUIRED_MAPS"), len(maps))
        for start, size in maps:
            self.assertEqual(start % 4096, 0)
            self.assertEqual(size % 4096, 0)
            self.assertLessEqual(start + size, 0x100000000)
        for i, (start, size) in enumerate(maps):
            for prior, prior_size in maps[:i]:
                self.assertFalse(start < prior + prior_size and prior < start + size)

    def test_small_context_maps_its_real_backing_page(self):
        self.assertEqual(macro("A1599_FW_STATIC_CONTEXT_SMALL"), 0x400)
        self.assertEqual(macro("A1599_FW_STATIC_CONTEXT_SMALL_BACKING"), 0x1000)
        self.assertIn("maps[i].bytes != bytes", TABLES_C)
        self.assertIn("A1599_FW_STATIC_CONTEXT_SMALL_BACKING", TABLES_C)

    def test_tables_require_all_five_exact_maps_and_seal_iovas(self):
        self.assertIn("map_count != A1599_FW_STATIC_TOTAL_REQUIRED_MAPS", TABLES_C)
        self.assertIn("A1599_DECODE_RESOURCE_COUNT", TABLES_C)
        for field in ("extra_vxd_iova", "context_large_vxd_iova",
                      "context_medium_vxd_iova", "context_small_vxd_iova"):
            self.assertIn(field, TABLES_C)
        self.assertIn("a1599_validate_one_mapping_pages", TABLES_C)

    def test_decode_dma_lifetime_zeroes_rolls_back_and_pins_after_start(self):
        lifetime = (ROOT / "a1599_fw_decode_lifetime.h").read_text()
        memory_c = (ROOT / "a1599_fw_static_boot_memory.c").read_text()
        self.assertIn("a1599_decode_dma_allocate(&m->decode_lifetime", memory_c)
        self.assertIn("memset(view.cpu, 0, view.bytes)", lifetime)
        self.assertIn("rollback:", lifetime)
        self.assertIn("if (set->started)", lifetime)
        self.assertIn("return -EBUSY", lifetime)
        self.assertIn("a1599_decode_dma_mark_started(&m->decode_lifetime)", memory_c)

    def test_one_shot_submission_uses_real_map_root_fence_and_default_gate(self):
        submit = (ROOT / "a1599_fw_decode_submit.h").read_text()
        self.assertRegex(HARNESS_C,
                         r"static bool allow_decode_submission;\s*module_param\(allow_decode_submission, bool, 0400\);")
        self.assertIn("if (!allow_decode_submission)", HARNESS_C)
        self.assertIn("static ushort decode_flags = 0x4015;", HARNESS_C)
        self.assertIn("if (decode_flags != 0x4015U)", HARNESS_C)
        self.assertIn("static int id81_byte0_bit7;", HARNESS_C)
        self.assertIn("if (id81_byte0_bit7 != 0)", HARNESS_C)
        self.assertNotIn("a1599_init_succeeded", HARNESS_C)
        self.assertIn("atomic_read(&a1599_init_transport_consumed)", HARNESS_C)
        self.assertIn("INIT_TRANSPORT_CONSUMED=1 semantic_init_complete=unproven", HARNESS_C)
        self.assertIn("atomic_cmpxchg(&a1599_decode_consumed, 0, 1)", HARNESS_C)
        self.assertIn(".group_sequence = 1", HARNESS_C)
        self.assertIn(".root_device_address = a1599_memory->root.dma", HARNESS_C)
        self.assertIn(".selector = 0", HARNESS_C)
        self.assertIn(".opaque = 0x10211000", HARNESS_C)
        self.assertRegex(HARNESS_C,
                         r"static int id81_byte0_bit7;\s*module_param\(id81_byte0_bit7, int, 0400\);")
        self.assertIn("if (id81_byte0_bit7 != 0)", HARNESS_C)
        self.assertIn("a1599_decode_submit_once", HARNESS_C)
        self.assertIn("a1599_m2h_mmio_wait_message", submit)
        self.assertIn("c0_parse", submit)
        self.assertIn("consumer index untouched", submit)

    def test_probe_uses_shared_32_to_36_bit_dma_mask_contract(self):
        dma_contract = (ROOT / "a1599_fw_dma_mask.h").read_text()
        memory_c = (ROOT / "a1599_fw_static_boot_memory.c").read_text()
        self.assertIn("a1599_fw_dma_probe_validate", HARNESS_C)
        self.assertIn(".dma_addr_mask = A1599_FW_DMA_ALLOC_MASK", HARNESS_C)
        self.assertIn("A1599_FW_DMA_DEFAULT_MASK 0xffffffffULL", dma_contract)
        self.assertIn("A1599_FW_DMA_ALLOC_MASK 0xfffffffffULL", dma_contract)
        self.assertIn("a1599_fw_dma_mask_begin", memory_c)
        self.assertIn("a1599_fw_dma_mask_restore", memory_c)
        self.assertIn("dma_set_mask_and_coherent(context, mask)", memory_c)

    def test_output_export_is_fixed_size_default_off_and_snapshot_only(self):
        self.assertRegex(HARNESS_C,
                         r"static bool allow_output_export;\s*module_param\(allow_output_export, bool, 0400\);")
        self.assertIn("static BIN_ATTR_RO(decode_output, A1599_FW_DECODE_OUTPUT_BYTES);", HARNESS_C)
        self.assertIn("sysfs_create_bin_file(&dev->kobj, &bin_attr_decode_output)", HARNESS_C)
        self.assertIn("READ_ONCE(a1599_decode_output_snapshot_ready)", HARNESS_C)
        self.assertIn("memcpy(buf, a1599_decode_output_snapshot + offset, count)", HARNESS_C)
        self.assertIn("count = available", HARNESS_C)
        self.assertNotIn("copy_from_user", HARNESS_C)

    def test_wire_words_and_explicit_bounds_gate_precede_writes(self):
        self.assertIn("A1599_VERIFIED_COMM_RAM_START 0U", HARNESS_C)
        self.assertIn("A1599_VERIFIED_COMM_RAM_END 0U", HARNESS_C)
        self.assertIn("words[0] = 0x00008014", HARNESS_C)
        self.assertIn("words[3] = 0x00400100", HARNESS_C)
        self.assertIn("if (!a1599_comm_ram_range_verified(before))", HARNESS_C)
        self.assertIn("if (!allow_fw_abi_ring_init)", HARNESS_C)
        self.assertIn("independent_silicon_comm_ram_extent=unproven", HARNESS_C)
        send_start = HARNESS_C.index("static int a1599_submit_init_once")
        first_payload_write = HARNESS_C.index("a1599_init_mmio_send_once", send_start)
        gate = HARNESS_C.index("if (!a1599_comm_ram_range_verified(before))", send_start)
        self.assertLess(gate, first_payload_write)

    def test_optional_actions_default_off_and_snapshot_reads_six_words(self):
        self.assertRegex(HARNESS_C, r"static bool snapshot_once;\s*module_param\(snapshot_once, bool, 0400\);")
        self.assertRegex(HARNESS_C, r"static bool init_once;\s*module_param\(init_once, bool, 0400\);")
        self.assertRegex(HARNESS_C, r"static bool allow_fw_abi_ring_init;\s*module_param\(allow_fw_abi_ring_init, bool, 0400\);")
        self.assertIn("static DEVICE_ATTR_RO(ring_snapshot);", HARNESS_C)
        self.assertIn("static DEVICE_ATTR_WO(init_once);", HARNESS_C)
        self.assertIn("atomic_cmpxchg(&a1599_init_consumed, 0, 1)", HARNESS_C)
        registers = "0x2ff0, 0x2fe4, 0x2ff4, 0x2ffc,\n\t\t\t\t     0x2fe8, 0x2fec"
        self.assertIn(registers, HARNESS_C)
        snap = HARNESS_C[HARNESS_C.index("static int a1599_ring_snapshot"):HARNESS_C.index("static bool a1599_comm_ram_range_verified")]
        self.assertNotIn("iowrite", snap)
        self.assertNotIn("writel", snap)

    def test_sender_is_empty_ring_one_shot_and_does_not_wait_for_ack(self):
        self.assertIn("before->h2m_read_index != before->h2m_write_index", HARNESS_C)
        self.assertIn("a1599_empty_ring_has_contiguous_space(capacity, read_idx", HARNESS_C)
        self.assertNotIn("(read_idx + capacity - write_idx) % capacity", HARNESS_C)
        self.assertIn("dedicated_ack=not_required", HARNESS_C)
        self.assertIn("semantic_init_complete=unproven", HARNESS_C)
        self.assertNotIn("wait_for_ack", HARNESS_C)

    def test_empty_ring_capacity_helper_preserves_one_slot_and_forbids_wrap(self):
        self.assertIn("words >= capacity", RING_H)
        self.assertIn("read_index != write_index", RING_H)
        self.assertIn("words <= capacity - write_index", RING_H)
        self.assertIn("a1599_fw_test_empty_ring_capacity_boundaries", (ROOT / "a1599_fw_candidate_kunit.c").read_text())

    def test_firmware_abi_gate_is_exact_and_poll_is_bounded_single_shot(self):
        self.assertIn("h2m_descriptor == 0x60cc01cc", RING_H)
        self.assertIn("m2h_descriptor == 0x60000033", RING_H)
        self.assertIn("h2m_start == 0x80cc && h2m_end == 0x87fc", RING_H)
        self.assertIn("m2h_start == 0x8000 && m2h_end == 0x80cc", RING_H)
        self.assertIn("return experiment_enabled &&", RING_H)
        self.assertIn("INIT_PREWRITE_RECHECK_MISMATCH", HARNESS_C)
        for iova in ("0x40100000", "0x40300000", "0x40340000"):
            self.assertIn(iova, HARNESS_C)
        self.assertIn("A1599_INIT_CONSUME_TIMEOUT_MS 2000U", HARNESS_C)
        self.assertIn("usleep_range(1000, 1500)", HARNESS_C)
        self.assertIn("a1599_init_mmio_wait_consumed(&a1599_production_mmio_ops", HARNESS_C)
        self.assertIn("retry=0", HARNESS_C)
        self.assertIn("INIT_CONSUME_UNCONFIRMED", HARNESS_C)
        send = HARNESS_C[HARNESS_C.index("static int a1599_submit_init_once"):]
        self.assertLess(RING_H.index("if (observed[i] != expected_regs[i])"),
                        RING_H.index("ops->write32(context, ring_start +"))
        self.assertIn("a1599_init_mmio_send_once(&a1599_production_mmio_ops", send)
        self.assertIn("a1599_init_mmio_wait_consumed(&a1599_production_mmio_ops", send)
        self.assertEqual(RING_H.count("ops->write32(context, 0x80, 1)"), 1)

    def test_ring_snapshot_resource_bounded_log_is_computed(self):
        self.assertIn("a1599_ring_resource_bounded(window, s)", HARNESS_C)
        self.assertIn("resource_bounded=%d", HARNESS_C)
        self.assertNotIn("resource_bounded=1 comm_ram_proven", HARNESS_C)

    def test_production_mmio_path_has_fake_backend_scenarios(self):
        mock = (ROOT / "host-regressions/test_init_mmio_mock.c").read_text()
        for marker in ("test_changed_descriptor_rejects_before_writes",
                       "test_producer_readback_mismatch_suppresses_kick",
                       "test_consumer_progress_after_single_kick",
                       "test_consumer_timeout_is_bounded_single_shot"):
            self.assertIn(marker, mock)
        self.assertIn("a1599_init_mmio_send_once", RING_H)
        self.assertIn("a1599_init_mmio_wait_consumed", RING_H)


if __name__ == "__main__":
    unittest.main(verbosity=2)
