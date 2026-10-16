# Omapad GUI 设计稿

> **状态：设计稿 v3（未实现），已过一轮 Codex 评审（2026-10-16，见 §11）。**
> 文中"映射"指向 `ipadctl` CLI 已有实现；硬件相关行为（30 s watchdog、电量计
> 采样耗时、背光必须走宿主侧）来自源码注释的实测结论。
> ⚖️ = 可推翻的决定；⚠️ = 需要上机验证的点，验证前不进 UI 承诺。

## 1. 定位与交互模型（主人 2026-10-16 拍板）

- **简洁第一**：一扇小窗、两个页签（屏幕 / 充电）、一个全局开关。
- **一个开关管自动**：「自动副屏」——自动识别设备名称与 DFU 状态，
  检测到 DFU 就**自动投放 Linux 并接上扩展屏**，全程不用人点。
- **摆放靠拖**：扩展屏在画布里**图形化拖动排列、点选旋转**。
- **亮度、分辨率可调**；**充电是独立页签**，不和显示设置混在一起。
- **常驻托盘**：平时只在 waybar 托盘；点图标弹出配置窗，关窗即隐藏回托盘。
- **风格与 [Recordian](https://github.com/GriphcodeDev/Recordian) 一致**：
  Rust + Slint、暗色、极简、无多余装饰。
- **全 Rust + 低内存常驻（硬约束）**：无网页引擎；内存预算策略见 §2.1
  （硬门槛数字在 P0 同机实测后定，不预写一个达不到的数）。

## 2. 选型

| 项 | 决定 | 说明 |
|---|---|---|
| 界面框架 | **Slint**（同 Recordian） | 声明式 `.slint`；**live-preview 是开发期工具**（slint-viewer / LSP），运行中的应用改样式仍需重编译，不写进发布构建 |
| 渲染后端 | **software（P0 实测已定，2026-10-16）**：托盘态 17.8 MB vs femtovg 117 MB，见 §2.1 | 小窗低频刷新场景 CPU 渲染足够；femtovg 的 GL 上下文常驻吃掉百 MB 级 |
| 托盘 | **ksni**，但必须 `default-features = false, features = ["async-io"]` | ⚠️ ksni 默认特性拉 tokio + zbus（已核实 Cargo.lock）；async-io 路线轻得多。托盘跑在自己线程，回调只发 channel，**绝不在回调里碰 Slint**（Slint 组件与事件循环必须在主线程） |
| 依赖纪律 | 删除 ipadctl 直依赖的 `tokio`（已核实源码零引用）；ksni 按上述关默认特性后重查依赖树；不引 GTK/webview/SVG 库 | 每一 MB 常驻内存都要有出处 |

### 2.1 内存与体积策略（硬约束，门槛后置）

先说结论：**原稿 ≤15/≤35/≤6 MB 的数字依据不足，撤回**。Slint 社区在
Arch/Wayland 上的单机报告：winit+femtovg 开过窗并销毁 UI 后 RSS 仍约 167 MB
（[讨论](https://github.com/slint-ui/slint/discussions/6718)，维护者确认缓存
不能简单卸载）；较旧的最小模板：femtovg ~51 MB、software ~14 MB、strip+LTO 后
二进制 2.6–3.5 MB（[讨论](https://github.com/slint-ui/slint/discussions/3376)）。
单机样本不能外推，但足以否定"隐藏后自然回落 15 MB"。

因此：

1. **P0 原型先行测量**：最小 Slint 窗 + ksni 托盘，同机分别测 femtovg /
   software（编译期二选一，各自构建），固定测量口径（冷启动、开窗、隐藏后、
   重开 N 次的 `VmRSS`）。**实测数字出来才写硬门槛进验收**。
2. 规划量级（femtovg 路线，用于排预期不是承诺）：托盘态（开过窗）50–100 MB、
   开窗 60–180 MB、二进制 8–15 MB。software 路线另测，预期显著更低。
3. **渲染后端编译期选定，进程内不做运行中切换**（Slint 不支持）。所谓"降级
   预案"= 换构建配置重出软件版，不是热切换。
4. `hide()` 只承诺隐藏并释放组件显示期强引用，**不承诺 GL 资源回落**；若实测
   隐藏态超标，再验证"销毁组件重建"路线（需自存页签/选中状态，重开有代价）。
   事件循环用 `run_event_loop_until_quit` 常驻。
5. 其余纪律不变：缓冲全有界（日志环形 200 条）、不缓存图像数据、字体走系统
   fontconfig 不内嵌 CJK、图标用 Slint 原生形状或 KB 级 PNG。
6. Release profile：`opt-level = "s"`、`lto = true`、`codegen-units = 1`、
   `panic = "abort"`、`strip = true`。

**P0 实测结果（2026-10-16，本机 Omarchy/Wayland，`omapad --measure`，
口径：冷启动 1 s → 开窗 2 s → 隐藏 2 s → 重开×3 → 隐藏 2 s）：**

| 渲染器 | 冷启动仅托盘 | 开窗 | 隐藏（开过窗） | 重开 3 次后隐藏 | 二进制 |
|---|---|---|---|---|---|
| **software**（采用） | **17.8 MB** | **52.9 MB** | **21.6 MB** | 21.6 MB | 10.4 MB |
| femtovg（否决） | 117.5 MB | 121.4 MB | 121.4 MB | 122.6 MB | 10.3 MB |

结论：software 全面胜出且隐藏后 RSS 真回落；femtovg 的 GL/EGL 上下文在
未开窗时就把底线抬到 ~117 MB，与 Codex 评审引用的社区报告一致，否决。

据此定下的硬门槛（给 P1 的画布/充电留了余量，超了按 §2.1 手段排查）：
仅托盘 ≤ 25 MB；隐藏常驻 ≤ 30 MB；开窗 ≤ 70 MB；二进制 ≤ 12 MB。
iPadctl CLI 二进制 908 KB。

## 3. 架构与数据流

```text
ipadctl/                     Cargo workspace（拆分自现单 crate）
├─ crates/core/    ipadctl-core   src/*.rs 模块迁入并整理成公共 API；
│                                  project_root() 从 main.rs 提为 core 公共项
├─ crates/cli/     ipadctl        现 main.rs 原样保留（CLI 与 GUI 共用 core/config）
└─ crates/gui/     omapad        Slint 界面 + ksni 托盘 + 后台线程
```

- GUI 以**普通用户**运行；只有 wrapper 走受限 `sudo -n`，配置与 Hyprland 文件
  归普通用户。
- 线程模型（"2 条线程"是目标不是契约，**扫描永不被慢操作阻塞**）：
  - **主线程**：Slint 事件循环 + ksni 托盘服务（async-io，独立 reactor）。
  - **扫描线程**：USB `read_dir` 每 ~1 s、kmsg 每 10 s。只做快操作，任何
    ACM/子进程调用都不在这里。
  - **工作线程（按任务起，上限 2）**：跑慢操作——`flash::flash`（同步子进程
    + 最长 120 s 等待）、电量计采样（单次超时 60 s）、限流回读。**每台设备
    一把互斥锁**（ACM 单会话），投放进行中扫描继续跑。
  - 后台 → UI：channel + `slint::invoke_from_event_loop` 唤醒（`mpsc::send`
    本身叫不醒 UI）；托盘回调同样只发 channel。
- 慢操作（电量计/限流）进充电页时取一次 + 60 s 后台补采，UI 标"采样于 N 秒前"。
- 配置仍写 `~/.config/ipadctl/config.toml`，新增：

  ```toml
  [settings]
  auto_connect = true        # 自动副屏开关（⚖️ 默认开，主人可改）
  flash_wait_secs = 120
  renderer = "auto"          # P0 实测后可能改为 "software"
  ```

## 4. 主窗口（点托盘弹出，约 420×640，关窗=隐藏）

```text
┌─ Omapad ────────────────────────────── ✕ ─┐
│  自动副屏                        (●——) 开  │
│  iPad mini 3 正在自动连接 · 步骤 2/3        │
│─────────────────────────────────────────────│
│  排列                          [↻ 旋转所选] │
│  ┌─────┐ ┌───────────┐ ┌────────┐           │
│  │mini3│ │ HDMI 主屏  │ │Pro 10.5│           │
│  │  ▯  │ │ （只读参照）│ │  ▭▭▭   │           │
│  └─────┘ └───────────┘ └────────┘           │
│─────────────────────────────────────────────│
│  iPad Pro 10.5                     ● 在线   │
│    亮度   ◉───────────────────── 76         │
│    分辨率 [原生 1668×2224 ▾]                │
│─────────────────────────────────────────────│
│  iPad mini 3                   ◌ DFU 待投放 │
│    识别: A1599 · 镜像在位 ✓ · 唯一 DFU ✓    │
│─────────────────────────────────────────────│
│  ⚠ 1-9.2 内核已放弃枚举 — 换口或拔插 ▸      │
│                                             │
│      [ 屏幕 ◫ ]      [ 充电 ⚡ ]             │
└─────────────────────────────────────────────┘
```

**「屏幕」页**：

| 元素 | 行为 | 映射 / 备注 |
|---|---|---|
| 自动副屏开关 | 开=DFU 自动投放；持久化 `[settings].auto_connect` | §5 |
| 排列画布 | 见下方画布细则 | `config` position/transform + `hypr::write_lua` + `hyprctl reload` |
| 亮度滑条 | 150 ms 防抖实时写宿主侧 backlight；松手存默认值；热插拔自动套 | `Connector::set_brightness` |
| 分辨率 | 下拉列出**该连接器的可用模式**（从 `hyprctl monitors` 读），选中后写入 lua 的 `mode` 字段（现在恒为 `"preferred"`，需扩展 `render_lua`）。⚠️ 半分辨率档的存在性与旋转后的逻辑尺寸**上机验证后才进 UI**；验证前该下拉只显示当前模式不可改 | `config` 新增 `mode` 字段 + `hypr::render_lua` 扩展 |
| 首次接入横幅 | 检测 `hypr::hooked()`：monitors.lua 未 `require` 我们的布局文件时，横幅提示 + **一键修复（diff 预览后写入）** + 提醒删除旧的手写 iPad 规则（顺序生效会打架）。这是布局生效的前提，**在 P1，不推 P2** | `hypr::hooked` / `paths::monitors_lua_path` |

**画布细则**（Codex 评审后补）：

- 坐标即 Hyprland **逻辑坐标**，支持负值（现默认配置就有 `-856x1146`）；
  画布带原点标记。矩形逻辑尺寸 = `panel` 经 transform（竖横交换宽高）÷ scale。
- 旋转按固定顺序 `0 → 1 → 3 → 2 → 0` 循环（数值 +1 会落在 2=倒屏上，且
  config.rs 语义是 0/1/2/3 四档显式定义）。
- 拖动只预览，**松手才校验**（与其他屏重叠、全部移出可见区）→ 保存 → 生成
  lua → reload → 读回连接器实际状态确认。
- 已知限制要在画布上如实标注：布局按 EDID `desc:` 匹配，**同型号两台会撞**
  （hypr.rs 已注明），幽灵连接器也会匹配——同型号多台时画布显示"无法区分"
  警示，不当成 P3 问题。

**「充电」页**：

```text
│  iPad Pro 10.5          +793 mA ↗ 充电中  │
│    限流   [500] [1000] [1500] mA   自定义…│
│    目标已提交 → 看门狗已写入 1000 mA      │
```

- 状态**三态**，用语收紧（Codex 评审后改）：`目标已提交`（写 watchdog 文件
  成功）→ `看门狗已写入`（J207 判据：dmesg 里**晚于本次提交时间**且值匹配的
  `d2355: write 0x044e` 记录；这是日志证据不是寄存器回读）→ A7 机型额外可到
  `寄存器已回读`（I2C 读 0x4C0）。维护口失联/超时单独报，不混在"未生效"里。
- 档位：J207 500/1000/1500 mA；A7 bf/7f（hex 档，标注 ≈+650~750 / ≈+206 mA
  估算区间，不冒充精确）。J207 无电量计：显示"无电量计脚本"。

## 5. 自动投放（「自动副屏」开启时）

**预检条件**（每条都触发"只报告不动手"）：

| 条件 | 真实能力边界（如实） |
|---|---|
| CPID/BDID 在机型表内 | `model::from_dfu_serial`；认不出**永不投放**，列 serial 原文 |
| DFU 恰好一台 | checkra1n 无法指定目标，多台列出端口要求拔线 |
| 镜像在位 | `model::bundle_info` **只做"bundle 文件存在 + IMAGE*.json 清单记录可读"**；真正的 SHA256 校验发生在 Python wrapper 内部（校验失败它会退出码 2）。UI 文案写"镜像在位"，不写"哈希已校验" |
| `sudo -n` 免密可用 | 预检跑 `sudo -n true` |

**成功判据分级**（不再是"出现 USB 设备就算成功"）：

1. `USB 上线`：`flash::wait_online` 认到新的 GUD 端口；
2. `连接器可用`：`hypr::connectors()` 交叉校验该端口对应 DRM connector 且
   `status == connected`；
3. `桌面生效`：`hyprctl monitors` 里该连接器被 Hyprland 接管（有当前模式）。

三步都过才在状态行打勾淡出；任何一步卡住，状态行停在那一级并给出下一步线索。

**失败与重试**：wrapper 退出码 2 → `STOP:` 原文展示；停在 uartproxy → DTB
兼容线索；超时单独说明。**失败后同一"DFU 出现周期"内不自动重试**——周期按
USB 端口的 DFU 会话计（换口=新周期，允许再自动试）；程序重启清除失败锁定；
手动"重试"永远重跑全预检。⚖️ 失败时是否发托盘通知：只报失败，成功不打扰。

## 6. 托盘（ksni）

- 图标聚合态：正常 / DFU 自动连接中（琥珀，tooltip 显示 x/3 与判据级）/ 错误。
- 点击（`activate`）= 弹窗聚焦；右键菜单：每台设备子菜单（亮度 25/50/75/100、
  关背光、限流档位）、"打开 Omapad"、"退出"。⚠️ 左键行为需在 waybar 实际
  SNI host 上验证。

## 7. 次级功能（P2+）

维护控制台（图形化 `exec`）；日志时间线（kmsg + 状态变迁史）；
环境自检页（检测 wrapper/工具链/镜像/sudo/monitors.lua 是否就绪，只报不装，
⚖️ 安装引导不做）。

## 8. 设计系统（v2，2026-10-16 主人要求顶级重设计；Recordian DNA 保留）

**栅格与字阶**：4px 基栅，间距令牌 sp-1..6 = 4/8/12/16/20/24。字阶五级
（8pt 基）：wordmark 13 mono 800 ls3px / 标题 13.5 600 / 正文 11.5 /
caption 10.5 mono / 段标 10 mono ls1.5px（中英并置如「排列 LAYOUT」）。

**色彩与海拔**（Recordian 令牌 + 两级新海拔）：窗口 `#0D120D` → 卡片
`#121A12`（描边 `#243024`，悬浮 `#162116`/`#2E3B2E`）→ 内凹槽 `#090D09`
（分段控件/画布）→ 提示条 `#1A2216`（方角 3px 左边框：橄榄=信息、洋红=警示）。
洋红 `#FF2E88`（选中底 `#3A1520`）＝品牌/主操作/失败；橄榄 `#B8C77A`＝在线/
信息；琥珀 `#F9E2AF`＝进行中。文字 `#F2F5F0` / `#A7B4A6` / `#7E8B7D` 三级，
对比度全部达标。等宽 `Noto Sans Mono CJK SC`。

**圆角阶梯**：窗内容器 12 / 按钮 7 / 胶囊与分段 9(6)。**组件族**：
- 按钮三级：主（洋红描边+洋红字+强调底）/ 幽灵（描边，hover 提亮）/ 紧凑
  （22px 高，工具行用）；高 26，字号 11.5 mono。
- 分段控件（页签/分辨率共用）：内凹槽 + 激活胶囊，26px。
- 状态呈现三件套：设备卡左 3px 状态色条（裁于卡片圆角内）+ 状态胶囊
  （状态色 12% 透明底 + 状态色 mono 小字）+ 语义色文字。
- 开关 40×22：粉轨+深钮=开（直觉向，修正 Recordian 反直觉版）。
- 滑条：3px 暗轨 + 洋红填充 + 14px 近白钮 + 等宽百分比。

**版式**：顶栏卡（wordmark + 一句话状态 + 总开关，最重要的控件常驻首屏）→
提示条（仅出现时占位）→ 分段页签 → 节区（段标 + 画布 + caption 提示）→
设备卡列表 → 底部信息栏（分割线 + 左状态右版本）。卡内左色条+内容缩进 17px
对齐。窗口 460×780。动效仅滑条实时反馈。

## 9. 分期（评审后重排，每段独立上机验收）

| 段 | 内容 | 验收 |
|---|---|---|
| **P0 骨架** | workspace 拆分；托盘 + 主窗生命周期（关窗隐藏、`run_event_loop_until_quit`）；只读设备识别列表；**内存原型实测（femtovg vs software，定默认与硬门槛）**；ksni 关默认特性后依赖树复查 | ✅ **2026-10-16 完成**：26 测试全绿（含实机 GUD 连接器测试）；三台实机 iPad 上屏（截图 `.scratch/p0-measure/p0-rightmon.png`）；RSS 数字见 §2.1 |
| **P1a 自动投放** | 开关 + 预检 + 分级成功判据 + 失败呈现 | 🟡 代码完成（2026-10-16）：29 测试全绿、UI 实机验证（三台在线 + 开关渲染）、sudo -n 实测可用；**DFU 端到端尚未实跑**（需一台真实置入 DFU 的 iPad，见 §12） |
| **P1b 画布与显示** | 拖动/旋转画布（含重叠校验）；monitors.lua 首次接入横幅 + 一键修复；亮度滑条；分辨率（先上机验证模式档位，再开 UI） | 🟡 大部完成（2026-10-16）：模式档位上机验证通过（每台恰两档）；布局链路端到端实测（3200→3208→还原，hyprctl 回读一致）；横幅一键修复实机生效（monitors.lua 已接入）；界面全要素渲染验证。**手动拖拽/旋转的指针交互待真人试用**（几何换算与链路已分别验证） |
| **P1c 充电页** | 档位 + 三态生效用语 + 电流显示 | 调档 → 看门狗已写入 |
| **P2** | 维护控制台、日志时间线、环境自检 | 排查不开终端 |

## 10. 已定与待定

**已定**：Slint + ksni(async-io) 托盘 + 单窗两页签；自动副屏开关；图形化
排列拖动+旋转；亮度/分辨率可调（分辨率档位上机验证后开放）；充电独立页；
Recordian 风格；全 Rust + 低内存（硬门槛 P0 实测定）。

**待定（⚖️）**：`auto_connect` 默认开；托盘通知只报失败；半分辨率模式细节；
同型号多台的区分方案（画布先如实警告）。

## 11. 评审记录

- **2026-10-16 Codex 评审**（全文：`.scratch/codex-review-design.md`，128k
  tokens，只读未改文件）。结论：**修改后可开工**。
- 5 个 🔴 阻断全部处置：① ksni 默认拉 tokio → 关默认特性改 async-io（已
  核实 Cargo.lock）；② 分辨率只改 scale 不换模式 → 改为显式 mode 字段 +
  上机验证；③ monitors.lua 首次接入是布局生效前提 → 从 P2 提前到 P1b；
  ④ 预检/成功判据过强 → 如实化（哈希校验在 wrapper 内；成功分三级）；
  ⑤ software renderer 预案不可热切换 → 改为编译期选择。
- 🟡 建议采纳：内存预算撤回改 P0 实测、hide() 语义如实化、线程模型细化
  （扫描不被阻塞、每设备互斥）、画布坐标/旋转顺序/重叠校验、充电三态用语、
  live-preview 定位为开发期工具、分期重排为 P0→P1a/b/c。
- 🔵 疑问处置：重试身份=DFU 出现周期、重启清锁、手动重试全预检；P1 面向
  "环境已就绪的主机"，环境自检进 P2。

- **2026-10-16 第二轮 Codex 评审（实现评审）**（全文：
  `.scratch/codex-review-code.md`）。结论：**10 项实现缺陷，先修复再进 P1c**。
  10 项全部当日修复：
  ①配置解析失败静默回退自动开启 → Settings 字段级 serde default + GUI 读失败
  即关闭并报原因；②预检与执行未绑定设备 → flash_with_progress 增加
  expect=(端口,机型) 执行前复核 + 预检后二次复核；③上线认领可能错认别台 →
  wait_online 按机型认领；④扫描线程 Weak::upgrade 恒 None（横幅死代码）→
  invoke 化；⑤同端口重插不套默认 → seen 随消失清理 + 首轮基线标志；
  ⑥布局操作交错 → layout_mu 全链路串行（校验→存→写→重载→回读）；
  ⑦拖拽位移反馈环 → 触区静止、仅视觉块随 dx/dy 移动；⑧旧配置缺机型时
  静默丢改动 → Config::entry_or_default；⑨状态行延迟清除抹掉新消息 →
  文本比对代际；⑩注释误判已接入 → 只认真正的 require 调用。
  建议项一并处理：config 原子写（tmp+rename）、preflight 不持 Shared 锁、
  模式档位按宽度识别（不依赖 availableModes 顺序）。36 测试全绿零警告。
  🔵 疑问处置：投放中退出进程 = 允许退出，wrapper 子进程独立继续，下次
  启动按设备状态自愈（⚖️ 记录在案）。实施中的方法论弯路也记一笔：
  用 `strings` 查中文标记验证二进制新鲜度会得出错误结论（strings 只出
  ASCII）；且 release 跨 crate LTO 会把 CLI 传 None 的分支优化掉——
  以 dev 构建或 GUI 二进制做标记校验才可靠。

## 12. P0 实施记录（2026-10-16）

**建成的结构**：根包 `ipadctl` 升级为 lib+bin（`src/lib.rs` 暴露公共模块，
`src/main.rs` 只剩 CLI 参数编排，`project_root()` 移入 `paths.rs`）；GUI 是
`crates/gui` 的 `omapad` 包（`--features gui-software|gui-femtovg` 编译期二选一）。
与原稿"三 crate"的差异：core 与 cli 同包——拆开的实际收益只在发布节奏，留到
需要时再拆，结构上公共 API 已就位。

**依赖落地**：直依赖 `tokio` 删除；`ksni` 用 `default-features=false,
features=["async-io","blocking"]`（`blocking` 是零依赖门闸，给同步 API；执行器
是 async-io 系，zbus 5.19）；`cargo tree -i tokio` 确认整树无 tokio。
Slint 1.18 特性：`std + backend-winit-wayland + compat-1-18` + 渲染器
（`renderer-winit-software + software-renderer-systemfonts` 或
`renderer-winit-femtovg`）——注意 1.18 的渲染器特性名带 `winit-` 前缀，
且无 `default-fonts`。

**实现中发现的 API 事实**：Slint 1.18 属性声明用 `in-out property`（`public
property` 不再可解析）；`Window` 无 focus 类方法（Wayland 也不允许客户端抢
焦点，show 即可）；ksni 0.3 的分隔符是 `MenuItem::Separator` 变体，托盘经
`ksni::blocking::TrayMethods::spawn()` 起。**托盘失败的退化路径**：SNI 起不来
时"关窗=退出"，避免关窗即隐藏的进程变孤儿。

**证据**（均在 `.scratch/p0-measure/`）：两渲染器二进制、`--measure` 输出、
实机截图（三台 iPad 全"在线"，中文字形正常）。`cargo test --workspace`
26 通过 0 失败。

### P1a 实施记录（2026-10-16）

**core**：`Config` 新增 `[settings]`（`auto_connect` 默认开 / `flash_wait_secs`
默认 120；旧配置无此节照常解析，测试覆盖）；`flash` 事件化
（`flash_with_progress` + `FlashEvent`，库不打印，CLI 回调里保持原文案）；
`preflight`（启动器+镜像在位 + `sudo -n true`；**哈希真校验在 wrapper 内**）；
`auto_decide` 纯函数（开关/忙/多 DFU/未知机型/退避/触发六分支，测试覆盖）；
`hypr::takeover` 分级判据（UsbOnly → ConnectorReady → DesktopActive，
第三级读 DRM connector 的 `enabled` 属性，不解析 hyprctl）。

**GUI**：「自动副屏」开关（持久化到 config.toml，与 CLI 同一真相）、状态行
（投放进度/分级结果/多 DFU 拒绝）、DFU 卡三态（待投放/进行中/失败+重试）、
失败按"DFU 出现周期"退避（设备消失即解锁，重试重跑全预检）、成功消息 5 s
淡出、失败消息保留。`--measure` 模式**禁用自动投放**（测量绝不刷机器）。
找不到仓库根目录（IPADCTL_ROOT / 上溯）时自动禁用并在状态行说明。

**实机验证（已做）**：三台 iPad 在线卡与 sysfs 逐项一致（1-12 / 1-9.1 /
1-9.3）；开关绿色 ON 渲染正确；`sudo -n true` 实测可用；扫描空转安全
（无 DFU 时 `auto_decide` = Sleep）。

**待实机验收（未做，不冒称已完成）**：真实 DFU 设备的端到端——预检触发、
checkm8→上传→上线、状态行走到"桌面生效 ✓"、失败路径（拔线超时/uartproxy）
与重试。做一台即可。

**证据学教训（本轮踩到）**：视觉模型对截图的**引导性提问会产出幻觉**——
先被"读"出一段不存在的多 DFU 拒绝文案，sysfs 核实为假。今后截图验证一律
中立转写 + sysfs/进程级证据交叉，不以单一模型读文为准。

### P1b 实施记录（2026-10-16）

**模式档位上机验证（先验证再开 UI，按设计流程）**：三台 iPad 的 EDID 经
hyprctl `availableModes` 确认**恰为两档**——mini 3/mini 2：`1536x2048@60` +
`768x1024@60`；Pro 10.5：`1668x2224@60` + `834x1112@60`。分辨率 UI 按两档
分段按钮实现（原生 = preferred/None，半分辨率 = 第二档写 `mode`）。

**core**：`DeviceSettings.mode`（缺省 None=preferred）；`Model.short` 画布短标签；
`hypr::HyprMonitor` 解析（驼峰字段 + 模式字符串数组，实测样本进单测）；
`hyprctl_monitors / monitor_model / logical_rect / rects_overlap / next_transform
/ insert_require(_into) / reload_hyprland / brightness_pct`；`render_lua` 输出
mode 档位。35 测试全绿（新增 6 个）。

**GUI**：画布（hyprctl 真实布局绘制，iPad 矩形可拖/选中，宿主屏灰色只读；
松手 → 重叠校验 → 存配置 → 写 lua → reload → 600 ms 后回读刷新）；旋转按钮
按 0→1→3→2 循环、中心锚定、交换宽高后再校验；亮度滑条（TouchArea 内建
mouse-x/pressed-x 拖动，150 ms 节流实时写，松手持久化 + 5 s 回显缓存防跳变；
热插拔上线自动套配置默认亮度）；monitors.lua 横幅（收起警告/展开预览+确认，
写前自动 .ipadctl-bak 备份）；页签（屏幕/充电占位）；同型号多台画布警示。

**实机验证（已做）**：
- 布局链路端到端：a1599 3200→3208（`config set`+`apply`+reload）→ hyprctl
  回读 x=3208 → 还原 3200。**"尾部 require 覆盖手写规则"的顺序语义实测成立**。
- 一键修复：按 `insert_require` 相同内容手工演练（备份+追加+reload），生效。
  现 monitors.lua 已保持接入状态（这正是功能目的）。
- 亮度：`apply` 三台背光全部写入成功（见下面的权限发现）。
- 界面：画布 6 矩形/滑条/分辨率按钮/页签逐字转写验证（中立提问）。

**实机发现并处置的两个雷（重要）**：
1. `~/.config/ipadctl/config.toml` 与 `~/.config/hypr/ipadctl.lua` 曾是 **root
   属主**（早期某次 sudo 运行所写）——GUI 一切配置写入都会 EACCES。已
   chown 回用户；根因是工具在 sudo 下运行时仍写用户路径，后续可在 core
   加属主自检（⚖️ P2）。
2. backlight sysfs 实测 root:644，普通用户直写必失败。`Connector::set_brightness`
   已加 EACCES → `sudo -n sh -c echo` 降级（与 wrapper 同一免密授权面）。

**未验证（如实）**：画布拖拽/旋转的**指针交互**（需真人或 computer-use 操作；
坐标几何与生效链路已分别由单测和 CLI 端到端覆盖）。Slint 交互实现注意：
`PointerEvent` 无位置字段，拖动要用 TouchArea 内建 `mouse-x/pressed-x` +
`moved` 回调；长度÷长度得 float 的写法在 1.18 可用。

### Recordian 皮肤实施记录（2026-10-16）

主人提供 Recordian 设置界面截图，指定"和它一个风格"。令牌见 §8（重写）。
结构适配：品牌行（洋红等宽 OMARPAD）+ 自动副屏做成 Recordian 式设置行卡
（标题+说明+右侧开关）+ 状态/横幅改提示条（3px 左边框）+ 节标题等宽灰绿 +
底部状态栏分割线。Recordian 自身缺陷（开关反直觉、低对比）按主人"修复缺陷"
要求规避，未复制。界面改动仅 `.slint` 皮肤层与 Rust 语义色常量，逻辑零改动；
36 测试全绿；截图验证（`skin2.png`）。

**工程异常再发一次（记录处置）**：`cargo build --release` 指纹缓存再次把
陈旧二进制报成 Fresh（源码 mtime 更新也不重编，ipadctl/omapad 各中招一次）。
处置：`cargo clean --release -p <包>` 强制重建 + 字节级标记校验。构建产物
发布前一律跑标记校验，不信"Finished 0.0xs"。

### 设计系统 v2 实施记录（2026-10-16）

主人要求"顶级软件设计师水准"整体重设计。产出 = §8 设计系统（栅格/字阶/
海拔/组件族/版式全部令牌化）+ app.slint 整体重写。关键设计决策：总开关
并入顶栏卡（首屏常驻，替代原先的大设置卡）；页签与分辨率共用分段控件；
设备状态升级为"左色条 + 状态胶囊"双通道（色弱可辨）；旋转按钮仅选中 iPad
时出现（按需呈现）；画布加 caption 说明深浅块语义；底部信息栏左状态右版本。
逻辑回调/属性零改动，36 测试全绿。渲染验证 `.scratch/p0-measure/design1-win.png`
（中立转写：层级清晰/间距均匀/无截断重叠，6 画布块标签与三卡状态全部正确）。
