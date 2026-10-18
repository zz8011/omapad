# J207（iPad Pro 10.5）触摸

链路：Z2 触控芯片 —SPI→ `spi3raw4.ko`（sysfs 文本接口）→ `touchd` 解码 0x76 报告 → `/dev/hidg0`（USB HID 多点触控）→ 宿主 `hid-multitouch` → libinput → Hyprland。

| 文件 | 包内路径 | 说明 |
|---|---|---|
| `touchd.c` | `/sbin/touchd` | 读报告、按 `-c a,b,c,d,e,f` 仿射校准、写 HID。`touchd -D` 输出报告描述符 |
| `touch-init` | `/sbin/touch-init` | 加载 `i2c-probe.ko`、`spi3raw4.ko`，上传触控固件与校准包，守护 `touchd`（读 `/run/touchd.args`） |
| `setup-gadget-j207` | `/sbin/setup-gadget` | **只用于 J207 包**：在 GUD 之外加 `hid.usb0`，HID 任一步失败只去掉 HID。它分叉自较早的通用版，没有 mini 2 产品串和 D2089 充电分支，不能用于 A7 机型 |
| `memtool.c` | `/lib/a1599/touch/memtool` | 物理内存读写小工具（touch-init 用来配 GPIO/电源） |
| `../modules/spi3raw4.c` | `/lib/a1599/touch/spi3raw4.ko` | 16K 内核外部模块 |

**不入库**（在被忽略的 `gud/out/` / `.scratch/` 中）：触控固件 `fw.bin` 与 `mtcal.pkt` / `ffcal.pkt` / `mtdo.pkt`（取自 iOS，苹果版权）、`usb_f_hid.ko`、`i2c-probe.ko`（16K 版）、各二进制。逆向笔记与试验过程在 `.scratch/touch-j207/`。

哈希（2026-10-18 核对）：设备与包内 `touch-init` = 本目录同名文件；`setup-gadget-j207` sha256 `d7b091df…` 与 `a1599-gud-j207-touch2.bundle` 包内一致；`memtool`、`spi3raw4.ko` 与包内一致。
`touchd`：Pro 上运行的是 2026-10-17 03:29 热更新版（md5 `de9aada4…`，比 touch2 包内的 `980a6bcf…` 新），源码即本目录 `touchd.c`。当时用 clang 22.1.8 编译，现工具链已是 23.1.3，按 `-O2 -s` 重编字节不同（字符串表除编译器版本外一致），因此打包沿用实测运行的那份二进制。
