#!/bin/busybox sh
# 在已运行的 A1599 Alpine RAM 系统上，把 gadget 从 3×ACM 切换为 2×ACM + GUD 显示。
# 需先 insmod usb_f_fs.ko（内核未内建 FunctionFS 时）。失败则回滚到原 3×ACM。
# 用法：setsid gud-switch.sh /run/gudd [gudd 参数...] </dev/null >/run/gud-switch.log 2>&1 &
GUDD=${1:-/run/gudd}; shift 2>/dev/null
cfg=/sys/kernel/config/usb_gadget/a1599
ffs=/dev/ffs-gud
log() { echo "[gud-switch] $*"; }
udc=$(ls /sys/class/udc | head -1)

rollback() {
    log "回滚: $1"
    echo "" > $cfg/UDC 2>/dev/null
    rm -f $cfg/configs/c.1/ffs.gud
    [ -L $cfg/configs/c.1/acm.usb2 ] || ln -s $cfg/functions/acm.usb2 $cfg/configs/c.1/acm.usb2
    echo 0x1d6b > $cfg/idVendor; echo 0x0104 > $cfg/idProduct
    echo "A1599 Local USB Services" > $cfg/strings/0x409/product
    echo $udc > $cfg/UDC && log "已回滚到 3xACM"
    exit 1
}

sleep 1   # 让发起命令的 shell 先把输出送回宿主

echo "" > $cfg/UDC || { log "解绑 UDC 失败"; exit 1; }
log "UDC 已解绑"
rm -f $cfg/configs/c.1/acm.usb2
[ -d $cfg/functions/ffs.gud ] || mkdir $cfg/functions/ffs.gud || rollback "创建 ffs.gud"
mkdir -p $ffs
grep -q " $ffs " /proc/mounts || mount -t functionfs gud $ffs || rollback "挂载 functionfs"

killall gudd 2>/dev/null; sleep 0.3
"$GUDD" "$@" $ffs >/run/gudd.log 2>&1 &
for i in 1 2 3 4 5 6 7 8 9 10; do [ -e $ffs/ep1 ] && break; sleep 0.3; done
[ -e $ffs/ep1 ] || rollback "gudd 未写入描述符"

echo 0x1d50 > $cfg/idVendor; echo 0x614d > $cfg/idProduct
echo "A1599 GUD Display" > $cfg/strings/0x409/product
ln -s $cfg/functions/ffs.gud $cfg/configs/c.1/ffs.gud || rollback "链接 ffs.gud"
echo $udc > $cfg/UDC || rollback "绑定 UDC"
log "已绑定: 2xACM + GUD"
