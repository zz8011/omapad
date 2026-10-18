# GUD 副屏方案（2026-10-15 新增）

让 iPad 以 **GUD（Generic USB Display）** 设备出现在宿主上。宿主使用主线 `gud.ko`，生成真实 DRM connector（`USB-1`），Hyprland 把它当普通显示器处理，不需要宿主侧采集、编码程序，也不依赖硬解。

## 组成

| 文件 | 作用 |
|---|---|
| `gudd.c` | iPad 端 FunctionFS 守护进程：处理 GUD 控制请求；bulk 接收 RGB565（可 LZ4）；写入 `/dev/fb0`。模式 1536x2048 和 768x1024（设备端 2 倍放大） |
| `rootfs/sbin/setup-gadget` | 开机组 gadget：acm.usb0 + acm.usb1（都是 root shell）+ ffs.gud，VID:PID 为 1d50:614d；GUD 失败时退回 2×ACM（1d6b:0104） |
| `rootfs/etc/inittab` | ttyGS0、ttyGS1 各跑一个 getty；去掉 JPEG displayd |
| `build_image.py` | 基于实机成功过的 gadget-fix bundle（`a069bf7…`），只改 rootfs，boot 前缀保持原字节。`j85m`（默认）→ `out/a1599-gud.bundle`；`j86` → `out/a1599-gud-j86.bundle` |
| `dtb/` | 由内核源码编出的机型 DTB。`j86` 用它替换 bundle 里板名错误的 DTB；差异与重编方法见 `dtb/README.md` |
| `launch/alpine_dfu_wrapper.py` | 原 DFU wrapper 的副本，只改 bundle 路径和哈希（J85M 用） |
| `launch/alpine_dfu_wrapper_j86.py` | 同上，指向 j86 bundle（J86 用） |
| `a1599sh.py` | 宿主侧工具：通过维护 ACM 执行命令、用 base64 上传文件 |
| `gud-switch.sh` | 在运行中的旧镜像里现场切换 gadget（实验用；见下方已知问题） |

`out/usb_f_fs.ko`：运行内核（v2.4 配置）没有内建 FunctionFS。这个模块是把 `drivers/usb/gadget/function/f_fs.c` 按 config-v2.4 编成外部模块得到的（构建目录 `.scratch/kbuild-v24`，源码树 `2026-10-11/task-2/.build/a1599/src/linux`）。模块用到的 132 个符号都由 vmlinux 导出。交叉编译使用 `.scratch/cc.sh`：clang 加 Alpine v3.24 的 musl-dev、compiler-rt 静态链接，各 apk 包都按已验签 APKINDEX 的校验值核对过。

## 证据（区分实测和推测）

- **本机回环实测**（dummy_hcd + 宿主 gud.ko + x86 版 gudd，用文件当 fb）：
  - probe 成功，得到 `USB-1`
  - 1536x2048 渐变图逐字节一致
  - 6 MB 随机噪声整帧按 512 KiB 分块传输，逐字节一致，0 错误
- **iPad 实测（现场切换，2026-10-15）**：
  - 往运行内核 insmod `usb_f_fs.ko` 成功
  - 切换后宿主识别为 `1d50:614d "A1599 GUD Display"`，gud 驱动建出 card2、fb2，connector 状态 connected，模式 1536x2048 和 768x1024
  - 当时版本 bulk 上限是 12 MiB：第一帧和一张不可压缩图的 flush 各失败一次（-110）。推测原因是 FunctionFS 大块 kmalloc 失败，已改为 512 KiB，回环测试已验证
  - 切换后两个 ACM 都没有回应，原因未查明，所以改为把 GUD 配置做进镜像
  - iPad 屏幕上的实际显示效果还没有经过用户确认
- **Hyprland**（早期结论，后已被下文推翻）：原装 aquamarine 不会接管运行中新增的 DRM 设备。补丁版可以热插拔接管，见“宿主 Hyprland 接入”。

## 实机排查记录（2026-10-15，实测）

| 现象 | 原因（证据） | 处理 |
|---|---|---|
| 大块未压缩 bulk 时 flush -110 | FunctionFS 每次 read 都 kmalloc 整块缓冲，单块最大 12 MiB | `max_buffer_size` 设为 512 KiB，宿主按行拆分 |
| 整帧渐变后两个 ACM 和 GUD 全部失联（宿主没有断开记录） | 看门狗打到屏幕上的 dmesg：`functionfs read size 9727 > requested size 9667`，随后 `dwc2_hsotg_ep_stop_xfr: timeout`。dwc2 处理多包且以短包结尾的较大 OUT 请求时，把实际长度算多了 60 字节，之后数据错位、端点卡死。纯色块（5 包）和全满包的噪声块不会触发 | 每次 read 不超过 2048 字节（4 包），可用 `/run/gudd-chunk` 调整 |
| 写屏时 ep0 被延迟 | 写屏时持有 ep0 也要用的锁 | 写屏移到锁外；DIRTYFB 由 flush 线程合并提交，≤30 Hz |
| fbdev mmap | 已改用 DRM dumb buffer 加 DIRTYFB（`-b` 可退回 fbdev） | |

修复后实测：整帧渐变，再加 5 帧随机噪声和一帧渐变，宿主 gud 统计 0 错误，shell 一直在线，dmesg 里没有 functionfs/dwc2 报错。

排查过程中排除的假设（都有实测）：CPU 满载（两核 100% 时只收数据仍然稳定）、framebuffer 与 RAM 重叠（fb 在 0x83d000000，RAM 到 0x83cffffff 为止）、刷新频率（整屏 DIRTYFB 只要 4–6 ms）。

（后续：读取方式改为“整包部分大请求 + 尾部短包单独读”，见“性能”。）

排查开关（`/run` 下的文件）：`gudd-noblit`（不写屏）、`gudd-nodirty`（写缓冲但不提交）、`gudd-sleep`（毫秒）、`gudd-verbose`、`gudd-fps`、`gudd-chunk`、`gudd-watch`（启用看门狗：心跳中断 15 秒后把诊断信息打印到 iPad 屏幕）。

## 使用（新 RAM 会话）

```sh
# iPad 进 DFU 后
sh gud/boot-ipad.sh      # 等待 DFU → checkm8/Pongo → 上传 → 等待 GUD 枚举
# Hyprland（补丁版）会自动接管；位置/方向见 ~/.config/hypr/monitors.lua 中的 desc: 规则
```

## 宿主 Hyprland 接入（2026-10-15）

- **实测**：重新登录后 Hyprland 已枚举出 `USB-1`（1536x2048@60），但 iPad 上没有画面。日志显示：`CDRMRenderer(drm): Can't create renderer, no matching devices found` → `initMgpu: no renderer`。aquamarine 0.15.1（以及上游 master 52ea11f）要求副 GPU 能建立 EGL 渲染器来做 blit；Mesa 只为有 render node 的设备提供 EGL device，gud 和 vkms 都不满足，所以每次提交都失败、被丢弃。
- **补丁** `aquamarine-0.15.1-gud-direct-import.patch`：副 GPU 无法建立渲染器时，直接把主 GPU 渲染出的线性缓冲（multigpu swapchain 本来就强制 linear）导入副设备扫描输出，不做 blit。这和 wlroots 处理 DisplayLink 类设备的做法相同。头文件只新增一个非虚成员函数，类布局不变；`LD_BIND_NOW` 符号解析检查通过。
- **部署**：库放在 `~/.local/lib/aquamarine-gud/`，由 `~/.config/uwsm/env-hyprland` 设置 `LD_LIBRARY_PATH`。系统库没有改动。回退方法：删除该文件后重新登录。系统 aquamarine 升级后，需要按新版本重新打补丁，或者删除该文件。
- **实测**：补丁版 Hyprland 在 iPad 上显示了桌面，可以把窗口拖过去；也能热插拔接管新的 GUD 设备（iPad 重新启动、或 gudd 重启后不用重新登录）。原装库在 gud 设备存在时会在 `CDRMBackend::flushAsyncCommitEvents` 崩溃（dmesg 中的 4 次 segfault 偏移都对应原装库）；补丁库目前运行稳定。

## 性能（2026-10-15，实测）

ftrace function profile（宿主拖动/滚动窗口 15 秒）：

| 版本 | bulk 平均耗时 | 15 秒内 bulk 次数 | gudd 吞吐 |
|---|---|---|---|
| 每次只读 2 KB，单线程 | 6.95 ms | 1636 | 4.6–5.3 MB/s |
| 整包部分 16 KiB 大读 + 尾部短包单独读；接收/处理两线程流水线；官方 LZ4；无分支转换循环 | 3.14 ms | 3059 | 10.5–12.1 MB/s |

控制传输约 0.2 ms，宿主 RGB565 转换约 0.15–0.3 ms，LZ4 压缩约 0.15 ms，都不是瓶颈。第二版每次 bulk 平均约 100 KB，3.14 ms 传完，折合约 32 MB/s，已接近 USB 2.0 批量传输上限；iPad CPU 仍有 59% 空闲。原生分辨率下要再提升，只能减少传输的数据量，比如用 768x1024 模式。

## 宿主显示器配置（2026-10-15）

- gudd 通过 `GET_CONNECTOR_EDID` 提供合成 EDID（`gud/edid.inc`：APP / iPad mini 3 / A1599，1536x2048，120x160 mm）。Hyprland 中显示为 `Apple Computer Inc iPad mini 3 A1599`。接口名 USB-N 每次重连都会变，所以规则按 `desc:` 匹配。
- `~/.config/hypr/monitors.lua` 中的规则：iPad `position = "512x1080", scale = 2`（在 HDMI-A-1 左边）；`Virtual-1` 设为 `disabled`。修改前的备份是 `monitors.lua.bak.*-before-ipad`。
- 方向需要手动设置 `transform`（1 = 90°，3 = 270°）。iPad 上的 Linux 目前没有可用的方向传感器驱动，**不能自动旋转**。
- 已知问题：iPad 断开后，aquamarine 会留下一个没有描述的残留输出（USB-N），重新登录后消失。
- gudd 可以热更新：上传到 `/run/gudd.new` → `mv` 到 `/usr/sbin/gudd` → `killall gudd`，gudd-supervisor 会自动重启并重新绑定 UDC，Hyprland 随之热插拔接管。

## 背光、熄屏与断线恢复（2026-10-15，实测）

- **背光**：设备树中的 Anya PMIC 背光（`pmic@3c:backlight@600`，主线驱动 `apple_pmic_bl`）在 iPad 上可以直接使用，最大值 2047，iBoot 交接时为 1638（约 80%）。gudd 通过 GUD 背光属性（编号 12）把它暴露给宿主，宿主随之出现 `card*-USB-*-backlight`（0–100），可以用 `brightnessctl -d card4-USB-2-backlight set 40` 之类的命令调节（实测 20%→60% 生效，PMIC 读回 1229）。
- **开机默认亮度 100%**（用户要求）：gudd 每次开机后首次启动时把背光写到最大，并创建 `/run/gudd-bl-initialized`；之后 gudd 重启时保留当前亮度。实测 J207 为 2047/2047，宿主背光设备显示 100。
- **熄屏**：宿主 DPMS 关闭 iPad 时，gudd 收到 SET_DISPLAY_ENABLE 0，把背光写成 0；重新打开时恢复（实测）。注意：Lua 版 `hl.dsp.dpms({ action = "toggle", monitor = ... })` 用 ID 或 `desc:` 能生效，用名字 `USB-2` 不生效。
- **CPU**：cpufreq 已启用，调速器为 schedutil（300 MHz–1.3 GHz）。
- **电池/充电**：Linux 中没有 power_supply 设备，上游也没有 A7 的充电/电池驱动。用户观察到接电脑使用时电量在下降。
- **断线恢复**：A1599 的 USB 没有 VBUS 检测。拔线、宿主重启或端口断电后，UDC 仍显示 `configured`，但不再响应宿主枚举（宿主报 `Device not responding to setup address`；宿主侧 authorized 0/1 不会触发这个问题）。gudd 声明 `POLL_STATUS` 让宿主每 10 秒轮询，收到控制请求时更新 `/run/usb-hb`。`usb-linkwatch` 在心跳超过 25 秒时逐级恢复（soft_connect → 重绑 UDC → 重载 dwc2）。实测端口断电 6 秒后，上电 22 秒自动恢复，第一级 soft_connect 就足够。
- **已知问题**：每次断开后，aquamarine 会留下一个残留输出（如 `USB-1`），并不断尝试 modeset（日志中出现大量 `failed to create a modeset blob`），重新登录后消失。
- 一键启动：`gud/boot-ipad.sh`（等待 DFU → checkm8/Pongo → 上传 → 等待 GUD 枚举）。
- **实测（用户操作）**：物理拔插线、宿主重启各一次，usb-linkwatch 都在检测到心跳中断后约 3 秒内恢复；宿主重启后 Hyprland 自动把 iPad 放回设定位置，不需要进 DFU。

## iPad Pro 10.5（A1701，J207，A10X）（2026-10-15，实测）

- DFU 报告 `CPID:8011 BDID:04`，按 libirecovery 对照表是 iPad7,3（J207）。A1599 是 `CPID:8960 BDID:32`。`boot-ipad.sh` 按这两个值选择启动包。
- 复用：m1n1（原二进制已含 t8011 支持）、checkra1n/Pongo、rootfs、gudd、gadget 脚本。上游 DTS `t8011-j207.dts` 的 USB 设备控制器同样是 dwc2（`apple,t8010-dwc2`）。
- **4K 页内核在 A10X 上无法启动**：Pongo bootm 后黑屏、没有任何输出。改用 16K 页内核（`.scratch/kbuild-16k`，其余配置与 v2.4 相同），`usb_f_fs.ko` 也按 16K 内核重新编译（`out/usb_f_fs-16k.ko`）。用户态 ELF 都是 64K 对齐，不需要改动。
- 16K 内核会停在 `PM: genpd: Disabling unused power domains`（屏幕照片实测），加上 bootargs `pd_ignore_unused clk_ignore_unused` 后可以正常进入 init。
- 实测结果：3 核、4 GB 内存；fb 1668x2224（stride 6672）；背光 `pmic@3c:backlight@b00`（初始 71%）；GUD 枚举成功，Hyprland 显示为 `Apple Computer Inc iPad Pro 10.5 A1701`。monitors.lua 中规则：`position 168x1080, scale 2, transform 1`。
- gudd 的 EDID 改为按 `/proc/device-tree/compatible` 查表运行时生成（A1599 的结果与原 `edid.inc` 逐字节一致，见 `tests/test_edid.c`）；背光路径改为自动查找；usb-linkwatch 的 dwc2 设备名改为从 UDC 反查。

- **启动末尾偶发卡住（未完全定位）**：4 次带 `pd_ignore_unused` 的启动里，第 1 次成功，第 2、3 次停在 `PM: genpd: Not disabling unused power domains` 之后（屏幕照片实测）。其中第 2 次带 `dwc2.apple_rx_fifo=1500`，第 3 次不带，可见与 FIFO 参数无关。第 4 次加 `cpufreq.off=1 initcall_debug` 后成功，genpd 之后依次是 `of_platform_sync_state_init` → `Freeing unused kernel memory` → `Run /init`。怀疑与 A10X Fusion 调频有关，但样本太少，不能下结论。以后再卡住时，`initcall_debug` 会在屏幕上显示最后一个 `calling ...`，即卡住的函数。
- **dwc2 RX FIFO 实验**：J207 上单次大请求只有约 18 MB/s（DMA 模式，每次传输 7–8 次中断；64 KiB 和 512 KiB 两种读取块大小结果相同）。推测是 539 字的 RX FIFO 限制了每个微帧能收的包数。为此写了 `linux-dwc2-apple-rx-fifo.patch`（启动参数 `dwc2.apple_rx_fifo=`），但验证被上面的偶发卡住打断，补丁已从内核源码撤回（原文件 `.scratch/dwc2-params.c.orig`），效果待验证。

## REA 逆向：iOS 的 dwc2 设备驱动（2026-10-15，静态分析）

目标：iOS 12.5.8（J85M/A7）kernelcache，`com.apple.driver.AppleSynopsysOTGDevice`（源码路径 `AppleSynopsysOTG2-252.220.2`）。Ghidra 12.1.4 直接导入会因 chained fixups 越界失败；在末尾补 64 KiB 零（`.scratch/re-otg/kc-12.5.8-j85m-padded.macho`）后可以导入，需要 `REA_GHIDRA_STARTUP_TIMEOUT_MS=3600000`。

- `AppleSynopsysOTGDevice::start`（`FUN_fffffff0083612d4`，REA 证据 `ev_972a3794…`）：从 ADT 读取 `fifo-depth`（必须 ≥ 0x76b）、`ahb-burst`（默认 10）、`phy-interface`（默认 16 位）、`buffer-options`、`clock-mask` 等。
- 核心初始化（`FUN_fffffff008366700`，证据 `ev_48e745b1…`），**观测到的写入**：GAHBCFG = `ahb-burst | 0x21`（默认 0x2B：DMAEn + HBSTLEN=5 即 INCR8）；GUSBCFG = `0x1400 | 8`（16 位 UTMI，TRDT=5）；GRXFSIZ = 0x21b（539）；GNPTXFSIZ = 0x0010021b；DIEPTXF1–6 由对象内的表给出；没有设置 DCFG.DescDMA（即缓冲区 DMA）。
- **与 Linux 的差异（推断出的提速点）**：Linux Apple 参数使用 HBSTLEN_INCR，并且在硬件支持时启用描述符 DMA；RX FIFO 两边相同，都是 539，这与“加大 FIFO 不提速”的实测一致。
- 实现：`linux-dwc2-apple-tuning.patch` 新增启动参数 `dwc2.apple_ahb_burst=` 和 `dwc2.apple_desc_dma=`，默认不改变原有行为；J207 启动包设为 `5` 和 `0`，与 iOS 一致。
- **实测（J207，2026-10-15）**：启动日志为 `apple: ahbcfg HBSTLEN=5, g_dma=1, g_dma_desc=0 (hw desc dma 1)`，可见原先 Linux 用的是描述符 DMA。同一基准（指针在 iPad 上画圈 10 秒）下：iPad 端单次 ≥64K 的读取速率中位数从 18.8 提高到 **41.3 MB/s**；宿主 bulk 平均耗时从 3.6–5.4 ms 降到 0.54 ms，等 USB 的时间占比从 74–81% 降到 19%；10 秒内更新次数从 1466–1974 增加到 3216。gudd、dmesg、宿主 gud 统计都是 0 错误。两个参数各自的贡献没有分开测。
- 未知：ADT 中实际的 `ahb-burst` 值（默认 10 只是代码里的回退值）、DIEPTXF 表的内容、DTHRCTL（这次分析的函数里没有写入）。

- **A1599 同样适用（实测，2026-10-15）**：用同一份源码按 config-v2.4 重新编译 4K 内核（`.scratch/kbuild-v24`，现在是 build_image 的默认内核；`J85M_V24=1` 可退回原 v2.4 二进制），bootargs 设为 `dwc2.apple_ahb_burst=5 dwc2.apple_desc_dma=0`。启动日志为 `HBSTLEN=5, g_dma=1, g_dma_desc=0 (hw desc dma 1)`。mpv testsrc2 下单次 ≥64K 读取速率中位数为 **37.0 MB/s**（原先约 18.8）。背光开机为 2047/2047。

### 帧率实测（J207，全分辨率 1668x2224，mpv 播放 60 fps lavfi 测试源，宿主 ftrace 统计 10 秒）

| 配置 | testsrc2（普通运动画面） | 全屏随机噪声（基本不可压缩） |
|---|---|---|
| INCR + 描述符 DMA，BULK_MAX 512 KiB | — | — |
| INCR8 + 缓冲区 DMA，BULK_MAX 512 KiB | 22.7 帧/秒（每帧提交 19.4 ms，拆 13 块） | 5.1 帧/秒 |
| INCR8 + 缓冲区 DMA，BULK_MAX 4 MiB | **37.4 帧/秒**（每帧 18.0 ms） | 4.9 帧/秒（每帧 158 ms，受带宽限制） |

每帧时间分解（512 KiB 时）：USB bulk 约 70%，控制传输约 16%，宿主 RGB565 转换和 LZ4 约 23%；读取 dmabuf 可以忽略。宿主 gud 的 flush 在原子提交中同步完成，每帧超过 16.7 ms 就会错过 60 Hz 节拍。半分辨率模式（数据量是 1/4）没有单独测试。

## 当前日常配置（2026-10-15，用户确认“非常好”）

- 两台 iPad 同时使用，**每台各插一个电脑自带 USB 口**（xHCI 根口各自有独立的 USB 2.0 链路）。hub `1-9` 的 3 号口（`1-9.3`）对两台 iPad 都反复报 `-71`（descriptor read、setup address 失败），不要使用。两台若挂在同一个 USB 2.0 hub 下，会共享约 40 MB/s 的上行带宽。
- monitors.lua 中下排从左到右为 iPad mini（横屏，transform 1，原生 1536x2048、scale 2，位置 -856x1146）、iPad Pro（横屏，transform 3，原生 1668x2224、scale 2，位置 168x1080）、HDMI-A-1，底边对齐。
- 两台开机亮度都是 100%，USB 都使用 iOS 的 dwc2 配置（INCR8 + 缓冲区 DMA）。
- REA 分析快照：`.scratch/re-otg/kc-12.5.8-j85m.rea-snapshot`（7 条证据），可用 `open_binary` 的 `snapshot_path` 导入，导入时目标文件必须是同一个 padded 文件。

## dwc2 OUT DMA 越界写与内核崩溃（2026-10-16，J85M 实测）

**现象（实测）**：一次 RAM 会话运行约 2 小时 44 分后内核 Oops：`alloc_vmap_area` 里 `list_del` 的 `next` 为 `0xffffffffffffffff`。持有 vmap 锁的进程被杀，之后 `gudd-watchdog` fork 时永久自旋（`kill -9` 无效、RCU stall），`usb-linkwatch` 随之消失，断线自动恢复失效。当时宿主有一条 udev 规则（`99-disable-gud-display.rules`，已停用为 `.disabled-20261016`）让设备约 27 秒重连一次，持续约 30 分钟。

**定位（实测）**：

- `slub_debug=FZP` 开机 2 秒抓到 slab 页开头 72 字节被改写：64×`0xff` + `23 50 00 00 00 00 00 eb`，是 LZ4 长匹配编码，即 GUD bulk OUT 帧数据。
- 给 `usb_f_fs.ko` 的 OUT 缓冲加 1 KiB 哨兵（`gud/usb_f_fs-out-guard.patch`），约 5700 次传输内记录到两类越界：
  - 1~3 字节，679 次：越界量 = (4 − 请求长度 mod 4) mod 4，DMA 按 4 字节字写入。上游 dwc2 只为地址不对齐的缓冲用 bounce buffer，不处理长度。普通 kmalloc 的大小都是 8 的倍数，这类越界不出块，无害。
  - **72 字节，1 次**：`请求 16384` 的整包请求，越界区开头全是 `0xff`，与崩溃时的特征一致。
- 加哨兵后，60 轮快速 + 5 轮慢速断开/重连复现测试中，slub_debug 报错为 0（`.scratch/bundle-tools/churn-test.sh`）。

**推测（未证实）**：gudd 先读整包部分、再单独读尾部短包（见 `gudd.c` bulk_thread）。若尾包在两次请求之间到达，缓冲区 DMA 地址寄存器停在上一缓冲区末尾，尾包就写到了末尾之后。64 KiB 的 kmalloc 按整页分配，越界正好落在下一页开头。尚未确认与 `dwc2.apple_ahb_burst=5 dwc2.apple_desc_dma=0`（缓冲区 DMA）有因果关系，也没在描述符 DMA 下对比过。

**处理**：f_fs 的 OUT 缓冲多分配 1 KiB 吸收越界（越界最多一个包，< 512 字节），释放时检查哨兵。`/sys/module/usb_f_fs/parameters/` 下：`guard_hits` 为全部越界次数，`guard_big` 为超过 3 字节的次数，`guard_max` 为最大越界字节数。超过 3 字节才打日志。**J207 的 `usb_f_fs-16k.ko` 尚未加此补丁**：它用同一份 f_fs 和同样的缓冲区 DMA 配置，大概率同样受影响。

**同批修复**：`gud/modules/hdq3.c` 原先读 BQ27540 时不和从机的下降沿同步，采样点又落在 tDW1 上限 50 µs 处，读数系统性偏低（例：电压 3874 → 实际 3999 mV，循环次数 295 → 423）。现在改为沿同步测宽（实测 1 = 40–42 µs，0 = 110–113 µs，阈值 65 µs），读每个字节期间关中断。`hdq3`、`i2c-probe` 都补了 `cmd`/`out` 的互斥锁和 sysfs 输出一页的上限。

**根因定位（iOS 逆向 + 实机 A/B）**：三次大越界都发生在 USB 刚枚举完时，稳定传输中从未出现，上面"尾包在两次请求之间到达"的推测不成立。iOS 12.5.8 的 AppleSynopsysOTGDevice 使能 OUT 端点时一律设 SNAK（`0x8364e6c`），Linux dwc2 只对 isoc OUT 设。给 dwc2 加了默认关闭的参数 `dwc2.apple_out_snak`（`gadget.c`，源码备份 `gadget.c.orig-20261016`），开启后 16 次枚举（开机 1 + 慢速重连 15）`guard_big=0`；未开启时基线约每 3~5 次枚举出现 1 次。**推测**：不设 SNAK 时主机首批包在请求挂上前就进了 RxFIFO，DMA 用了残留的 DOEPDMA 地址。样本仍小，未做长时间验证。测试镜像为 `.scratch/bundle-tools/a1599-gud-snak.bundle`，逆向笔记在 `.scratch/re-otg-dma/FINDINGS.md`。

**正式镜像（2026-10-16）**：用 `.scratch/bundle-tools/patch_bundle.py` 在原 bundle 上改，记录在 `out/*.patch.json`。
- 第一版（已实机启动）：只替换 initrd 里的 `usb_f_fs.ko`、`hdq3.ko`、`i2c-probe.ko`。J85M `4dc4a7b1…`，J86 `f67914dc…`，保留为 `out/*.pre-snak.bundle`。
- 当前版：再换成带 `linux-dwc2-apple-out-snak.patch` 的内核（`.scratch/kbuild-snak`，Image.gz `524bc4be…`，副本 `out/Image-dwc2-snak.gz`），bootargs 追加 `dwc2.apple_out_snak=1`，f_fs 哨兵保留兜底。
  J85M `34fac101…`（与上面实测的测试镜像逐字节相同），J86 `76366042…`（DTB 仍为 j86 版）。两个启动器的 `BUNDLE_SHA256` 已同步。
  2026-10-16 三台同时实机启动（各一次 DFU 通过）：`apple_out_snak=Y`、`guard_big=0`、无 SLUB/oops 报错。J86 一开始放电约 −800 mA（`0x4C0/0x4C6/0x4CE` 与正在充电的 J85M 同为 `bf/32/32`，换 Hub 口、重写 `0x4C0` 都无效），换线后 +732 mA，**是线材问题**。交叉验证：那根线接 J207 时 D2355 ch8 只有 `0x39`~`0x3f`（Hub 口、主机直连口相同），换好线后回到 `0x8f`~`0x94`（与以前实测的 `0x92±3` 一致）；同时 J86 换上那根线后又回到 −799 mA。两台的充电寄存器始终不变（J207 `0x44e=0x64`，J86 `0x4C0=bf`）。驱动报 Charging 不代表充电电流正常，要看 ch8 或电量计读数。
  J207 `1d191238…`（已实机启动，见上）：基于 `834d7997…`（保留为 `out/a1599-gud-j207.pre-snak.bundle`），换成 `.scratch/kbuild-16k-snak` 的 16K 内核（`864a6086…`，副本 `out/Image-16k-dwc2-snak.gz`）和带哨兵的 16K `usb_f_fs.ko`（`b1f2f0de…`，源码 `.scratch/ffs-guard16`），bootargs 追加 `dwc2.apple_out_snak=1`。
  - **内核源码里的另一处改动**：`drivers/mfd/simple-mfd-i2c.c` 在 10-15 08:43 被加了 `apple,august-pmic` 的实验性 sysfs（备份 `.charging-modified`；原版在 `linux-6831bc7.tar.gz`），比之前实测过的 4K/16K 内核都新。4K SNAK 内核把它链了进去，但 J85M/J86 DTB 里没有该节点，驱动不会 probe。J207 DTB 有 `pmic@3c`（`apple,august-pmic`），会生效，所以 16K 内核是临时换回原版文件编的（编完已恢复修改版和原时间戳）。
  注意：`patch_bundle.py` 默认校验 bundle 里的内核等于 `kbuild-v24` 的，要在当前版上再改，得以 `out/*.pre-snak.bundle` 为输入并带上 `--image`。

## 未完成

- `dwc2.apple_out_snak` 只在 J85M 上做了 16 次枚举的验证，没有长时间测试；J86、J207 只验证了启动，没跑重连测试（见上节）
- 功耗、长时间稳定性没测；J207 全分辨率普通运动画面约 37 帧/秒（宿主 gud 同步 flush 每帧约 18 ms）
- 没有触摸输入
- ~~充电/电池：没有驱动，接电脑使用时电量下降~~ → **2026-10-15 已解决（J85M，实机闭环）**：
  根因是缺 Tristar 驱动导致 PMU 把 USB 口当"未识别充电器"，D2089 输入限流锁在 `0x02`。
  改写寄存器 `0x4C0`（I2C0 0x3c，输入限流）即恢复充电，**已固化进镜像**，冷启动自动生效。
  详见 `.scratch/usb-charge/j85m-charging-diagnosis.md` §十三。
  J86（mini 2 Cellular）走同一份 D2089 代码、`setup-gadget` 已加 `apple,j86` 分支，
  **但这台机器还没实机验证过**。
  J207 侧的对应修复是 D2355 `0x44e`（见「充电」）。
- ~~J86 引导失败~~ → **2026-10-16 已解决（实机闭环）**：m1n1 引导前要求 DTB 的
  `compatible` 以 `"apple," + 小写(ADT target-type)` 开头，而这台 target-type 是 `J86`、
  bundle 里写的却是 `apple,j85m` → m1n1 报
  `ERROR: Kernel found but no devicetree for apple,j86 available.` 后拒绝引导，设备停在
  uartproxy。已生成 `out/a1599-gud-j86.bundle` 和配套启动器，`boot-ipad.sh` 按 `BDID:0C`
  自动选它，**一次 DFU 通过**。DTB 来自内核源码，与 j85m 版逐字节只差 compatible/model
  两个字符串（`s5l8960x-mini3.dtsi` 全文就是 `#include "s5l8960x-mini2.dtsi"`），
  详见 `dtb/README.md`。设备端实测 `model = Apple iPad mini 2 (Cellular)`、
  充电 `0x4C0` 回读 `bf`、电量计 **+278 mA**（正值 = 充电；SOC 已 99%，属恒压收尾）。
- **⚠️ 板名字符串有三个消费方，换 DTB 必须三处同改**（2026-10-16 连踩两次）：
  ① m1n1 的 compatible 检查、② `rootfs/sbin/setup-gadget` 的充电分支、
  ③ `gudd.c` 的 EDID 机型表。两次都是**静默失效** —— 先是不配置充电（`0x4C0` 不动），
  后是 EDID 回空（宿主 `card*-USB-*/edid` 读到 0 字节、Hyprland 里这台的
  `description/make/model` 全空、`desc:` 规则抓不住它）。两处均已修，`gudd.c` 表上方
  也加了联动注释。
  **✅ 均已实机复验**：j86 的 EDID 由 0 → 128 字节，Hyprland 出现
  `Apple Computer Inc iPad mini 2 A1490`。离线回归 `gud/tests/test_edid.c` 确认
  A1599 的 EDID 逐字节未变。
  **三台同时在线实测**（`desc:` 规则可用）：`USB-2` = iPad mini 3 / 1536x2048（`1-4`）、
  `USB-3` = iPad mini 2 / 1536x2048（`1-9.3`）、`USB-4` = iPad Pro 10.5 / 1668x2224（`1-9.1`）。
- **⚠️ 一次只能刷一台**：`checkra1n` 没有设备选择参数（`--help` 全表无 `--ecid`），
  它自己扫 `05ac:1227`。wrapper 的 `require_exactly_one` **是唯一防止刷错机器的东西**，
  不能放宽。两台同时进 DFU 只能"拔一台（DFU 不掉）→ 刷 → 插回 → 刷"。
  `boot-ipad.sh` 遇到多台会列出 ECID 并退出。
- Hyprland 里会残留**已断开**的旧连接器条目（如 `USB-1`，描述同样是 iPad Pro 10.5）。
  它不在活动显示器列表里，但反复插拔后会不会干扰 `desc:` 匹配**没有实测过**。
- **“A1600”这个称呼待确认**：用户此前把这台叫 A1600（mini 3 Cellular），但 m1n1 实测
  target-type=`J86`、Model=`iPad4,5`，内核源码 `s5l8960x-j86.dts` 标注为
  iPad mini 2 Cellular（A1490）。文档以实测为准。
- 两台 mini 同时使用时合成 EDID 完全相同（`edid.inc` 写死 iPad mini 3 / A1599 / 1536x2048），
  Hyprland 的 `desc:` 规则会撞车。目前只靠 USB 产品串区分（j86 那台是
  `iPad mini 2 GUD Display`），**尚未实机验证宿主能否稳定分开两台**。
- 每次 iPad 开机都要进 DFU 并在宿主上启动（checkm8 是 tethered 启动；A7 生态目前
  没有"装到内置存储、开机直接进 Linux"的方案，Hoolock 只在 A11 支持内置存储）
- **`error -71` 枚举失败：不是某个端口特有的** —— hub `1-9.3` 和电脑自带口 `1-4`
  都出现过同一签名（`device descriptor read/64, error -71/-110` → `unable to enumerate`），
  且**重试会自己恢复**（`1-4` 上是第 5 次才成功）。根因未查明，待查
- 直连 USB 口与自供电 hub 口的充电效果**实测相当**（均约 +0.5~0.75 A），
  但端口长期可靠性、以及充到接近满电时的行为都没测
