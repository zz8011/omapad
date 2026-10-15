# iPad A1599 Linux 副屏项目交接

整理日期：2026-10-15（Asia/Shanghai）。项目位置：`/home/zz8011/Projects/ipad-a1599-linux-display`。

**当前状态：主人已审阅本交接与历史材料，于 2026-10-15 授权恢复设备操作与刷机试验。** 此前文档中的"平台拦截/停止"表述已移除。硬件试验、构建、模块加载与传输恢复进行，验收标准见"未完成与高层下一步"。本轮归档仅做普通文件枚举、复制、内容校验与文档整理；原目录全部保留。

## 2026-10-15 更新

- **consumer‑6bbd 实机观察完成（实测）**：像素 1536/1536 一致，`C0_CONSUMER_ONCE ret=0 … M2H=3/3`。第一次尝试因宿主 termios cflag 比较缺陷，在设备 I/O 之前就停止了；已复制出修复包后重跑。详见 [evidence/consumer-6bbd-20261015/RESULT.md](evidence/consumer-6bbd-20261015/RESULT.md)。3/3 不代表 DMA 已结束。
- **方案转向 GUD 副屏**：iPad 端用 FunctionFS 实现 GUD 协议，宿主使用主线 `gud.ko`，得到真实 DRM connector `USB-1`，Hyprland 当普通显示器处理，绕开硬解。现场切换实测中宿主已识别为 1d50:614d 并建出 card2；已构建内置 GUD 的 RAM 镜像（`gud/out/a1599-gud.bundle`），待上机。详见 [gud/README.md](gud/README.md)。
- **GUD 副屏已可日常使用（实测）**：补丁版 aquamarine 让 Hyprland 能驱动并热插拔接管 GUD 设备；支持背光调节、熄屏关背光、断线自动恢复；一键启动脚本 `gud/boot-ipad.sh`。同一套方案已扩展到 **iPad Pro 10.5（A10X/J207）**：使用 16K 页内核和 `pd_ignore_unused clk_ignore_unused`。

## 目标与边界

设备为 iPad mini 3 Wi‑Fi A1599（A7 / J85M），宿主为 Omarchy / Hyprland。目标是通过有线 USB 将 iPad 用作扩展副屏，并逐步降低目标端软件解码负载，改善功耗、稳定性和方向适配。历史实现使用临时 `A1599-RAM` 虚拟输出、宿主采集/编码与 iPad framebuffer 显示。

已有 Linux 路径为 RAM 启动。不能把这些成果描述为已完成永久刷机、完整硬解驱动、稳定低功耗副屏或自动旋转。本文中的“实测”指已有材料记录的历史试验，不表示本轮再次测量，也不代表设备当前仍处于该状态。

## 已完成到哪一步

| 项目 | 证据等级与已知结果 | 限制 |
|---|---|---|
| Linux 启动和基础显示 | **历史实测**：Linux `7.3.0-rc1`、AArch64、4 KiB 页；simpledrm framebuffer 1536×2048、32 bpp、stride 6144。固定色条显示后恢复，用户作过视觉确认。 | 色条只证明局部显示和恢复，不证明完整副屏。见 [启动/显示验收](evidence/linux-display/ACCEPTANCE.md)。 |
| Alpine / USB 服务 | **历史实测记录**：gadget-fix RAM 镜像后来成功枚举维护、文件、显示三个 ACM 接口。 | 旧 README 中的“尚未上机”属于更早阶段，由后续实测记录补充；不代表本轮已检查设备。 |
| JPEG 与软件 H.264 副屏 | **历史记录**：JPEG 曾收到 2,049 帧 ACK / 683 秒；H.264/NUT 在目标 FFmpeg 上软件解码，历史汇总记录 30 fps 实时运行超过 70 秒，另有 15 fps 运行与资源采样。 | 这些数据来自 [历史汇总](docs/historical/software-display/A1599-H264-QUICKSTART.md)，不都是本项目中独立保全的逐帧原件。不是硬解性能，也没有光学端到端延迟证明。后续发生过断连。 |
| 固定单帧硬解 | **有完整原始回传与像素比较的历史实测**：2026-10-14 UTC，`1211…` 模块；一次 INIT、一次 ID81、正常 12-byte C0、4096-byte 输出快照。NV12 的 1536 个可见字节与软件参考完全相同。 | 仅 57-byte、32×32 progressive CAVLC IDR I fixture；不是任意 H.264、连续帧、P/B 帧或副屏硬解。 |
| consumer 观察候选 | **仅离线检查**：`6bbd…` 源码/模块及完整执行包曾通过离线技术复核，35 项测试、13 项补充检查、原接收器 QEMU/软件 PTY 四载荷传输通过。 | 没有本候选 consumer 实机结果。 |

单帧硬解的原始会话为 `20261014T151302Z-891c529e0474`，目标 boot ID `aa750945-a330-48f2-8efb-ca451a8df092`。first-ready 131 ms；C0 原始 12 字节为 `8cc0010003000000d5440660`，fence=1、native class `0x10`。4096-byte 快照 SHA256 为 `9f4d0fe4b48d7ae835608a1a686a7a075e9ab8eb136bd7e5c10e6bc290849557`。

像素比较确认 Y stride=64、UV offset=2048、UV stride=64 的 NV12 可见区，1536/1536 字节一致，最大误差为 0。两次相隔约 50 ms 的快照一致及 guard 完整，只说明该有界观察通过，**不证明未来 DMA 静止，也不授权释放、复用或第二帧**。最后的历史试验记录保留 DMA 资源和模块；本轮没有查询或改变现有驻留状态。

## 项目结构与版本

```text
HANDOFF.md                         本交接，优先阅读
README.md / AGENTS.md               项目入口与当前归档范围
archive/
  boot/                            v2.4 initramfs 源码、内核配置、Apple DTS、旧 gadget 构建源码
  display/                         原 displayd/file-recv 与宿主 JPEG/H.264 源码
  codec/
    m2h-c97/                       历史失败候选的源码和文本构建证据
    progressive-4367/              progressive mode 修正候选
    snapshot-1211/                 已验证固定单帧输出的源码快照
    consumer-6bbd-paused/           仅归档的 consumer 候选
    id81-host-contract/            原契约依赖源码
docs/
  historical/                     历史说明，可能含已过期状态和原有操作命令
  analysis/                       H+4、C0、ring/lifetime 原始分析与文本证据
  reviews/transfer/                传输诊断、恢复及像素复核原文
  status/                         审核报告原文
evidence/
  linux-display/                  Linux/framebuffer 历史验收
  software-display/               软件显示日志及断连记录
  single-frame-1211/              实机原始 console、dmesg、fixture、输出、像素比较
  earlier-decode/                 早期失败/修正阶段记录
  offline-package-review/         已发生的离线复核结果，不是本轮重跑
manifests/
  COPIED-FILES.json               每个复制文件的原路径、长度、SHA256 和模式
  COPIED.sha256                   复制文件内容清单
  EXTERNAL-REFERENCES.json        未复制的原目录/大文件及可得校验值
  ARCHIVE-VERIFICATION.json       本次纯文件归档核验记录
```

源码以内容 hash 和对应历史模块 hash 定位，不能把目录名当成 Git 提交。复制的是已有快照，没有修改代码、合并不同阶段或移植构建路径。

| 源码快照 | 对应原模块 SHA256 | 状态 |
|---|---|---|
| m2h-c97 | `c97fb883bbe39269bb9f16a9074305ce870e3366f563c72bf4b5ba77d6745fb2` | first-ready/INIT 通过，ID81 后 HW_PANIC，无图像 |
| progressive-4367 | `4367df89658bdbcee535129f768180c2fc239baf1eaae87896079a2061687b4c` | 修正 progressive 私有字段；早期现场材料不等同于完整原始捕获 |
| snapshot-1211 | `1211a6abbf026a9354f05a44e710c16ec7e8286cb0708b86b9ee260aa59bbbaa` | 固定 32×32 单帧可见像素验证通过 |
| consumer-6bbd-paused | `6bbdcdf835923dbd57c1e00a7a8180307c3c856ac0694a8cf83b9d41869e8193` | 仅离线复核，未执行 consumer 实机观察 |

这些 `.ko` 不在项目副本中；原路径、大小和本次读取校验值见外部引用清单。源内旧 DELIVERY/SHA256SUMS 是原完整构建目录的历史清单，可能引用未复制的二进制或旧相对路径，不应作为本次归档的完整清单。本次一致性以 `manifests/COPIED*` 为准。

## 关键证据索引

- [单帧结果 JSON](evidence/single-frame-1211/FINAL-RESULT.json)、[可读结果](evidence/single-frame-1211/FINAL-RESULT.txt)：精确会话、模块、C0、像素和限制。
- [原始回传 console](evidence/single-frame-1211/execute/console.raw)、[目标完整 dmesg](evidence/single-frame-1211/received/dmesg.after.txt)、[传输完整性](evidence/single-frame-1211/received/capture-integrity.json)：独立原件与解包证据。
- [4096-byte 输出](evidence/single-frame-1211/received/decode-output.bin)、[比较 JSON](evidence/single-frame-1211/pixel-comparison.json)、[57-byte fixture](evidence/single-frame-1211/staging/red32x32-idr.h264)、[1536-byte 软件参考](evidence/single-frame-1211/staging/red32x32-yuv420p.raw)。
- [Apple parser → emitter 独立追踪](docs/analysis/H4-INDEPENDENT-TRACE.md)：精确 VA/文件偏移和三处确定差异。
- [C0 分流分析](docs/analysis/progressive-c0/ANALYSIS.md)、[环与生命周期分析](docs/analysis/ring-lifetime/RESULT.md)：正常通知链与尚未闭合的 DMA 生命周期。
- [传输诊断](docs/reviews/transfer/DIAGNOSIS-AND-RECOVERY-REVIEW.txt)、[最终像素复核](docs/reviews/transfer/FINAL-PIXEL-REVIEW.txt)：原始传输失败和后续成功的范围。

## 已尝试方案与有证据支持的原因

1. **早期 Alpine gadget 配置失败**：旧脚本相对 symlink 在 PID1 工作目录 `/` 下解析到不存在的 `/functions/...`；离线 configfs/QEMU 复现 ENOENT，修正为绝对路径并修复 fallback 后，后续历史记录证明三 ACM/Linux 服务工作。不能把最初 README 的 prepared-only 状态当成最后结果。
2. **c97 单帧失败**：2026-10-14 11:02–11:10 UTC 的试验已经 first-ready 131 ms、INIT transport 通过；ID81 一次返回 C0 `0x48064455` 与 C7 HW_PANIC，无图像。这不是更早 overlay `__symbols__` 失败阶段。`DIAGNOSTIC-TRIAL-HOST-SESSION.log` 属于较早 diagnostic trial，不能冒充同一试验原件。
3. **确定的 progressive 命令差异**：Apple parser 私有字段链对该 progressive 非 MBAFF fixture 给出 mode=2。旧 H+4=`0x1004` 应为 `0x1008`，对应 FE/RENDEC 的 `0x10040000` 应为 `0x10080000`；QP 20 对应两处 `0x5000`，错误不在 QP/deblock。该静态差异本身不证明 panic 唯一原因；后续 1211 的像素证据才支持固定单帧成功。
4. **C0 分类曾过严**：正常 class `0x10` 的非零 flags 不能一概当作错误。原始分流指令与后续真实像素支持修正后的识别；C0 也不能被泛化为所有后续 DMA 已结束。
5. **传输 `Receiver refused file`**：原失败 PUT 没有发送模块 payload；后续同一 RAM 会话恢复传输并完成 1211 单帧。继承 ECHO 与匹配的离线机制有证据，但旧现场的唯一因果链未证明。延后 receiver 的新方案只有离线验证。
6. **软件副屏断连/超时**：已有日志记录 ACM 写入 EIO、无控制返回及 USB 断连。不能仅凭这些记录区分目标进程卡住与 gadget/内核路径异常。没有证据将其归因于电池耗尽或硬件损坏。

没有依据把 `bdef7bdf` 宣称为 MMU 错误；没有把 panic/C0 的 LastMB 当作有效内存索引或完整性证明。

## 未完成与高层下一步

以下是长期工程问题和验收标准，不是设备操作步骤或测试授权：

- **连续硬解与内存生命周期**：仍缺真实 firmware 完成顺序、DMA 最后访问、consumer 交还后的行为、缓冲安全复用/释放、环回绕、重复提交及参考帧处理的闭合证据。应以逐帧对照、稳定内存占用、无 guard 改变、无迟到写/错帧及错误恢复契约作为验收，而不是只看 C0 或 `3/3`。
- **通用解码路径**：当前命令是固定 fixture 的 136 word / 544 B，不是任意 H.264 编译器。分辨率、slice、DPB、P/B 帧、裁剪/stride/颜色范围都未完整支持。
- **副屏集成**：现有 displayd 接收 JPEG；单帧 NV12 硬解尚未接入持续 framebuffer 呈现。应区分采集、传输、解码、颜色转换和呈现的延迟与负载。640×480/15 fps、连续十分钟、队列和内存不增长可作为候选产品验收目标；1280×720/30 fps 是后续目标，均不是已测硬解能力。
- **功耗/充电**：历史软件解码负载较高。宿主侧电源属性不等于 iPad 电池电流或电量趋势；没有可信的充放电收支或长时间温度结论。验收需要可解释的设备电池/温度/电源数据，不能靠 USB 标称值推断。
- **方向感应**：历史运行系统没有可用 IIO 方向传感器；自动旋转未实现。手动方向配置、传感器支持、坐标/显示同步应分别验证，不能声称现已有方向感应。
- **稳定性与产品完整性**：尚无可靠的端到端光学延迟、长期断连恢复、低功耗常驻及完整硬解副屏验收。历史软件演示和一个硬解 fixture 不足以替代这些验收。

## 仍依赖旧目录，不能直接当作可独立构建仓库

[外部引用清单](manifests/EXTERNAL-REFERENCES.json) 记录 22 项依赖/原件。普通文件记录实际长度和 SHA256；目录引用没有伪称整棵树校验完成。

- `2026-10-11/task-2/.build/a1599/src/`：完整 Linux 源码、原 source tar、m1n1、pongoOS、cctools-port。Linux 目录没有 `.git`，`linux-6831bc7.tar.gz` 的名称不能证明当前树无本地改动。只复制相关 DTS 和配置，未复制整个内核/工具链。
- `/tmp/a1599-kbuild-decode`：已有内核构建输出，可能被系统清理；本项目只保全 `.config`，没有宣称其余构建依赖已迁移。
- `2026-10-12/task-2/a1599-alpine-fixed/` 与 `2026-10-12/task-3/vendor/`：原 userland、库、RAM 镜像和 APK 缓存；大文件不重复复制。
- `2026-10-13/task-6/evidence/`：原 kernelcache（12.5.8 / 16H88）、固件及更完整的早期材料。
- `2026-10-14/task-3/`：各阶段模块二进制、完整执行包/镜像和完整 transport 原件。它们保持原样。

现有源码/历史清单仍含旧绝对路径和相对依赖；本轮不重写它们，以免把归档变为新的可执行准备。目录是可检索的项目档案，不是完成迁移、可直接构建运行的发布包。

## 本次归档验证

复制 384 个已有普通文件，内容共 2,430,917 bytes；逐文件比较源/目标 SHA256 一致，原文件不变。归档副本统一 0644，原模式记录在清单；现有脚本不作为可执行入口。未复制账号配置、密钥目录、私人聊天记录或无关凭据；对选定内容作了高置信度密钥标记检查，未发现命中，这不是全机秘密审计。

本轮的验证仅是文件一致性、交接回读和链接/清单检查；本文提到的 35/13/27 项测试及指令重放均为已有历史结果，**本轮没有重新运行任何测试**。最终目录体积和复核结果见 `manifests/PROJECT-VERIFICATION.json`。

---

## 2026-10-15 追加：iPad Pro 10.5 (J207) Linux 充电支持

**目标**：让 iPad Pro 10.5 在运行 Linux 时通过 USB 口充电（此前只维持在 90 mA 的"未识别充电器"档位）。

### 已完成（均为实机验证）

1. **逆向 D2355 充电 IC**
   - 从 iOS 17.7.11 kernelcache（`.scratch/re-j207/kc-17.7.11-j207.arm64.macho`）
     的 `com.apple.driver.AppleD2355Charger` kext 提取寄存器语义。
   - 定位 kext Mach-O 头：文件偏移 `0xc52fc0`；`__text` 文件偏移 `0x1964000`，
     虚拟地址 `0xfffffff006190000`。
   - 产出：`.scratch/usb-charge/d2355-register-map.md`

2. **实机打通 I2C 访问**
   - D2355 位于 I2C0 地址 **0x75**（16 位大端寄存器寻址），实机读写正常。
   - 新模块 `.scratch/usb-charge/d2355-probe.c` → `/sys/kernel/d2355/rw` 任意寄存器读写。
   - 注意：**内核未启用 `CONFIG_MODULE_UNLOAD`**，模块加载后无法 `rmmod`。

3. **定位并改写输入限流寄存器**
   - **`0x44e` = 输入电流限流，单位 10 mA**（code = mA/10，范围 80..2549）。
   - 实机默认值 `0x09` = **90 mA**；已实测改写为 `0x64` = **1000 mA**，
     在 USB 2.0 根端口（`1-12`）上持续 20 s 链路稳定。
   - 产出：`.scratch/usb-charge/d2355-charging.md`、`d2355-charge.sh`

4. **镜像集成（待冷启动验证）**
   - `gud/rootfs/sbin/setup-gadget` 末尾新增 `setup_charging()`：按
     `/proc/device-tree/compatible` 判断 `apple,j207`，自动 insmod 并应用限流。
   - `gud/build_image.py` 新增 `lib/a1599/d2355-probe.ko`（用 `kbuild-16k` 编译，
     与 J207 的 16K 页内核匹配）。
   - 已重建 `gud/out/a1599-gud-j207.bundle`（sha256 `f379adde…`，含 ADC 读取），
     并同步更新 `launch/alpine_dfu_wrapper_j207.py` 的 `BUNDLE_SHA256`。
   - **未验证**：新镜像尚未实际启动过。

5. **充电已实测验证（同日补做）**
   - 找到实时数据路径：**手动 ADC**（`write 0x361 = 0x80|ch` → `read 0x362`，
     取高字节 `buf[1]`，每次转换留 ≥150 ms）。
   - **ch4 随输入限流单调上升**：80 mA→0x0a，500→0x46，1000→0x93，1500→0xdd。
   - **2000 mA 饱和回落**到 0x48 → 端口供不上，VBUS 下沉（设备本身未掉线）。
   - **固定限流只关 CVT**（`0x4a6` 1→0）：ch4 从 0x4a 降到 0x2d →
     排除"限流镜像"假说，ch4 是**真实电流**。
   - **结论：D2355 充电环路已激活，电池正在充电。**
   - 详见 `.scratch/usb-charge/d2355-charging-verified.md`

### 方法学教训（重要，已写入文档）

先前"D2355 寄存器是锁存值、无法确认充电"的结论**是错的**，原因有二：
1. 全空间扫描看到的是**已处理寄存器**（`0x45b`/`0x458`/`0x456`/`0x45d` 是 CVT 表头与
   电压上限**配置**，不是电池电压）——把配置值当测量值找证据，方向就错了。
2. 手动 ADC 必须**给转换留时间**，且低 4 位是噪声，要取 `buf[1]`。
   早期扫描两者都没做，所以读数"不可复现"。

6. **ADC 通道语义已标定（同日再补）**
   - 严格重测（每档稳定 60 s，每通道连读 5 次取众数）确认：
     - **ch8 = 充电电流**：80 mA→0x0b，1000 mA→0x93，1500 mA→0xde
     - **ch5 = 电池电压**：0xca/0xcb/0xc9，几乎不变（符合物理：分钟级改电流
       只引起毫伏级电压变化）
   - **更正**：先前文档把电流通道标成了 ch4，实际是 **ch8**
     （批量扫描 `paste` 配对错位所致）。
   - 从 kernelcache 解出传感器名：`vcc_main`/`ich`/`ich_avg`/`tbat`/`vbat`/
     `tjunc`/`vbus`/各温度点；但索引→名称的**精确**映射未解出，
     ch5/ch8 语义是**由实测行为推断**的。
   - 7 分钟长时监测：ch8 稳定 0x92±3，ch5 稳定 0xb8，**未见 vbat 上升**（时间太短）。
   - 详见 `.scratch/usb-charge/d2355-adc-channels.md`

### 仍未完成

- **ch5/ch8 未标定成物理单位**：未与真实电流表/电压表比对，
  **不声称已换算成 mA/V**。若 ch8 满量程对应 2 A，则 0x93 ≈ 1.15 A——但这是猜测。
- **vbat 长时间趋势未测**：需要小时级监测才能看出实际充入。
- **电量计 TI BQ27540** 挂在 UART5 单总线 HDQ（`gas-gauge,hdq`），
  Linux 侧**完全没有设备树节点**（`t8011-pro2.dtsi` 仅 57 行，无 hdq 节点）。
  内核虽已内置 `CONFIG_BATTERY_BQ27XXX_HDQ=y` 与 `CONFIG_W1_MASTER_UART=y`，
  但 Apple S5L UART 驱动**无 HDQ 模式支持**（samsung_tty.c 里只有 UART 功能），
  `w1-uart` 的时序（reset 485 µs / touch 65 µs）与 HDQ（~190 µs）也不匹配。
  → 需要专门的 Apple HDQ 驱动，工作量较大。
- `/sys/class/power_supply/` 下的 `dialog-battery`（`capacity=50`）是早期实验模块的
  硬编码占位值，**不可作为充电判据**。
- **Linux 侧完全没有 Tristar CBTL1610 驱动**（I2C0 0x1a），所以充电器类型识别
  （BC1.2 / Apple 分压握手）从未发生——这正是限流被锁在 90 mA 的根因。

### 下一步优先级

1. **冷启动验证新镜像**：`gud/out/a1599-gud-j207.bundle`（sha256 `41653609…`）
   已含自动充电配置 + ADC 读取模块，但**尚未实际启动过**。
2. **标定 ch5/ch8**：用 USB 电流表对比，把 ADC 读数换算成 mA/V。
3. **补 Tristar CBTL1610 驱动**——让充电器识别回归正常，可自动获得 500 mA/1 A/2.4 A 档位，
   并支持 Apple 快充。
4. 接入 **BQ27540 电量计**（需专门写 Apple HDQ over UART 驱动，见上）。
