# consumer‑6bbd 实机观察（2026-10-15，实测）

- RAM 会话：`20261014T182538Z-4d47b6f215c6`，物理口 1-9.3，bundle `94068c0d…`，模块 `6bbdcdf8…`；目标 boot ID `39a2e6e3-5272-4e4c-ae9e-b0d0ab066333`。
- 第一次 execute_session 在打开文件通道后立即 STOP：termios 读回中 c_cflag 多出内核写入的 CBAUD/CIBAUD（`0x10021002`），iflag/oflag/lflag=0、速度 115200 均正确；file-link TX 0 字节，receiver 未启动。属宿主比较逻辑缺陷，非设备异常。
- 修复：复制为 `task-3/a1599_consumer_trial_ready_20261015_termiosfix/`，仅改 `bound_transport.py` 的比较（屏蔽 baud 位，见 termios-fix.patch），35 项离线测试通过；原冻结包未改。同会话的一次性标记已改名为 `*.aborted-termios-cflag-before-device-io` 留档。
- 第二次（新 trial 目录 `evidence/a1599-consumer-6bbd-20261015-termiosfix`）：四文件 FILE-OK；输出快照 SHA256 `9f4d0fe4…`，与 1211 历史快照相同；NV12 1536/1536 可见字节一致。
- consumer：`C0_CONSUMER_ONCE ret=0 attempted=1 write_issued=1 verified=1 after_valid_count=6 H2M=10/10 M2H=3/3 resources_pinned=1`。

限制：3/3 仅为即时读回；不证明 DMA 结束，不许可缓冲复用/释放或第二帧。资源与模块仍驻留。
