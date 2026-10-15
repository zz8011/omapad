# gud/dtb/ — 各机型 DTB（构建输入）

这两份 DTB 由内核源码的 `arch/arm64/boot/dts/apple/` 编出，`build_image.py` 在
`BOARD=j86` 时读 `s5l8960x-j86.dtb`；`j85m` 仍沿用原始 bundle 里自带的 DTB
（已核对与 `s5l8960x-j85m.dtb` 逐字节相同）。

## 为什么需要单独一份 j86 DTB

m1n1 在引导前会检查 DTB 的 `compatible` 是否以 `"apple," + 小写(ADT target-type)`
开头（`src/payload.c: payload_run`）。这台机器的 target-type 是 `J86`，而 bundle
里的 DTB 写的是 `apple,j85m`，匹配失败：

```
ERROR: Kernel found but no devicetree for apple,j86 available.
```

于是 m1n1 起来后拒绝引导内核，设备停在 uartproxy。见
`.scratch/usb-charge/j85m-charging-diagnosis.md` §十四。

## j86 与 j85m 的实际差异

`arch/arm64/boot/dts/apple/s5l8960x-mini3.dtsi` 全文只有一句有效内容：

```dts
/*
 * The Mini 3 seems to be only an iteration over the Mini 2 with some
 * small changes, like the introduction of Touch ID, hence there is little
 * to no differentiation between these 2 for now.
 */
#include "s5l8960x-mini2.dtsi"
```

所以 `s5l8960x-j86.dts`（→ mini2.dtsi）和 `s5l8960x-j85m.dts`（→ mini3.dtsi → mini2.dtsi）
描述的是同一棵树。两版编出的 DTB 等长（19904 字节），**逐字节只在 `compatible` 和
`model` 两个字符串处不同**（实测差异落在结构块偏移 139–228 区间内）。也就是说换这份
DTB 不会引入任何未经实测的硬件描述改动。

内核侧也没有任何按板名 `apple,j85m`/`apple,j86` 的特判，驱动一律靠 `apple,s5l8960x`
绑定。

## 重新生成

需要内核源码树（当前为 `~/Documents/Codex/2026-10-11/task-2/.build/a1599/src/linux`）。
`dtc` 直接吃 `#include` 会报语法错误，必须先过 C 预处理器：

```sh
SRC=~/Documents/Codex/2026-10-11/task-2/.build/a1599/src/linux
for b in j85m j86; do
    cpp -nostdinc -undef -D__DTS__ -x assembler-with-cpp \
        -I "$SRC/arch/arm64/boot/dts/apple" -I "$SRC/include" \
        "$SRC/arch/arm64/boot/dts/apple/s5l8960x-$b.dts" \
      | dtc -I dts -O dtb -o "gud/dtb/s5l8960x-$b.dtb" -
done
```

自检：重编出的 `s5l8960x-j85m.dtb` 应与
`archive/boot/kernel-config/s5l8960x-j85m-v2.4.dtb` 的 SHA-256 完全相同
（`23a55a0cac3e543c…`）—— 对得上才说明源码树版本与现用镜像一致。

## 实测状态

| 文件 | 来源 | SHA-256（前 16 位） | 状态 |
|---|---|---|---|
| `s5l8960x-j85m.dtb` | 内核源码重编 | `23a55a0cac3e543c` | 与 bundle 内 DTB 逐字节相同（离线核对） |
| `s5l8960x-j86.dtb` | 内核源码重编 | `8e08d68be5a48dc9` | ✅ 2026-10-16 实机引导成功 |

✅ **实机验证（2026-10-16）**：`boot-ipad.sh` 一次通过。设备端读回
`/proc/device-tree/compatible` = `apple,j86` / `apple,s5l8960x` / `apple,arm-platform`，
`model` = `Apple iPad mini 2 (Cellular)`，GUD 副屏正常上线（宿主 `USB-3`，1536x2048@60）。
即 §15.3 那段"与 j85m 版只差两个字符串"的推断成立，换 DTB 没有引入硬件描述改动。
