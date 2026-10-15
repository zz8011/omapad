# A1599：H+4 独立 parser → emitter 核验

2026-10-14，离线读取与静态分析；未操作 USB、设备节点、模块、重启或硬件。原始证据和冻结目录未修改。

## 结论

**冻结候选中的 H+4=`0x00001004` 与该 fixture 的 Apple 原始路径不符；应为 `0x00001008`。**

错误不在 `SliceQPY=20` 或 `disable_deblocking_filter_idc=1`。错误是把 `C+0x534`（十进制 1332）的值取成了 1。progressive、非 MBAFF 的完整私有字段链给出 **2**。原厂写 1 的路径需要 `P.half6 bit0=1`，此 fixture 不满足。

同一个字段还进入 `C+0x55c`，形成下列三个已确定的命令差异：

| 544-byte 命令中的 byte offset / word index | 字段 | 冻结候选 | Apple 原始数据流推导 | XOR |
|---|---|---|---|---|
| `0x004` / 1 | H+4 | `0x00001004` | **`0x00001008`** | `0x0000000c` |
| `0x0c8` / 50 | FE `0x0a0c` | `0x10040000` | **`0x10080000`** | `0x000c0000` |
| `0x0f0` / 60 | RENDEC `0x0ae4` | `0x10040000` | **`0x10080000`** | `0x000c0000` |

QP 所在命令 `0x0cc` / 51、`0x0f4` / 61 均为 `0x00005000`，与本次独立链一致。

这是三个确定的不匹配位置，**不是已验证完整修正流，不证明 HW_PANIC 的唯一原因，也不代表硬解成功**。未根据 `bdef7bdf` 猜测 MMU 错误；未把 panic/C0 的 LastMB 当成有效宏块索引。

## 输入与地址基准

- 样本：`evidence/a1599-usb-reenum-20261013/firmware-12.5.8-16H88/kernelcache.release.ipad4bm.decompressed`，27,192,152 bytes，SHA-256 `76269e79b4715783b95407acbb54df2369116c76709a03f24e0fe95924199eb9`。
- 冻结模块：`a1599_first_decode_candidate_m2h_snapshot_20261014/build/a1599_fw_candidate.ko`，本次复核 SHA-256 `c97fb883bbe39269bb9f16a9074305ce870e3366f563c72bf4b5ba77d6745fb2`。
- fixture：57 bytes，SHA-256 `839967e0bd3c887029c3a8517f3f5ed0aea57b0a567ac9ac700a85f92458af31`。直接读取二进制重新解析 SPS/PPS/slice；没有调用已有 serializer 或 builder 作为 oracle。
- 本文 **全部代码 VA 为 12.5.8 的 unslid VA**；不是旧摘要的 12.5.7 VA。Mach-O `__TEXT_EXEC` 的 VA=`0xfffffff0075b0000`、fileoff=`0x5ac000`，故本文代码 file offset=`VA−0xfffffff007004000`。表所在 `__DATA` 同样成立。不要把此公式扩展到所有 segment。
- 原始日志 `DIAGNOSTIC-TRIAL-HOST-SESSION.log` 实际含较早的 09:22–09:40 diagnostic trial、129ms ready；其中有完整命令 dump。**它不是 11:02–11:10 的 131ms M2H 试验日志**。本报告用它核对命令 byte offset，并用 hash 匹配的冻结源码核对三个常量；最新实机状态仍以 `M2H-ONE-SHOT-RESULT-20261014.md` 为准。
- 已读该结果文件：131ms first-ready、INIT transport consumed、一次 ID81、C0 `0x48064455`、C7 原始记录、无图像。没有回退到旧 overlay 阶段。

## 对象命名与桥接

`D` 是 decoder/parser 外层对象；`R` 是原始 slice 结构；`Q` 是原始 PPS；`SPS` 是原始 SPS；`C=*(D+0x30f8)` 是 producer context；`P=*(D+0x28)=*(C+0x448)` 是紧凑 picture 参数；`T=*(D+0x30)=*(C+0x450)` 是紧凑 slice 参数；`H=*(C+0x15d0)` 是命令中的 36-byte header。

这些是指针，尤其 `P` **不是**嵌入在 `C+1096` 的结构。

桥接 `0xfffffff008692dc4`（file `0x168edc4`）：

- `0x8692df0` 读 `R=*(D+0x1c8)`。
- `0x8692df4–0x8692e08` 把 `D+0x28` 的 P 指针和 `D+0x30/+0x38` 的指针复制到 `C+0x448/+0x450/+0x458`；`x23=T`。
- parser table → R：`0x8684044/0x8684048` 把 slice 指针表传入 parser `0x868751c`；parser 在 `0x868754c` 取 `table[index]`。外层 `0x868e540–0x868e548` 从 parser 对象 `+0x920+8*index` 取该结构，`0x868e5b8` 写 `D+0x1c8`。
- `0x8692f64–0x8692f6c` 调 producer `0x869726c`；其 `0x8697290–0x8697294` 取 `C` 和 `T`。之后 `0x869767c–0x8697684` 以 `(C,T)` 调 main emitter `0x869885c`；该 emitter `x19=C,x20=T`。

## 1. QP 与 deblock 的 parser 写入链

以下缩写 VA `0x868xxxx` 均补全为 `0xfffffff00868xxxx`；每个关键点同时给 file offset。

| 原始指令位置（完整 VA / file offset） | 数据流与 fixture 值 |
|---|---|
| `0xfffffff008685bc0` / `0x1681bc0` | PPS 有符号 Exp-Golomb 结果写 `Q+0xa0=pic_init_qp_minus26=-3`；此前 `0x8685bb0–0x8685bbc` 是符号映射。 |
| `0xfffffff008685cc4` / `0x1681cc4` | `Q+0xac=deblocking_filter_control_present_flag=1`。 |
| `0xfffffff008685ce8` / `0x1681ce8` | `Q+0xb0=constrained_intra_pred_flag=0`。 |
| `0xfffffff008687570–0xfffffff00868757c` / `0x1683570–0x168357c` | 每次以 `memset(R,0,0xe18)` 清零原始 slice，覆盖 `+0x1c/+0x20/+0xe0c/+0xe10`。不是依赖初次分配或沿用上一帧。 |
| `0xfffffff008687b04` / `0x1683b04` | `slice_qp_delta=-3` 写 `R+0xdfc`；`0x8687af4–0x8687b00` 做有符号映射。 |
| `0xfffffff008687b08–0xfffffff008687b2c` / `0x1683b08–0x1683b2c` | 读 `Q+0xa0`，加 delta、加 26，得到 20 并做范围检查。此处尚未直接写最终 QP 命令。 |
| `0xfffffff008687bc4–0xfffffff008687c00` / `0x1683bc4–0x1683c00` | 由 `Q+0xac=1` 进入 ue(v) 读取；写 `R+0xe08=disable_deblocking_filter_idc=1`。 |
| `0xfffffff008687cd0–0xfffffff008687cd4` / `0x1683cd0–0x1683cd4` | `cmp w8,#1; b.eq 0x8687d80`，跳过 alpha/beta 读取和写入，故两字段保持刚才清零的 0。 |
| `0xfffffff008687d1c` / `0x1683d1c`；`0xfffffff008687d60` / `0x1683d60` | idc≠1 时才执行的 alpha/beta se(v) 写入 `R+0xe0c/+0xe10`。本 fixture 不执行。 |
| `0xfffffff008692e2c–0xfffffff008692e38` / `0x168ee2c–0x168ee38` | `R+0xe0c/+0xe10` → `T.byte0x1b/0x1c`，均 0。 |
| `0xfffffff008692f30–0xfffffff008692f34` / `0x168ef30–0x168ef34` | `R+0xe08` → `T.byte0x363=1`。 |
| `0xfffffff008692f40–0xfffffff008692f44` / `0x168ef40–0x168ef44` | `R+0xdfc` → `T.byte0x35f=0xfd`。 |
| `0xfffffff0086932b8–0xfffffff0086932bc` / `0x168f2b8–0x168f2bc` | `Q+0xa0` → `P.byte0xa8=0xfd`。不要误用 `Q+0xa8`，后者是 chroma QP offset。 |
| `0xfffffff008698358–0xfffffff008698368` / `0x1694358–0x1694368` | 两次 `ldrsb` 正确符号扩展两个 0xfd，`-3 + -3 + 26 = 20` 写 `C+0x530`。 |
| `0xfffffff008698660–0xfffffff00869866c` / `0x1694660–0x169466c` | `(C+0x530 & 63)<<10` OR 到 `C+0x560`；本 fixture 为 `0x5000`。 |

QP 经 `0x8698804–0x8698810` 发至 FE `0x0a10`，并经 `0x8698938–0x8698940` 发至 RENDEC `0x0ae8`。因此 **H+4 并不直接装载 QP**。把 QP=20 再塞进 H+4 会引入另一处错误。

## 2. 真正的差异：progressive → 私有 picture mode

SPS parser 的 `0xfffffff008685654` / file `0x1681654` 写 `SPS+0x660=frame_mbs_only_flag=1`。紧接 `0x8685658–0x8685660` 在此值非零时置 `w21=0`，`0xfffffff008685684` / file `0x1681684` 写 `SPS+0x664=mb_adaptive_frame_field_flag=0`。

slice parser 在 `0xfffffff0086876b0–0xfffffff0086876b4` / file `0x16836b0–0x16836b4` 检查 `SPS+0x660`。值为 1 时不进入 `0x8687814` 的 field flag 读取；已清零的 `R+0x1c=field_pic_flag=0` 保持不变。`0x8687838` 是另一分支的 field flag 写入，本 fixture 不走。

picture 参数构造：

| 完整 VA / file offset | 变换 |
|---|---|
| `0xfffffff00869304c–0xfffffff008693058` / `0x168f04c–0x168f058` | `R+0x1c` 的 bit0 → `P.half6 bit0`，所以为 0。 |
| `0xfffffff00868c8b0–0xfffffff00868c8d8` / `0x16888b0–0x16888d8` | `record[D+0x44*slot+0x2f80] = (R.field_pic_flag==0) && (SPS.mb_adaptive_frame_field_flag!=0)`，所以为 0。 |
| `0xfffffff00869305c–0xfffffff008693078` / `0x168f05c–0x168f078` | 上述 record byte → `P.half6 bit1`，所以为 0。 |

producer 的决定性分支（12.5.8 原始指令）：

```text
VA fffffff0086974e8 / file 016934e8  ldrb w11,[x9]        ; x9=P+6
VA fffffff0086974ec / file 016934ec  tbnz w11,#0,0x869750c
VA fffffff0086974f0 / file 016934f0  mov w11,#1
VA fffffff0086974f4 / file 016934f4  str w11,[x19,#0x538] ; C+0x538=1
VA fffffff0086974f8 / file 016934f8  ldrb w11,[x9]
VA fffffff0086974fc / file 016934fc  tst w11,#2
VA fffffff008697500 / file 01693500  mov w11,#2
VA fffffff008697504 / file 01693504  cinc w11,w11,ne       ; bit1=0，不递增
VA fffffff008697508 / file 01693508  b 0x8697880
VA fffffff008697880 / file 01693880  str w11,[x19,#0x534] ; C+0x534=2
```

`0x8697874–0x869787c` 那条 `C+0x538=3,w11=1` 路径，是从 `0x869750c/0x8697510` 在 `P.half6 bit0=1` 且 `P.byte4 bit7=1` 时才进入。不能为 progressive I/IDR 直接套用。I/IDR 并不等同于该分支条件。

本次 `replay_picture_mode.c` 从 kernelcache 读取以上真实指令，以 Capstone 解码并解释有限指令集；模拟 P/C 内存中，`P6.low=0x10`（4:2:0 bit4=1，相关 bit0/bit1=0），分别令 P4=0 和 128，均得到 `C534=2,C538=1`。另用 P6=0x12、0x11 走三条反事实分支核对：得到 3、0、1。这不是自家 serializer/builder 的交叉比较，也不是执行整个 Apple driver。

## 3. H+4 的全部输入及最终 store

P.half6 的高相关位由 PPS 填充：`0x86930ac–0x86930c8` 把 `Q+0x98=weighted_pred_flag=0` 放 bit8；`0x86930cc–0x86930e8` 把 `Q+0x9c=weighted_bipred_idc=0` 放 bits10:9；`0x86930ec–0x8693108` 把 `Q+0xb0=constrained_intra_pred_flag=0` 放 bit7。PPS 的这三个 parser store 分别为 `0x8685b50`、`0x8685b74`、`0x8685ce8`。

slice_type ue(v)=7 在 `0x86875f8` 初存，`0x868760c–0x8687618` 归一化成 2，`0x8692e0c–0x8692e10` 复制到 `T.byte0x16`。表 VA `0xfffffff00886f9b4` / file `0x186b9b4` 的前三字节实读 `01 02 00`，故 I 的映射值为 0。

H+4 emitter 关键点：

| 完整 VA / file offset | 输入/运算 |
|---|---|
| `0xfffffff008699430–0xfffffff008699440` / `0x1695430–0x1695440` | P.half6 bits10:8 → H bits19:17，P bit7 → H bit23，均 0。 |
| `0xfffffff008699444` / `0x1695444`；`0xfffffff008699470` / `0x1695470` | `T.byte0x363=1` → `1<<12`。 |
| `0xfffffff008699474–0xfffffff008699488` / `0x1695474–0x1695488` | alpha/beta 低四位 → bits11:8 / bits7:4，均 0。 |
| `0xfffffff00869948c` / `0x169548c`；`0xfffffff0086994b8` / `0x16954b8` | `C+0x534=2` → `2<<2=8`。 |
| `0xfffffff0086994bc–0xfffffff0086994f8` / `0x16954bc–0x16954f8` | I slice 的表项 0，OR 到低两位。 |
| `0xfffffff0086994fc–0xfffffff008699504` / `0x16954fc–0x1695504` | store 到 `H+4`，并保存 `C+0x1650`。 |

因此 H+4 唯一值为 `0 | 0 | (1<<12) | 0 | 0 | (2<<2) | 0 = 0x1008`。**本次 H+4 的字段链已闭合，没有未识别的输入需要猜值。**

## 4. 同一 mode 错误的第二条传播链

`0x869836c–0x8698374` 从 `T+0x361` 建立 bit31，I slice 下原始 `R+0x3c` 由清零保持 0。`0x8698378–0x86983ac` 加 deblock_idc 到 bits29:28；`0x86983d8–0x869841c` 加 alpha/beta；**`0xfffffff008698444–0xfffffff008698478` / file `0x1694444–0x1694478` 把 C+0x534 左移 18 后 OR 到 C+0x55c**。I 的表项和 CAVLC 的 cabac_init 字段均为 0，first slice counter 为 0。

结果 `C+0x55c = (1<<28)|(2<<18) = 0x10080000`。

- FE emitter：`0xfffffff0086987f4–0xfffffff008698800` / file `0x16947f4–0x1694800`，读 C+0x55c 并发往 `0x04800a0c`。
- Main emitter：`0xfffffff00869892c–0xfffffff008698934` / file `0x169492c–0x1694934`，作为起始 RENDEC target `0x0ad8` 后的第四个 word 发出，因此目标为 `0x0ae4`。

冻结源码 `a1599_fw_decode_stream.c` 中恰有一处 `0x00001004` 和两处 `0x10040000`。旧契约摘要 §5.1 和 post-slice ledger 的对应数值应标记为被本报告推翻，不能继续当成独立 Apple 证据。

## 交付、复核和边界

- `apple-vxd-12.5.8.disasm.txt`：Capstone 5.0.9 对本地样本产生的带原始字节、VA、file offset 的反汇编，局部证据以本文函数范围为准；整个范围含数据，不把线性解码出的所有行都当代码。
- `disasm.c`：复现反汇编的小工具。
- `replay_picture_mode.c`、`picture-mode-replay.txt`：有限指令解释器及五条分支输出。
- `verify_h4.py`、`H4-VERIFICATION.json`：fixture 位解析、样本 hash、table bytes、分支结果及命令差异；JSON 给出各已解析字段的 bit 起止位置。
- `REVIEW-ONLY-three-word-correction.patch`：只展示三个已证明常量的差异，**未应用**到冻结目录、未生成或加载新模块。

复现命令（在本交付目录运行）：

```sh
gcc -Wall -Wextra -O2 replay_picture_mode.c -lcapstone -o replay_picture_mode
python3 verify_h4.py
```

本次复核 PASS：输入 hash、fixture 32-bit header、五条原始 picture-mode 分支、I-slice table bytes、三个命令位置与两个 QP 位置。没有重复旧 resource_probe 的 16 项测试；未安装软件。

剩余边界：这只证明三个 word 的静态差异。`C+0x538` 也从误选路径的 3 变为 1，后续应继续审查其消费者（例如 `0xfffffff0086971ac`）及整条 progressive command 路径，不能宣称仅替换三个常量就足够；本次没有复核全部 command words、DMA 实际可达性、输出布局或固件完成语义。panic 的因果确认仍未完成，且本次实机授权已用完。
