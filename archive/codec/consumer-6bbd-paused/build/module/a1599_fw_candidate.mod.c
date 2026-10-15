#include <linux/module.h>
#include <linux/export-internal.h>
#include <linux/compiler.h>

MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
	.arch = MODULE_ARCH_INIT,
};

KSYMTAB_FUNC(a1599_fw_first_ready_candidate_run, "");
SYMBOL_FLAGS(a1599_fw_first_ready_candidate_run, 0x01);
KSYMTAB_FUNC(a1599_fw_first_ready_experiment_validate, "");
SYMBOL_FLAGS(a1599_fw_first_ready_experiment_validate, 0x01);
KSYMTAB_FUNC(a1599_fw_first_ready_experiment_run, "");
SYMBOL_FLAGS(a1599_fw_first_ready_experiment_run, 0x01);
KSYMTAB_FUNC(a1599_fw_static_boot_memory_validate_tables, "");
SYMBOL_FLAGS(a1599_fw_static_boot_memory_validate_tables, 0x01);
KSYMTAB_FUNC(a1599_fw_first_ready_build_tables, "");
SYMBOL_FLAGS(a1599_fw_first_ready_build_tables, 0x01);

MODULE_INFO(depends, "");

MODULE_ALIAS("of:N*T*Capple,s5l8960x-vxd-first-ready-experiment");
MODULE_ALIAS("of:N*T*Capple,s5l8960x-vxd-first-ready-experimentC*");
