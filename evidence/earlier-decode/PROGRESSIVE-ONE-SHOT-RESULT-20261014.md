# Progressive-mode 修正版单帧试验结果

日期：2026-10-14 UTC

## 范围和产物

- 用户批准本次唯一一次 RAM 单帧试验；没有内部存储写入。
- 目标 DFU 初始枚举：Apple `05ac:1227`，USB hub path `1-9.3`。
- 固定 launcher session：`20261014T123404Z-4c103d2be7e9`。Launcher 核对的固定 bundle SHA-256：`a069bf7743ac234a463e106f6eacce366fd5a89ca8d10a467e1cf93b790bb074`；Pongo banner 校验后上传 bundle 并发送 `bootm`，未发送 post-boot 服务命令。
- RAM Linux gadget 后续枚举为 `1d6b:0104`，ACM0/1/2 映射到 hub path `1-9.3` 的接口 `:1.0`、`:1.2`、`:1.4`。执行时使用了绑定该实际 hub 身份的临时 host helper；固定 decode candidate 源文件未修改。
- 模块 `866280` bytes，SHA-256 `4367df89658bdbcee535129f768180c2fc239baf1eaae87896079a2061687b4c`，目标 ACM1 CRC ACK `d6de8207`。
- VXD firmware `57344` bytes，SHA-256 `6b274c576a7952d7a4f22f65adf5b1041c4329c3dfc7ce761de53668cfd4a41e`，目标 ACM1 CRC ACK `1af336b2`；RAM `/lib/firmware/a1599/` 中副本 hash 匹配。

## First-ready 和 INIT

- 目标内核：`7.3.0-rc1 #4 SMP PREEMPT`，root 为 `rootfs`，`modules_disabled=0`；`file-recv` 和 `displayd` 均存活，初始 `/run/incoming` 为空，之前无候选模块或 VXD node。
- `insmod` 只执行一次，参数为 `run_once=1 snapshot_once=1 init_once=1 allow_fw_abi_ring_init=1 allow_decode_submission=1 allow_output_export=1 decode_flags=0x4015 id81_byte0_bit7=0`，`INSRET=0`。
- 先前失败的 overlay 符号问题已解决：live DT preflight 通过，target-path-only overlay 成功应用（ID 1）；VDEC genpd attach 成功，runtime-PM resume held，domain on、usage 1。
- first-ready signature `0xa5a5a5a5` 在 131 ms 到达；firmware upload、PC 写入/回读及 start 均成功。
- 一次 INIT 传输被消费：H2M read/write `0→5`，`INIT_TRANSPORT_CONSUMED=1`。语义 INIT 仍未证明。
- 试验保留的未验证 DMA/MMU/IOVA/coherency/ordering 假设仍适用；first-ready 不代表解码成功。

## 唯一 ID81 decode 及固定结果门

- 提交前 ring snapshot：validation 0，H2M `5/5`，M2H `0/0`，符合候选硬编码提交门。
- 日志中的 136-word command 含修正值：byte offset `0x004` 的 H+4 为 `0x00001008`；byte offsets `0x0c8` 和 `0x0f0` 为 `0x10080000`。
- 恰好一次 `decode_once=1` 写入。维护 shell 在 30 秒内未返回完成标记；后续只读 ACM0 查询确认命令已完成，内核记录 `consumed=1`、`published=1`、`h2m_consumed=1`、`response_received=1`、有效 C0，`retry=0`。
- C0 原始 12 bytes：`8c c0 01 00 03 00 00 00 d5 44 06 60`。status `0x600644d5`，为 `nonzero-no-detail`；`LastMB=3`，detail 缺失。驱动返回 `-121`。
- 提交后 ring：H2M `10/10`，M2H `0/3`（而不是旧诊断中的 `0/8`），validation 0。
- `guard_checked=0`、`guard_state=not_checked`、`output_snapshot_ready=0`、`output_stable_50ms=0`。因此没有声称帧解码成功，也未读 `decode_output` 或执行像素比较。
- 按用户限定，固定 32-byte `m2h_ring_raw_once` 接口只尝试一次，恰传入 `bs=32 count=1`。内置状态门要求 H2M `10/10` 且 M2H `0/8`，本次 M2H 是 `0/3`，接口返回 `Stale file handle`；目标 `/run/a1599_m2h_raw.bin` 为 0 bytes。未放宽条件、未增加读取，也未再触发 decode。

## 停止状态

- 候选模块仍驻留，日志为 `resources_pinned=1`；DMA mappings 和 runtime-PM reference 保留。
- `file-recv`（PID 106）及 `displayd`（PID 107）仍运行。没有卸载模块、移除 overlay、reset/reboot、额外 MMIO、再次 decode、追加 M2H 读取或写入内部存储。
- 本次观察到的新结果是有效 C0 `0x600644d5` / M2H `0/3`，不同于旧结果 `0x48064455`；其原因尚未确定。
