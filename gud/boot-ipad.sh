#!/bin/sh
# 一键启动 iPad 副屏：先把 iPad 手动进入 DFU（屏幕全黑），再运行本脚本。
#
# 用法：
#   sh gud/boot-ipad.sh
#
# 注意：启动器（alpine_dfu_wrapper.py）只接受 --dry-run | --start | --upload SESSION，
# 不接受 --ecid，而且要求**恰好一台** DFU 设备。多台时它自己会报错，
# 本脚本会先把所有 DFU 设备的 ECID 列出来，方便你判断该拔掉哪台。
#
# 为什么上传前要记录已有设备：多台 iPad 可以同时接着，脚本必须只把"新出现的"
# GUD 设备算作本次成功 —— 否则一台已在线就会掩盖另一台的失败
# （2026-10-15 实际踩到：A1600 引导失败停在 m1n1 uartproxy，却因为 Wi-Fi 版
#   仍在线而误报"完成"）。
set -e
HERE=$(cd "$(dirname "$0")" && pwd)

# 列出某 VID:PID 设备所在的 sysfs 端口名（每行一个）
ports_of() {
    for d in /sys/bus/usb/devices/*; do
        [ "$(cat $d/idVendor 2>/dev/null)" = "$1" ] && \
        [ "$(cat $d/idProduct 2>/dev/null)" = "$2" ] && basename "$d"
    done
}
# $1 是否出现在空格分隔列表 $2 中
have() { case " $2 " in *" $1 "*) return 0 ;; *) return 1 ;; esac; }

echo "[1/4] 等待 DFU 设备 (05ac:1227)…"
i=0
while [ -z "$(ports_of 05ac 1227)" ]; do
    i=$((i+1))
    [ $i -gt 60 ] && { echo "60 秒内没有发现 DFU 设备，请确认 iPad 已进入 DFU（屏幕全黑、无苹果 logo）"; exit 1; }
    sleep 1
done

IDS=$(for d in $(ports_of 05ac 1227); do cat /sys/bus/usb/devices/$d/serial 2>/dev/null; echo; done)
N=$(echo "$IDS" | grep -c .)
echo "  发现 $N 台 DFU 设备"

if [ "$N" -gt 1 ]; then
    echo "  有 $N 台在 DFU，启动器只接受恰好一台 —— 请先让其余设备退出 DFU："
    echo "$IDS" | sed 's/^/    /'
    exit 1
fi
SEL="$IDS"

ID=$(echo "$SEL" | grep -o "CPID:[0-9A-Fa-f]* CPRV:[0-9A-Fa-f]* CPFM:[0-9A-Fa-f]* SCEP:[0-9A-Fa-f]* BDID:[0-9A-Fa-f]*")
ECID=$(echo "$SEL" | grep -o "ECID:[0-9A-Fa-f]*" | head -1 | cut -d: -f2)
case "$ID" in
    *CPID:8960*BDID:32*) W="$HERE/launch/alpine_dfu_wrapper.py"; echo "机型：iPad mini 3 Wi-Fi（A1599, J85M）" ;;
    # BDID 0x0C：2026-10-15 实测 m1n1 报 target-type=J86、Model=iPad4,5，对应内核
    # 源码的 s5l8960x-j86.dts（iPad mini 2 Cellular）。此前按 A1600 的标注是猜的，
    # 以实测为准。这台必须用 j86 专用包：m1n1 只认 compatible 以 "apple,j86" 开头的 DTB。
    *CPID:8960*BDID:0C*) W="$HERE/launch/alpine_dfu_wrapper_j86.py"; echo "机型：iPad mini 2 Cellular（J86 / iPad4,5）" ;;
    *CPID:8011*BDID:04*) W="$HERE/launch/alpine_dfu_wrapper_j207.py"; echo "机型：iPad Pro 10.5 Wi-Fi（A1701, J207）" ;;
    *) echo "不支持的设备：$ID"; exit 1 ;;
esac
echo "  ECID: $ECID"

BEFORE_GUD=$(ports_of 1d50 614d | tr '\n' ' ')
BEFORE_PXY=$(ports_of 1209 316d | tr '\n' ' ')
[ -n "$BEFORE_GUD" ] && echo "  注意：已有 GUD 设备在线（端口 $BEFORE_GUD）—— 本次只认新出现的那个"

echo "[2/4] checkm8 + PongoOS…"
OUT=$(sudo timeout 240 python3 "$W" --start 2>&1) || { echo "$OUT" | tail -5; exit 1; }
S=$(echo "$OUT" | grep -o 'session [0-9TZ-]*[0-9a-f]*' | cut -d' ' -f2)
[ -n "$S" ] || { echo "$OUT" | tail -5; echo "没有拿到 session"; exit 1; }

echo "[3/4] 上传 Linux 并启动（session $S）…"
sudo timeout 240 python3 "$W" --upload "$S" 2>&1 | tail -1

echo "[4/4] 等待本台 iPad 上线…"
i=0
while [ $i -lt 90 ]; do
    for p in $(ports_of 1d50 614d); do
        if ! have "$p" "$BEFORE_GUD"; then
            echo "完成：iPad 已作为 GUD 显示器连接（端口 $p），Hyprland 会自动接管。"
            exit 0
        fi
    done
    for p in $(ports_of 1209 316d); do
        if ! have "$p" "$BEFORE_PXY"; then
            echo "失败：iPad 停在 m1n1 uartproxy（端口 $p）——m1n1 起来了但内核没有引导，GUD 未出现。"
            echo "      排查方向：换电脑自带 USB 口重试 / 检查该机型的 DTB 与内核载荷。"
            exit 1
        fi
    done
    i=$((i+1)); sleep 1
done
echo "失败：90 秒内既没出现新的 GUD 显示器，也没出现 m1n1 uartproxy。"
exit 1
