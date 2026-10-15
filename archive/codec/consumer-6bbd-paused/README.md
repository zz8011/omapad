# A1599：一次 C0 consumer 观察候选

2026-10-14，离线构建。此目录独立于已实机通过的 1211 候选。完整原始指令依据、限制和下一次观察范围见 [RESULT.md](../a1599_ring_lifetime_20261014/RESULT.md)。

新增行为仅为默认关闭的 `allow_c0_consumer_observation` 参数和一次性的 `c0_consumer_once` 入口：在既定成功单帧快照和 pinned 资源条件下，最多读 21 个 MMIO words，唯一写为 M2H consumer `0x2fe8=3`。这个动作没有执行过，不授权连续 IDR 或 DMA 复用/释放。外部主机必须先核对像素；内核入口只核验快照和环状态。

关键文件：

- [a1599_fw_c0_ack.h](a1599_fw_c0_ack.h)：实际有限事务，逐项前置检查、一次写入与 readback。
- [a1599_fw_first_ready_platform_harness.c](a1599_fw_first_ready_platform_harness.c)：默认关闭的 gate、mutex、日志、CPU 快照发布顺序。
- [CHANGE.patch](CHANGE.patch)：相对 1211 的生产文件差异。
- [VERIFICATION.json](VERIFICATION.json)：模块 SHA256、构建/回归结果和基线保全结果。
- [HOST-REGRESSIONS.log](build/HOST-REGRESSIONS.log)：27 条构建/执行命令，0 失败。
- [a1599_fw_candidate.ko](build/module/a1599_fw_candidate.ko)：仅构建、未加载的 ARM64 实验模块。

主机复核可运行 `python3 host-regressions/run_all.py`。该目录依赖相邻 `../id81_host_contract`，冻结清单另外记录其文件哈希。本目录不是完整 RAM 启动/传输包；不得将旧会话参数直接用于新设备动作。

原 1211 模块 hash：`1211a6abbf026a9354f05a44e710c16ec7e8286cb0708b86b9ee260aa59bbbaa`。原目录 118 个文件均保持不变。新模块 hash 见下面的冻结记录。

本轮新模块 SHA256：`6bbdcdf835923dbd57c1e00a7a8180307c3c856ac0694a8cf83b9d41869e8193`。

构建使用已有 Linux 源码 `/home/zz8011/Documents/Codex/2026-10-11/task-2/.build/a1599/src/linux`、已有配置输出 `/tmp/a1599-kbuild-decode`，参数 `ARCH=arm64 LLVM=1 KCFLAGS=-DCONFIG_A1599_FW_EXPLICIT_EXPERIMENT`，`M` 指本目录、`MO` 指本目录 `build/module`。没有安装软件或模块。
