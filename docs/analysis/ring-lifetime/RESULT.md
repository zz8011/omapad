# A1599 M2H consumer 与 DMA 生命周期：离线结果

2026-10-14。仅主机文件分析、原始指令回放、编译和离线回归；没有设备访问。

**已经闭合 M2H consumer 的地址、单位、回绕和写入顺序，也闭合了 Apple 正常 C0 → 完成处理 → 下一作业调度的主机控制流。尚未闭合每个私有 DMA 缓冲的最后访问点，因此本轮交付一次 consumer 观察候选，不开放连续 IDR、缓冲复用或释放。**

## 当前基线和日志来源

1211 候选已有单次 INIT、单次 ID81 和可见 NV12 像素逐字节匹配；本轮没有重做试验。冻结模块 SHA256 为 `1211a6abbf026a9354f05a44e710c16ec7e8286cb0708b86b9ee260aa59bbbaa`。其整个候选目录的 118 个文件与本轮开始时逐文件 SHA256 和文件集合完全一致。

日志来源核对见 [LOG-PROVENANCE.txt](../next_stage_plan_20261014/LOG-PROVENANCE.txt)。1211 的原始串口档案是 [execute/console.raw](../evidence/a1599-snapshot-next-trial-1211/execute/console.raw)，它包含回传文件的 base64；解包的 [dmesg.after.txt](../evidence/a1599-snapshot-next-trial-1211/received/dmesg.after.txt) 第 280–282 行记录 `actual_length=12` 和 `8c c0 01 00 03 00 00 00 d5 44 06 60`。独立保存的 [c0-001.bin](../evidence/a1599-snapshot-next-trial-1211/received/c0-001.bin) 是这 12 个字节。旧 4367 的 ACM0 查询只有工具输出转录，不能将后续 1211 原始档案冒充其原始日志。

## 原始二进制定位

唯一指令依据为 iOS 12.5.8 / 16H88 `kernelcache.release.ipad4bm.decompressed`，SHA256：

`76269e79b4715783b95407acbb54df2369116c76709a03f24e0fe95924199eb9`

完整路径、12 个原始片段的 VA、文件偏移、原始字节 SHA256 见 [SOURCES.json](evidence/SOURCES.json)。本报告所有所列片段满足 `文件偏移 = VA - 0xfffffff007004000`；范围右端按下文说明取值。相邻 `.bin` 为原件摘取，`.txt` 为带每条指令偏移的反汇编。

## 已证明：consumer 交还的是环槽

| 原始位置（VA；文件偏移） | 数据流及结论 |
|---|---|
| `0xfffffff008682640–2658`；`0x167e640–e658` | 读 `core<<16 | 0x2fe4`；高 16 位写 context+0x60，表示 ring byte offset；低 16 位写 +0x64，表示容量 words。当前 `0x60000033` 给出 offset=0x6000、capacity=51。 |
| `0xfffffff008681680–1690`；`0x167d680–d690` | consumer=`core<<16 | 0x2fe8`，producer=`core<<16 | 0x2fec`；通信窗口基点为 `core<<16 | 0x2000`。 |
| `0xfffffff0086816a8–16e0`；`0x167d6a8–d6e0` | 从 `corebank+0x2000+context[0x60]+4*read` 读 word0，写接收记录+0x10；长度取 word0&0x7f，word 数为 `(len+3)>>2`；read 逐 word 加一，到 capacity 回到 0。 |
| `0xfffffff0086816e8–171c`；`0x167d6e8–d71c` | 拷贝余下 words，每个 word 都执行同样回绕。原循环没有在所查路径先核验 available words≥声明长度；候选自己的完整记录检查比它严格。 |
| **`0xfffffff008681720–172c`；`0x167d720–d72c`** | **把更新后的 read 写到 consumer。发生在完整消息拷贝后、消息类型分派前。当前 0/3 的 12-byte C0 对应唯一写 `0x2fe8=3`。** |
| `0xfffffff008681730`；`0x167d730` | 此时才读 byte1，随后跳转到 C0/C1 等类别分支。 |
| `0xfffffff008677ef0–7f10`；`0x1673ef0–3f10` | MMIO helper 使用全局映射基址 + w1，按全局模式可字节翻转后执行 store；这个 helper 未附加 kick、显式屏障或 readback。 |

证据：[M2H-RECEIVER.txt](evidence/M2H-RECEIVER.txt)、[MMIO-ACCESSORS.txt](evidence/MMIO-ACCESSORS.txt)、[RING-DESCRIPTOR-DECODE.txt](evidence/RING-DESCRIPTOR-DECODE.txt)。

IRQ 清除是独立动作：接收器 `0xfffffff00868165c–1670` 根据 0x608 bit14 写 0x60c=0x4000；父中断路径 `0xfffffff00867a970` 也有清除。**consumer 写、IRQ 清除、硬件 DMA 完成不能互相替代。** 本轮候选没有增加 IRQ 清除。

C0 分支 `0xfffffff0086817b4–17e4`（文件 `0x167d7b4–d7e4`）读取 fence+2、字段+4 和 flags+8；bit27 清零分类为 0x10，置位走 0x8010。普通 12-byte C0 不读取 +12 detail。没有把 flags 的 bit7/29/30 解释成 DMA fence，也没有把 LastMB 当内存索引或完成证明。

## 已证明：Apple 主机按正常 C0 完成作业并可调度下一项

| 原始位置（VA；文件偏移） | 已观察到的主机行为 |
|---|---|
| `0xfffffff00867a950`；`0x1676950` | 调用 M2H 接收器。 |
| `0xfffffff00867a994–a9a4`；`0x1676994–69a4` | 经虚表取消 timer，再调用完成入口 `0xfffffff00867c8e8`。先前 timer 追踪解析为 `IOTimerEventSource::cancelTimeout`，不等价于硬件等待。 |
| `0xfffffff00867cb1c–cb30`；`0x1678b1c–8b30` | 取 `driver+core*0xb18+0xe38` 的 surface，调用虚表+0x228，参数 w1=0；错误日志识别为 deviceUnLockSurface。 |
| `0xfffffff00867cb74–cb8c`；`0x1678b74–8b8c` | 调用 `ProcessHWResponse`（`0xfffffff008680534`，文件 `0x167c534`）；正常 class 0x10 路径传递错误值 0。该函数的通知及 pending 计数更新见原始片段。 |
| `0xfffffff00867cb98–cbd8`；`0x1678b98–8bd8` | 减 active count；清 current-client、busy 标记和长度 0xb18 的 job 记录，再清对应 bookkeeping。 |
| `0xfffffff00867cbe0–cca0`；`0x1678be0–8ca0` | mode byte+0x2c10 为 1、存在 queued job 且 active=0 时，调用下一项函数 `0xfffffff00867dd98`。 |
| `0xfffffff00867df3c–df5c`；`0x1679f3c–9f5c` | 出队后保存新的 client，调用 `submitCommandInternal` 入口 `0xfffffff00867a044`。 |

这证明主机正常完成边界与下一作业入口的顺序，不能直接扩大成 Linux 当前七块私有资源都可立即覆写的证明。

### 新收窄项：IOSurface 基类解锁不等待 VXD

`0xfffffff007f40b38–b58`（文件 `0xf3cb38–cb58`）的 metaclass 构造使用名称 IOSurface，metaclass 为 `0xfffffff00895c268`；分配函数 `0xfffffff007f399c4–9a10`（文件 `0xf359c4–5a10`）设置对象虚表为 `0xfffffff007475d60`。

该虚表 +0x228（文件 `0x471f88`）解析到 **`0xfffffff007f3ee90`**（文件 `0xf3ae90`）。在 w1=0 时，它调用 +0x1b8，再递增 `*(surface+0xc0)+0x10`。基类 +0x1b8（文件 `0x471f18`）指向 **`0xfffffff007f3cb70`**（文件 `0xf38b70`）：获得软件锁、递增 shared record+0xc、置 surface+0x22/+0x23/+0x27 状态、释放软件锁。两者没有 VXD MMIO 完成轮询或硬件 fence 等待。

见 [IOSURFACE-VTABLE.json](evidence/IOSURFACE-VTABLE.json)、[IOSURFACE-UNLOCK.txt](evidence/IOSURFACE-UNLOCK.txt)、[IOSURFACE-MARK.txt](evidence/IOSURFACE-MARK.txt)。**剩余不确定性已收窄到实际作业 surface 的动态类型：尚未把 surface handle lookup/factory → 作业记录 → e38 的完整生产链闭合，因此不能排除 +0x228 或嵌套 +0x1b8 的覆盖。** 不再笼统声称“基类 unlock 内部可能等待 DMA”。

## 原始指令离线回放与限制

[replay_receiver.c](tests/replay_receiver.c) 在 qemu-aarch64 用户态映射原 kernelcache 副本，执行其原始 ARM64 指令，不执行设备固件。接收器的 MMIO 访问由有界数组替身承接。

- 接收器 **408 个组合**：2 个 core bank × 51 个起点 × 长度 12/13/16/127。验证逐 word 拷贝、环尾回绕、正确 bank 的 consumer 更新，以及 consumer 写时尚未分类而完整 payload 已在 CPU 接收记录内。非 12 长度只检验长度运算，不声称其为有效 C0 ABI。
- 完成路径 **2 个组合**：有/无待执行 job。执行原完成函数、IOSurface 基类 unlock/+1b8、下一项调度函数；验证 unlock → notify → record clear → dequeue → next submit 的顺序和 surface 状态/计数。
- 被替换边界：`0x8677e84/7ef0/7de4` 的 MMIO/log；`0x869c66c/9c5f8` 的 queue count/registration；`0x7770abc/7718d0` 的锁；`0x7a6ff04` trace；`0x8680534` client notify；`0x75b56c0` bzero；`0x869bc34` dequeue；`0x867a044` next-submit；`0x7c40c34/40d64` IOSurface 软件锁（这里均为回放映射地址）。
- 所以该回放证明相应控制流与可见内存效果，**不模拟固件 DMA、不证明真实对象动态类型、不把 next-submit 替身当作第二次硬件提交**。无队列组合有意跳过闲置电源管理分支。

[verify_evidence.py](tests/verify_evidence.py) 重新核对 kernelcache SHA、12 段字节、3 个虚表槽、固件 text/data 和旧候选 118 文件，并编译执行回放。结果见 [EVIDENCE-VERIFICATION.json](evidence/EVIDENCE-VERIFICATION.json) 及 [RECEIVER-REPLAY.log](evidence/RECEIVER-REPLAY.log)。

## 尚未证明的 DMA 条件

固件 staging 与 kernelcache 内 text/data 已逐字节核对一致，但**尚未定位发送 C0 的固件指令及其之前各 DMA 队列的 drain/order 点**；没有可交付的固件 C0 emitter VA/offset。text 为 kernelcache 文件 `0x14eef8`、长度 `0x66dc`；data 为 `0x1555fc`、长度 `0x820`。原件定位不等于顺序证明。

| 对象 | 可以确认的当前行为 | 复用/释放前仍需闭合 |
|---|---|---|
| M2H 三个 message words | 完整拷贝后 Apple 写 consumer 交还环槽 | 当前候选仅允许这个特定记录；一般异步消息队列和多消息关联另行实现 |
| COMMAND、BITSTREAM | 现 job 已获得正常 C0；H2M consumer 已前进 | 固件/引擎最后一次间接读取与 C0 发布之间的顺序；H2M 被消费本身不够 |
| PRELOAD、VLC | 现固定 fixture 使用的表有稳定内容 | 是否跨 job 保留指针，以及是否只读；暂时保持原地址、内容和映射 |
| CURRENT_PICTURE、INTRA | 一次作业使用，原候选始终 pin | 私有元数据/参考状态是否跨 IDR 保存，哪些引擎仍访问、何时失去引用 |
| OUTPUT | coherent CPU 读与既有像素核对已成功 | 将其覆写、回池、解除映射或释放前的最后硬件访问；两次 50 ms 快照相等不是无限期静止证明 |
| 五个 INIT 映射及 hardware context（含额外 0x2000 backing） | 会话级固定映射，启动后不释放 | 逐对象角色、跨 job 状态、停止与硬件静止契约；正常单帧 C0 不意味着整个会话已停止 |

已排除把 consumer/IRQ clear/timer cancel/基类 IOSurface unlock 本身当作 DMA drain 的路径。未闭合的是固件发布顺序、具体 surface 动态绑定和各私有对象最后使用点，不能以 flags、LastMB、延时或 panic 填充值替代。全新地址且永久 pin 的多帧方案可避免 CPU 回收，但仍不能自行证明 INIT/context/私有参考状态允许下一作业。

## 本轮实现与验证

新候选在 [a1599_consumer_observation_candidate_20261014](../a1599_consumer_observation_candidate_20261014/README.md)，保留原框架，只新增 `a1599_fw_c0_ack.h`、harness 接线与离线测试。生产文件差异见 [CHANGE.patch](../a1599_consumer_observation_candidate_20261014/CHANGE.patch)。

- 默认关闭 `allow_c0_consumer_observation`。新增 `c0_consumer_once` 接受一次 `1`，必须已有有效 CPU 输出快照、decode gate 已用、资源已 pin；同 mutex 保护环事务。输出像素比较仍由外部审核完成，内核不伪称比较过。
- 两次核对六个固定寄存器，中间核对原 12-byte payload；期望 descriptors=`60cc01cc/60000033`、H2M=10/10、M2H=0/3、fence=1/class10。前置条件变化不写。
- **最多 21 次 32-bit MMIO 读、唯一一次写 `0x2fe8=3`**。写前用 `mb()` 排序已完成的 record reads/copy；它不充当 DMA 完成判据。写后六项 readback 必须得到 M2H=3/3 且其他字段相同才报告 verified。
- 独立记录 attempted、write_issued、verified、after_valid_count；写后失败不能被误报成“没有写”，未读字段不能被当作完整采样。终止观察一旦开始即不重试；授权/快照 gate 未满足时零 MMIO。
- 不清 decode_once gate，不增加第二个 ID81，不释放/复用缓冲，不增加 IRQ clear/kick/reset。

**本轮最终复核：27 条回归编译/执行命令，0 失败；ARM64 模块构建 exit=0；408+2 个原始指令回放通过；118 个基线文件原样保留。** 日志分别为候选 `build/HOST-REGRESSIONS.log`、[MODULE-BUILD.log](evidence/MODULE-BUILD.log)、[RECEIVER-REPLAY.log](evidence/RECEIVER-REPLAY.log)。没有重复 resource_probe 的旧 16 项测试。

`ACK-BASELINE-RED.log` 是对原 1211 receiver 的预期失败刻画：它读到 C0 后 consumer 仍为 0，因此新增的 consumer==3 断言失败；这不是基线 bug，也不是未解决的本轮回归。`ACK-RED.log` 是更早的缺功能测试脚手架，不能用来声称真实基线产生了该失败；两者明确区分保留。

## 最小下一次观察提案——尚未授权执行

本候选是可审查的源码和模块；**尚未生成带新会话绑定的完整 RAM 启动/传输执行包，不能直接套用旧脚本运行。** 模块 SHA 和文件冻结清单见候选 README、VERIFICATION.json 和 SHA256SUMS。

若下一次获批，范围建议收窄为：在另行批准的新 RAM 会话中，沿现有经过修复的传输框架加载冻结候选一次；原 fixture、一次 INIT、一次 ID81；导出并在主机比较像素通过后，显式触发一次 consumer 观察。只增加上面的最多 21 读/1 写；正常结果为 producer/consumer=3/3，所有 decode 资源保持 pin。任何 gate、读回或像素失败即停止，不重试、不提交第二帧、不扩大采证，不自动卸载、reset 或 reboot。

新风险是交还环槽后 firmware 可再使用这些槽、产生新消息或暴露迟到事件；现有双重检查不能冻结 firmware。3/3 只说明瞬时索引状态，不能证明未来无写。错误路径保留内存和映射，不能以 consumer 写成功解锁回收。

当前成功 RAM 会话不在本轮结束，不热替换 1211。后续完整包应冻结启动材料、脚本、module、firmware、fixture/reference 和明确动作上限，再取得相应执行批准。三帧连续 IDR 仍受上述生命周期缺口约束；本轮没有把“准备下一试验”扩成执行授权。
