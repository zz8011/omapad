obj-m += a1599_fw_candidate.o
a1599_fw_candidate-y := a1599_fw_linux_backend_skeleton.o \
		       a1599_fw_static_boot_memory.o \
		       a1599_fw_first_ready_tables.o \
		       a1599_fw_first_ready_platform_harness.o \
		       a1599_fw_decode_stream.o \
		       a1599_fw_id81.o

# Optional in-module regressions; requires a kernel built with CONFIG_KUNIT.
a1599_fw_candidate-$(CONFIG_KUNIT) += a1599_fw_candidate_kunit.o
