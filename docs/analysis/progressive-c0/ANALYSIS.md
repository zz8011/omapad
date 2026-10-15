# 修正候选 C0 0x600644d5：有边界的离线解释

## 核心结论

**不能把本次 `ret=-121 / nonzero-no-detail` 直接认定为 Apple 定义的硬解错误。** 原始 Apple C0 分支对给定 status `0x600644d5` 走分派值 **0x10**，不读 detail；候选则把任何非零 status 都转换为 `-EREMOTEIO`，并要求 status==0 才读取 guard。两者的判定依据不同。

这不是解码成功结论。没有 guard、稳定输出或像素证据，也未证明 bit29/30 的完整 firmware 语义。现有证据能缩小的是 **“为什么候选返回 -121 且不检查输出”**：这是已定位的候选宿主策略；不能据此继续给固件错误分类或推断 panic 根因。

本次没有 USB、设备节点、模块、MMIO、额外 ring/output 读取或新试验。只读取已保存文件并在用户态重放原始消息分支。冻结新旧候选未修改。

## 证据层级

已读报告：`/home/zz8011/Documents/Codex/2026-10-13/task-6/evidence/a1599-decode-trial-20261014/PROGRESSIVE-ONE-SHOT-RESULT-20261014.md`。

随后已找到并读取同一 evidence 目录补交的 `PROGRESSIVE-ONE-SHOT-TOOL-OUTPUT-TRANSCRIPT-20261014.md`，SHA-256=`25c493d2aecce51fea6dfb43b6b258e451e8bc77792a58e1a3326abf29f08838`。它明确说明：从此前已返回工具输出整理，ACM0原始查询输出当时没有独立落盘。因此这是**执行者补交的工具输出转录，不是独立原始ACM日志**。本目录 `C0-TOOL-OUTPUT-TRANSCRIPTION.bin` 从其 C0_RAW 行提取；与先前 `C0-REPORT-TRANSCRIPTION.bin` 的12个字节一致。

补交转录提供了可核对的内核时间与字段：319.813471 的 `C0_CAPTURE` 声明 actual_length=12、raw_length=12、response_received=1、record_valid=1、message_read/write=0/3、last_mb_present=1、flags_present=1、detail_present=0、parsed_detail_present=0、guard_checked=0；319.855524 的 `C0_RAW` 行包含下列12bytes。319.866477 的 `DECODE_ONE_SHOT` 行记录 ret=-121、response_ring_consumer_advanced=0、output_snapshot_ready=0。代码与字段的边界核对通过。

文档校验问题：此补交转录头部把模块SHA写成61位 `4367df89658bdbcee535129f768180c2fc239baf1eaae87879a2061687b4c`，比已验证64位SHA少了 `960`。主试验报告和本地模块使用的是完整SHA `4367df89658bdbcee535129f768180c2fc239baf1eaae87896079a2061687b4c`。这是转录头部需要修正的问题，不能推断成设备上模块改变。此分析未修改原证据文件。

未用旧 diagnostic/M2H 日志替代本次日志，也未重新访问设备。搜索与补交链的文件可见性不作为硬件事实。独立原始会话记录仍缺失，但已保存的工具输出转录足以作为本报告明确标注来源的输入。

## 报告转录的字段边界

```text
8c c0 01 00 03 00 00 00 d5 44 06 60
```

| 字段 | 值 | presence 与限制 |
|---|---|---|
| byte0 | 0x8c | high bit=1；长度由低7位取得，12 bytes；high bit不命名 |
| byte1 | 0xc0 | C0 dispatch ID |
| +2 u16 LE | 1 | fence，两个字节存在 |
| +4 u16 LE | 3 | 原始分支的16位字段；虽 log称 Last MB，不当作已验证有效索引 |
| +6/+7 | 00 00 | 字节存在；所核对 C0 分支不读取其语义 |
| +8 u32 LE | 0x600644d5 | 完整存在 |
| +12 u32 | 不存在 | 不得用日志里默认打印的 detail=0 代替“存在且值0” |

`ceil((0x8c & 0x7f)/4)=3`，与报告中 M2H 0/3 一致。在那个 snapshot 时刻，3 words 可全部由该 C0 解释；没有多余 words 支持另一个 C7。不能推出之后永远没有 C7，也不能把固定32-byte接口失败说成“C0没有捕获”：按报告，C0已由此前响应路径捕获，32-byte接口是另一条受限接口。

相对旧 status `0x48064455`，XOR=`0x28000080`：

| status bit | 旧 | 新 | 已知解释 |
|---|---|---|---|
| 7 | 0 | 1 | 发生变化；语义未闭合 |
| 27 | 1 | 0 | 原始 C0 的分派/额外 detail 读取条件改变 |
| 29 | 0 | 1 | 发生变化；本分派分支不检验它，语义未闭合 |
| 30 | 1 | 1 | **没有变化**；本分派分支不检验它，语义未闭合 |

不能只分析高位而遗漏 bit7；不能给 bit29/30 臆造成功、错误或硬件阶段名称。

## Apple 12.5.8 的实际指令

kernelcache SHA-256=`76269e79b4715783b95407acbb54df2369116c76709a03f24e0fe95924199eb9`。本文是12.5.8 VA；旧契约摘要的12.5.7代码地址少0x100。以下文件偏移来自实际文件，代码 VA−`0xfffffff007004000`。

| 完整 VA / file offset | 数据流 |
|---|---|
| fffffff0086816c0..16e0 / 0x167d6c0..d6e0 | 对 byte0所在word AND 0x7f，加3，再右移2，确定消息 word 数 |
| fffffff00868190c / 0x167d90c | dispatch table首项 signed offset=-0x158，C0目标为0x86817b4；已检查原始表字节 |
| fffffff0086817b4 / 0x167d7b4 | `ldrh w10,[x20,#0x14]`，storage有16-byte宿主prefix，故对应message+4 |
| fffffff0086817b8 / 0x167d7b8 | `ldrh w8,[x20,#0x12]` → message+2 fence |
| fffffff0086817bc / 0x167d7bc | `ldr w9,[x20,#0x18]` → message+8 flags/status |
| fffffff0086817c0..17c8 / 0x167d7c0..d7c8 | 增加C0计数并保存 fence |
| fffffff0086817d0 / 0x167d7d0 | `tbnz w9,#27,0x8681828`，此分支只检验 bit27 |
| fffffff0086817d8..17e4 / 0x167d7d8..d7e4 | 本次 bit27=0：设w9=0x10、写分派数组及宿主record+0xc，跳公共尾部 |
| fffffff008681828 / 0x167d828 | 仅bit27=1路径 `ldr w11,[x20,#0x1c]`，即message+12 detail |
| fffffff008681830..1838 / 0x167d830..d838 | 该另一条路径写分派值0x8010 |
| fffffff00868185c / 0x167d85c | 公共路径计数递增，之后返回接收循环 |

bit27=1路径另调用原始格式串（VA fffffff007366f1f / file0x362f1f）：`MSVDXISRHandler Core[%d]: ui32Fence %d VDEBCR %x Flags %x Last MB %d %x - errorFlags %d`。其 errorFlags参数在本分支写1；这个事实仍没有给 bit29/30 命名。

## 原始基本块重放

`replay_c0_branch.c` 将原始代码副本映射到低用户地址，执行 **0x86817b4..0x8681860**。在0x8681864/1868设置恢复返回地址及RET边界，原始日志函数替换为空函数；未执行 MMIO receiver loop。12-byte输入放在可访问内存末尾，后面紧邻不可访问 guard page，保证若误读 +12 将不能静默得到零。

7组状态实际运行结果见 `REPLAY-RESULT.log`：

| status | 原始分派 |
|---|---|
| 600644d5（本次转录） | 10 |
| 400644d5（清bit29） | 10 |
| 200644d5（清bit30） | 10 |
| 000644d5（两位都清） | 10 |
| 60064455（清bit7） | 10 |
| 00000000（控制） | 10 |
| 48064455（bit27置位、提供16byte控制） | 8010 |

测试退出0；12-byte路径均未触碰 guard page。此重放验证的是**当前 C0 分派行为及字段访问边界**，不是 firmware 对这些 flags 的完整含义。

## 候选为何返回 -121 且没有输出证据

`a1599_fw_decode_submit.h:104–111` 在合法 C0 已解析后执行：

```c
result->response_error = result->response.status != 0;
return result->response_error ? -EREMOTEIO : 0;
```

同目录 `a1599_fw_first_ready_platform_harness.c:804` 附近另以 `response.status == 0` 才启动 guard 检查；之后输出快照还要求 `!ret`、guard intact、allow_output_export。因此本次 guard_checked=0 与 output_snapshot_ready=0 是现有门控可以完整解释的结果，并非 guard损坏或没有图像的观测。

底层 `id81_contract.c:c0_parse()` 本来已经按 bit27 区分 `C0_DISPATCH_10` / `C0_DISPATCH_8010`；更上层又把两者中任何非零status统一当作response_error。需要评审的是这个附加规则，不是把12-byte消息补成16bytes，也不是重新扩大ring读取。

## 一个有依据的下一步

**下一项离线开发应先分离“C0原始分派类别”和“输出已验证成功”两个判断，审查/测试 `status != 0 → -121` 以及关联guard门，而不是继续给bit29/30命名或立刻重试硬件。** 以这里原始指令结果作为回归依据：此12-byte消息应被识别为合法、无detail、class0x10；其解码/输出状态仍为“未验证”。不能只删除错误门后把它标成成功，更不能因此自行扩大输出读取权限。任何未来 bounded guard/output验证仍需独立明确授权，本次不执行。
