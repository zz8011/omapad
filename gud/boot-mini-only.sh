#!/bin/sh
# 启动 iPad mini 3 (当有多台设备时)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)

echo "[1/3] 检查 iPad mini 3 (CPID:8960 BDID:0C)..."
MINI_SERIAL=$(for d in /sys/bus/usb/devices/*; do 
  [ "$(cat $d/idVendor 2>/dev/null)" = "05ac" ] && \
  [ "$(cat $d/idProduct 2>/dev/null)" = "1227" ] && \
  cat $d/serial 2>/dev/null | grep "CPID:8960.*BDID:0C" || true
done | head -1)

if [ -z "$MINI_SERIAL" ]; then
  echo "未找到 iPad mini 3 在 DFU 模式"
  exit 1
fi

echo "找到: $MINI_SERIAL"
W="$HERE/launch/alpine_dfu_wrapper.py"

echo "[2/3] checkm8 + PongoOS..."
OUT=$(sudo timeout 240 python3 "$W" --start --ecid $(echo "$MINI_SERIAL" | grep -o "ECID:[0-9A-F]*" | cut -d: -f2) 2>&1) || { echo "$OUT" | tail -5; exit 1; }
S=$(echo "$OUT" | grep -o 'session [0-9TZ-]*[0-9a-f]*' | cut -d' ' -f2)
[ -n "$S" ] || { echo "$OUT" | tail -5; echo "没有拿到 session"; exit 1; }

echo "[3/3] 上传 Linux 并启动（session $S）..."
sudo timeout 240 python3 "$W" --upload "$S" 2>&1 | tail -1

i=0; until lsusb -d 1d50:614d >/dev/null 2>&1; do 
  i=$((i+1)); [ $i -gt 60 ] && { echo "iPad 启动后 60 秒内没有出现 GUD 显示器"; exit 1; }
  sleep 1
done

echo "完成：iPad mini 3 已作为 GUD 显示器连接。"
