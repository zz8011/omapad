# A1599 consumer 冻结执行包独立复核

审核日期：2026-10-15（Asia/Shanghai）。审核员：task-4，同一独立审核线程。

## 结论

**本次执行包的离线技术复核通过，未发现需要阻断的实现问题；但执行仍存在尚未解决的独立平台阻碍，不能据此认定已具备执行条件。此结论不构成设备执行批准；用户尚未批准设备执行。**

**停止状态补注（2026-10-15）：**来源线程 `01a0f04c-cf59-77b1-ad7e-7178575cb015` 补充告知，作者此前 turn `01a13b3c-acaa-7140-b6de-4544bc4409b9` 返回平台 `possible cybersecurity risk` 拦截，当前尚未解决。此项为来源线程报告的独立平台阻碍；本审核员未重新验证或处理该拦截。离线测试通过不解除该拦截，用户执行批准也不能被本报告当作拦截已解除的证据。保持停止，不执行设备操作，也不继续准备或测试同一试验。本次补注仅修改报告。

通过范围是下述精确冻结对象、唯一操作者、绑定同一 A1599、新 RAM 会话、一次 INIT/ID81，以及真实像素 gate 通过后的一次 consumer 观察。不得把通过扩大到第二帧、DMA 复用/释放、自动恢复或额外采证。

我实际读取了 REVIEW-REQUEST.md、FREEZE.json、执行和目标脚本、绑定/传输/证据代码及相关测试；独立比对了原始镜像和 launcher，实际运行了离线测试。没有调用 execute_session 或 bound_launcher 的设备入口，没有访问 USB/真实 tty、加载/卸载模块、reset 或 reboot，没有修改作者文件。

## 冻结与来源

原包：`/home/zz8011/Documents/Codex/2026-10-14/task-3/a1599_consumer_trial_ready_20261014/`。

| 对象 | 核验结果 |
|---|---|
| DELIVERY.sha256 | 44 项逐文件核验通过；清单 SHA256 `88985614ae72ae1b3e37f5c906d6f8cdc4866ce56edcad0ab3af366f9eb1b4ac` |
| 模块，889384 bytes | `6bbdcdf835923dbd57c1e00a7a8180307c3c856ac0694a8cf83b9d41869e8193`，与此前已审对象相同 |
| 新 bundle，13826458 bytes | `94068c0d4a606f2edb7517fca455fdbf19c3de1adba766fa6554d4b71346b1af` |
| file-recv，12872 bytes | `7515a14756613d825258a4022318fc2c127a1b53357470010b146181d5e08d05`，原字节不变 |
| INPUTS.json | 全部 10 项长度和 SHA256 核验通过 |
| HISTORICAL-SOURCES.json | 全部 12 项长度和 SHA256 核验通过；历史材料只用于来源/回归，不当作新设备结果 |

审核结束重新核验原包 44 项，全部仍与冻结清单相同。审核副本中的测试产物与原冻结日志分开保存。

## 镜像、启动器及严格握手

独立解析旧、新 bundle 内的 gzip/newc，比较每个条目的元数据与内容：538 个旧条目变为 539 个。既有条目只有 `etc/inittab` 改变，精确替换原 file-recv respawn 行；唯一新增条目是 `sbin/a1599-start-receiver`，内容与被冻结启动脚本相同。9553530-byte 启动前缀完全相同；kernel、原接收器、gadget 配置及其他条目未变。

新 launcher 与原已审 launcher 的差异精确等于 bundle 文件名和 SHA256 两项替换。`bound_launcher.py`、`usb_binding.py` 与此前已审版本逐字节一致；显式端口、芯片指纹、控制器/hub/leaf/tty 实例检查、绑定一次、无任意 tty 回退及 BindingError→WrapperError 清理路径保留。

`execute_session.py` 先持有全局互斥锁，打开文件 FD，配置并读回 raw/no-echo，再打开维护 FD；文件 FD 在后续全过程保持打开。文件通道静默检查 2 秒后，才通过维护通道按 SHA 验证并启动 receiver。目标启动脚本检查 tmpfs、空 incoming、无已加载模块/receiver、原接收器 hash，并使用 mkdir 哨兵只启动一次，无 respawn。

`expect_startup()` 单独消费一次精确 startup READY，随后 `send_reviewed()` 只接受 HELLO 对应的一次 READY。PUT、FILE-READY、载荷及 CRC ACK 严格对应；同块多余内容和安静窗口内额外内容都会停止。旧的“容忍首次 PUT 前额外 READY”逻辑已删除。关闭 FD 不恢复继承的 ECHO，也不 flush 丢弃输入。

这消除了本次设计针对的启动/握手竞争机制；软件 PTY 通过不等于已验证真实 CDC/USB 时序，也不证明旧现场 ECHO 原因唯一。

## 像素 gate、同会话及一次 consumer

`session_flow.py` 的实际顺序为：receiver → 四文件传输 → decode → verify_pixels → 写 PIXELS-VERIFIED.json → consumer → verify_consumer。consumer 命令只在 `verify_pixels()` 正常返回之后构造并发送。

像素 gate 要求封装长度/hash 完整、同 boot ID、唯一 INIT/ID81/DECODE/C0 记录、无 HW_PANIC、唯一正常 12-byte C0/fence=1/class10、脚本/导出成功、4096-byte 快照及 guard 正确、唯一完成标记。随后实际执行 FFmpeg，核对固定软件参考 hash，再运行冻结比较器，要求唯一 NV12 且 1536/1536 可见字节一致。它没有把原有成功记录或纯内核 snapshot_ready 当作本次像素通过。

新随机 nonce 在接收器启动时保存在目标 RAM；consumer 请求传入该 nonce、同一 boot ID 和本次已验证输出的 SHA256。目标重新核验这三者、先前脚本结束状态及输出文件 hash，再 mkdir 建立 consumer 一次性哨兵，最后只写一次 `c0_consumer_once`。输出 hash 与 boot/nonce 是受控执行链的关联检查，不是抵抗恶意目标的认证协议。

模块 hash 未改变，因此此前已审的上限继续适用：consumer 动作最多增加 21 次 32-bit MMIO 读、唯一一次 `0x2fe8=3` 写；包括两轮前置寄存器检查、三个已保存 C0 payload words 和一轮 readback。写前屏障、attempted/write_issued/verified/after_valid_count 分离、失败不重试、原 decode latch 与 DMA pin 保持不变。新包未增加第二次 decode、IRQ clear、kick 或释放路径。

成功结果还须由完整回传的唯一 `C0_CONSUMER_ONCE` 记录确认全部字段：ret=0、attempted/write_issued/verified=1、after_valid_count=6、H2M=10/10、M2H=3/3、resources_pinned=1，以及第二帧/复用均禁止。

## 时限、部分执行及证据

主机会话和 trial 的 O_EXCL 标记在设备 I/O 前建立；每阶段目录也以不覆盖方式建立。目标 receiver、decode、consumer 各有一次性目录哨兵。失败不会删除标记或重发动作。

内部会话时限为 300 秒；清单另用 310 秒宿主 timeout 和 5 秒 kill-after。receiver 命令 15 秒、decode 120 秒、consumer 30 秒，FFmpeg/比较器各 30 秒；协议有单次写和应答期限。总时限约束实际总耗时，而不是保证各阶段都能用满各自预算。

这些是宿主等待/终止边界，不是取消已经开始的目标操作。超时、丢回显、部分写入一律可能已经发生设备动作；不能因此重试，也不能把没有 FINAL-RESULT 或 STOP 文件解释为零设备副作用。外层强制终止时目标脚本可能仍在运行。

两个通道实际接收/发送的字节持续保存至 raw 文件，接收先落盘再做后置实例检查；另保存事件与 termios 读回。阶段命令发送前存档。阶段完成时保存返回内容和 rc；未完成时仍以总通道 raw 为证据。目标 EXIT 路径封装已有 status/dmesg/stderr/快照；host 解包校验并保留原始及失败产物。失败后无额外设备诊断、恢复、卸载或重启；资源保持 pin。

## 独立执行的验证

1. **35/35 原包 unittest 方法通过**，在 task-4 审核副本运行。涵盖 USB fake-tree/FD 门禁、原 launcher 清理、真实历史像素与实际 FFmpeg 比较、像素单字节损坏、错误 boot、重复 ID81、部分证据拒绝、流程顺序，以及普通文件隔离的 consumer shell。
2. **原 AArch64 file-recv + QEMU 用户态 + 软件 PTY，通过四个实际冻结载荷**：模块 889384、固件 57344、target-once 5062、target-consumer 2770 bytes。逐文件 SHA/内容一致，零 FILE-ERR、零接收器 restart；RX/TX hash 与作者冻结验证记录完全一致。模块仅当作普通文件载荷传输，未执行。
3. **审核员新增 13 项检查全部通过**：正常 consumer 验证、错误 nonce/boot/输出 hash、部分 readback、producer=8、重复 consumer、第二 ID81、缺失完成标记、截断封装、阶段部分标记超时、同块及稍后额外协议回复。负例均拒绝；超时只发送一次命令，多余协议回复前未发 PUT/payload。
4. `prepare_host.py` 仅普通文件操作验证通过，输出到 task-4 新目录；FFmpeg 软件参考重新生成并匹配，staging 清单通过。三个目标脚本 `sh -n` 通过。
5. 独立镜像/启动器/输入比对及原包审核前后冻结核验通过。

验证过程中一次审核副本 QEMU 启动未收到 READY：原因是审核员使用 copyfile 复制 ELF 后未保留 0755 执行位，副本为 0644。恢复原执行位后重跑通过；失败记录保留。这不是原包实现缺陷或设备失败。

软件 PTY 验证执行的是原 receiver 与真实文件协议；它没有执行完整设备入口、真实 USB 绑定、启动流程或硬件 sysfs 调用。目标 shell 的运行测试使用明确映射到临时普通文件的测试环境，不能称为设备验证。

## 执行批准边界

以下仅记录已审技术动作范围，不是当前执行建议或继续推进的许可。上述独立平台阻碍尚未解决，保持停止；不能仅凭本报告或新增用户批准认定已经满足执行条件。

已审范围为：结束当前 1211 RAM 会话，进入一次新的纯 RAM 启动，加载指定 94068… bundle 和 6bbd… 模块，一次 INIT/ID81，主机确认 1536 字节像素完全匹配后追加最多 21 读和一次 consumer 写。当前 RAM 状态丢失及本次新实验可能异常/失联均属于需明确说明的风险。

把像素确认和 consumer 观察放在同一次新 RAM 会话可避免为这两步分开启动；不承诺后续多帧开发无需新启动。consumer=producer=3 **只证明当时的读回**：不能证明 DMA 已结束、所有私有缓冲最后访问点已闭合、未来没有迟到消息，也不授权释放、复用或第二帧。

## 审核证据路径

- `freeze-verified.json`：本次冻结对象。
- `independent-image-input-verification.json`、`inittab.diff`：独立镜像/输入核验。
- `unittest.log`：35 项实际执行结果。
- `receiver-independent.log`：审核副本执行位失误的初次失败记录。
- `receiver-independent-rerun.log`：修复审核副本执行位后的 QEMU 验证。
- `a1599_consumer_trial_ready_20261014/checks/RECEIVER-QEMU.{json,rx,tx}`：审核副本的独立接收器产物。
- `review_additional_checks.py`、`review-additional-results.json`：新增 13 项检查与结果。
- `prepare-host-independent.log`、`prepared-trial/`：只涉及普通文件的准备验证。
- `final-verification.json`：结束时原包仍冻结、独立结果核验。

以上相对路径均位于 `/home/zz8011/Documents/Codex/2026-10-14/task-4/consumer-package-review/`。
