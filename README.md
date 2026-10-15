<p align="center">
  <img src="branding/omarpad-horizontal-dark.svg" alt="Omarpad — iPad USB Linux 副屏" width="640">
</p>

# Omarpad

通过 USB 将旧 iPad 变成 Linux 桌面的扩展显示器。

当前主线采用 **GUD（Generic USB Display）**：iPad 在 RAM 中运行 Linux，通过 USB 接收画面并显示；宿主使用 Linux `gud` 驱动创建 DRM 显示接口，桌面环境将它作为普通显示器使用。项目主要在 Omarchy / Hyprland 上开发和验证。

> 项目处于活跃硬件试验阶段。下述实机结果来自 2026-10-15 至 2026-10-16 的项目记录，本次 README 整理仅核对文件与文档。完整操作和排查说明见 [GUD 文档](gud/README.md)，历史与证据边界见 [交接文档](HANDOFF.md)。

## 当前进展

已有实机记录确认：iPad 显示 Hyprland 桌面、窗口跨屏移动、背光调节、随宿主熄屏关闭背光，以及 USB 断线后的自动恢复。2026-10-16 的记录包含三台 iPad 同时在线。

| 设备 | 硬件标识 | 原生分辨率 | 已有验证 |
|---|---|---|---|
| iPad mini 3 Wi-Fi（A1599） | A7 / J85M | 1536 × 2048 | Linux RAM 启动、GUD 桌面显示、背光、断线恢复、充电修复 |
| iPad mini 2 Cellular（A1490） | A7 / J86 / iPad4,5 | 1536 × 2048 | 专用 DTB 启动、机型 EDID、充电电流回读、三屏同时在线 |
| iPad Pro 10.5 Wi-Fi（A1701） | A10X / J207 | 1668 × 2224 | 16 KiB 页内核启动、GUD 桌面显示、背光、USB 传输调优、充电寄存器与 ADC 试验 |

机型识别以 DFU 和设备树记录为依据。历史材料中曾将 J86 称为“A1600”，当前记录使用实测的 J86 / iPad4,5 标识。

## 工作方式

```text
Linux 宿主桌面
    ↓ DRM / gud 驱动
USB：RGB565 画面，可使用 LZ4 压缩
    ↓ FunctionFS
 iPad：gudd → 显示缓冲区 → 屏幕
```

GUD 路线直接传输像素，省去视频编码与解码环节。iPad 端 `gudd` 负责协议处理、画面更新和背光控制，配套服务负责 USB 链路恢复。

历史性能记录中，J207 在原生分辨率下播放 60 fps 测试源，宿主提交统计为普通运动画面约 **37.4 帧/秒**、全屏随机噪声约 **4.9 帧/秒**。结果依赖画面压缩率和 USB 带宽；这些数字对应特定测试配置，光学端到端延迟仍待测量。测试条件见 [GUD 性能记录](gud/README.md#帧率实测j207全分辨率-1668x2224mpv-播放-60-fps-lavfi-测试源宿主-ftrace-统计-10-秒)。

## 启动使用

### 准备条件

- 支持的 iPad、可传数据的 USB 线，以及 Linux 宿主。
- 宿主可用的 `gud` 内核驱动；使用 Hyprland 时，需要项目记录中的 aquamarine 补丁配置。
- 已准备好的对应机型 RAM 启动包、checkm8 / PongoOS 启动依赖，以及 Python 3、`sudo`、`timeout` 等脚本依赖。

**当前仓库仍依赖本机旧工作目录中的内核、工具链和启动材料。** 克隆仓库后需要先核对本地路径与依赖；完整清单见 [外部引用](manifests/EXTERNAL-REFERENCES.json)，GUD 构建和部署说明见 [gud/README.md](gud/README.md)。

### 启动步骤

1. 将一台目标 iPad 手动置于 DFU 模式（屏幕全黑），连接宿主。
2. 在仓库根目录运行：

   ```sh
   sh gud/boot-ipad.sh
   ```

3. 脚本按 DFU 硬件标识选择启动包，执行 checkm8 / PongoOS、上传 Linux，并等待新 GUD 设备枚举。
4. 已配置补丁的 Hyprland 接管显示器后，设置屏幕位置、缩放和方向。显示器匹配建议使用 EDID 的 `desc:`，USB 连接器编号会随重新连接变化。

**每次启动时仅保留一台设备处于 DFU 模式。** 启动器要求恰好一台 DFU 设备，多台时脚本会列出设备并退出。已运行的 GUD 显示器可以保持连接。

Linux 运行于 RAM；iPad 重新开机后需要再次进入 DFU 并由宿主启动。已有记录显示，运行期间拔插 USB 或重启宿主可由链路恢复服务重新接管，恢复行为与 iPad 自身重新开机分别处理。

## 已知限制

- **宿主适配**：Hyprland 的实测配置使用补丁版 aquamarine。系统库升级后需重新核对兼容性；其他桌面环境尚缺本项目的验证记录。
- **USB 稳定性**：部分连接出现过 `-71` / `-110` 枚举错误，重试曾恢复，根因仍待定位。多台设备共用 USB 2.0 hub 时共享上行带宽。
- **连接器残留**：断开后 aquamarine 可能保留旧输出条目，并反复尝试 modeset；历史处理方式为重新登录。
- **方向与输入**：屏幕方向通过手动配置；自动旋转和触摸输入尚未实现。
- **充电与功耗**：mini 系列已有充电修复记录；J207 已完成寄存器与 ADC 试验，自动配置的冷启动验证和物理单位标定仍需补齐。各机型的长期电量趋势、温度和功耗仍需测量。
- **启动与长期运行**：J207 曾出现偶发启动停顿，原因未完全定位。长期稳定性与完整延迟验收尚未完成。

更详细的现象、修复过程和验证范围见 [GUD 文档](gud/README.md)。该文档保留了按时间追加的记录，阅读时需结合后续更新判断状态。

## 目录导航

| 路径 | 内容 |
|---|---|
| [gud/](gud/README.md) | 当前副屏主线：设备端守护进程、RAM 镜像组装、启动脚本、宿主与内核补丁 |
| [branding/](branding/README.md) | Logo、应用图标及深浅色版本 |
| [gud/dtb/](gud/dtb/README.md) | 机型设备树与 J86 适配说明 |
| [HANDOFF.md](HANDOFF.md) | 项目历史、实机证据、外部依赖和后续问题 |
| [archive/](archive/) | 历史启动、软件副屏和硬解源码快照 |
| [docs/](docs/) | 历史说明、逆向分析、传输复核与审核记录 |
| [evidence/](evidence/) | 启动、显示、单帧硬解等试验的日志与原始结果 |
| [manifests/](manifests/) | 归档文件校验、来源与外部引用清单 |

## 历史硬解研究

项目早期探索过 JPEG、软件 H.264 副屏与 A7 硬件解码。固定 **32 × 32 H.264 IDR 单帧**已有像素对照记录：NV12 可见区域 **1536 / 1536 字节一致**。后续 consumer 观察结果见 [consumer 实机记录](evidence/consumer-6bbd-20261015/RESULT.md)。

这项验证的范围是固定单帧。连续解码、通用 H.264 支持、DMA 最后访问，以及缓冲区安全复用和释放仍需闭合证据。当前 GUD 副屏独立于这条硬解研究路线运行。

原始单帧结果见 [结果 JSON](evidence/single-frame-1211/FINAL-RESULT.json) 和 [像素比较](evidence/single-frame-1211/pixel-comparison.json)。历史源码与文档保持原样，其中的绝对路径、执行命令和阶段性结论需要结合当前环境核对。
