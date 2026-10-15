#!/usr/bin/env python3
"""基于已实机成功的 gadget-fix RAM bundle 生成 GUD 版 bundle。

只改 rootfs：替换 etc/inittab、sbin/setup-gadget，新增 lib/a1599/usb_f_fs.ko、usr/sbin/gudd。
boot 前缀（m1n1 + 内核 + DT）保持原字节。输出到 gud/out/。
"""
import gzip, hashlib, importlib.util, io, json, stat
from pathlib import Path

HERE = Path(__file__).resolve().parent
OLD = Path('/home/zz8011/Documents/Codex/2026-10-12/task-2/a1599-alpine-fixed/diagnostic-v2/gadget-fix')
BUNDLE_SHA = 'a069bf7743ac234a463e106f6eacce366fd5a89ca8d10a467e1cf93b790bb074'
PREFIX_LEN = 9553530
sha = lambda d: hashlib.sha256(d).hexdigest()

spec = importlib.util.spec_from_file_location('cpio', OLD / 'build_gadget_fix.py')
cpio = importlib.util.module_from_spec(spec); spec.loader.exec_module(cpio)

bundle = (OLD / 'out/a1599-alpine-m1n1-linux-gadget-fix.bundle').read_bytes()
assert sha(bundle) == BUNDLE_SHA
prefix, initrd = bundle[:PREFIX_LEN], bundle[PREFIX_LEN:]

# 机型：默认 A1599（j85m）。j207（iPad Pro 10.5）只替换 prefix 中的 DTB 段，
# m1n1、bootargs、内核 Image 保持原字节（prefix = m1n1 1196032 + bootargs 38 + DTB 19904 + Image.gz）。
import sys
BOARD = sys.argv[1] if len(sys.argv) > 1 else 'j85m'
DTB_OFF, DTB_LEN = 1196032 + 38, 19904
assert prefix[DTB_OFF:DTB_OFF + 4] == bytes.fromhex('d00dfeed') and prefix[DTB_OFF + DTB_LEN:DTB_OFF + DTB_LEN + 2] == b'\x1f\x8b'
FFS_KO = HERE / 'out/usb_f_fs.ko'
D2355_KO = HERE / 'out/d2355-probe-16k.ko'
if BOARD == 'j207':
    # A10X 不支持 4K 页粒度（4K 内核在 head.S 停住、黑屏无输出，实测），改用 16K 页内核。
    K16 = Path('/home/zz8011/Projects/ipad-a1599-linux-display/.scratch/kbuild-16k/arch/arm64/boot')
    dtb = (K16 / 'dts/apple/t8011-j207.dtb').read_bytes()
    image = (K16 / 'Image.gz').read_bytes()
    # 实测：16K 内核能启动，但停在 "PM: genpd: Disabling unused power domains"，
    # 因此保留未使用的电源域和时钟（pd_ignore_unused clk_ignore_unused）。
    # dwc2.apple_rx_fifo：加大 dwc2 RX FIFO（见 linux-dwc2-apple-rx-fifo.patch），突破每微帧约 4 包的上限。
    import os
    # 启动末尾偶发卡在 genpd 之后（与 dwc2 FIFO 参数无关，两次实测）；疑似 A10X Fusion 调频，先关 cpufreq，并开 initcall_debug 定位
    # dwc2.apple_ahb_burst=5 dwc2.apple_desc_dma=0：与 iOS 一致（INCR8 + 缓冲区 DMA），实测单次读取 18.8→41.3 MB/s
    extra = os.environ.get('J207_EXTRA_ARGS', 'cpufreq.off=1 initcall_debug dwc2.apple_ahb_burst=5 dwc2.apple_desc_dma=0')
    args = ("chosen.bootargs=init=/init loglevel=7 pd_ignore_unused clk_ignore_unused " + extra).strip().encode() + b"\n"
    assert prefix[1196032:DTB_OFF] == b"chosen.bootargs=init=/init loglevel=7\n"
    prefix = prefix[:1196032] + args + dtb + image   # m1n1 + bootargs + DTB + Image.gz
    FFS_KO = HERE / 'out/usb_f_fs-16k.ko'

elif BOARD in ('j85m', 'j86'):
    # 默认改用同源码重新编译的 4K 内核（含 linux-dwc2-apple-tuning.patch），并与 J207 一样启用 iOS 的 dwc2 配置；
    # J85M_V24=1 时保留原 v2.4 内核与 bootargs（退路）。
    import os
    if not os.environ.get('J85M_V24'):
        image = Path('/home/zz8011/Projects/ipad-a1599-linux-display/.scratch/kbuild-v24/arch/arm64/boot/Image.gz').read_bytes()
        extra = os.environ.get('J85M_EXTRA_ARGS', 'dwc2.apple_ahb_burst=5 dwc2.apple_desc_dma=0')
        args = ("chosen.bootargs=init=/init loglevel=7 " + extra).strip().encode() + b"\n"
        assert prefix[1196032:DTB_OFF] == b"chosen.bootargs=init=/init loglevel=7\n"
        if BOARD == 'j86':
            # m1n1 用 ADT 的 target-type 小写化当期望的 compatible（src/payload.c:
            # payload_run）。这台机器的 target-type 是 "J86"，DTB 里写的却是
            # "apple,j85m" —— 匹配不上，m1n1 报 "no devicetree for apple,j86
            # available" 并拒绝引导（2026-10-15 实测，见 j85m-charging-diagnosis.md §十四）。
            # 用 gud/dtb/ 里由内核源码编出的 j86 DTB：与 j85m 版逐字节只差 compatible
            # 和 model 两个字符串（s5l8960x-mini3.dtsi 全文就是 #include mini2.dtsi，
            # 硬件描述完全一致），不夹带任何未验证的硬件改动。生成方法见 gud/dtb/README.md。
            dtb = (HERE / 'dtb/s5l8960x-j86.dtb').read_bytes()
            assert dtb.count(b'apple,j86\x00') == 1 and b'apple,j85m' not in dtb
        else:
            dtb = prefix[DTB_OFF:DTB_OFF + DTB_LEN]
        prefix = prefix[:1196032] + args + dtb + image
else:
    raise SystemExit('未知机型: ' + BOARD)
SUFFIX = '' if BOARD == 'j85m' else '-' + BOARD
entries = cpio.parse(gzip.decompress(initrd))
names = {n for n, f, d in entries}

replace = {
    'etc/inittab': (HERE / 'rootfs/etc/inittab').read_bytes(),
    'sbin/setup-gadget': (HERE / 'rootfs/sbin/setup-gadget').read_bytes(),
}
add = [  # (路径, 内容, 模式)
    ('lib/a1599', None, stat.S_IFDIR | 0o755),
    ('lib/a1599/usb_f_fs.ko', FFS_KO.read_bytes(), stat.S_IFREG | 0o644),
    # D2355 充电控制模块：仅 J207 会用（setup-gadget 里按 compatible 判断）。
    # 必须用与当前镜像同一份内核源码构建，否则 vermagic 不匹配无法 insmod。
    ('lib/a1599/d2355-probe.ko', D2355_KO.read_bytes(), stat.S_IFREG | 0o644),
    ('usr/sbin/gudd', (HERE / 'out/gudd').read_bytes(), stat.S_IFREG | 0o755),
    ('sbin/gudd-supervisor', (HERE / 'rootfs/sbin/gudd-supervisor').read_bytes(), stat.S_IFREG | 0o755),
    ('sbin/usb-linkwatch', (HERE / 'rootfs/sbin/usb-linkwatch').read_bytes(), stat.S_IFREG | 0o755),
    ('sbin/gudd-watchdog', (HERE / 'rootfs/sbin/gudd-watchdog').read_bytes(), stat.S_IFREG | 0o755),
    ('sbin/d2355-watchdog', (HERE / 'rootfs/sbin/d2355-watchdog').read_bytes(), stat.S_IFREG | 0o755),
]
if BOARD in ('j85m', 'j86'):
    # D2089 充电控制（I2C0 0x3c / 寄存器 0x4C0）+ BQ27540 电量计读取。
    # 这两个模块用 J85M 的 4K 内核源码构建，vermagic 与 16K 镜像不匹配，只打进 A7 两个机型。
    # 电量计不是充电必需，但它是"到底有没有在充电"的独立判据（见 sbin/battery）。
    add += [
        ('lib/a1599/i2c-probe.ko', (HERE / 'out/i2c-probe.ko').read_bytes(), stat.S_IFREG | 0o644),
        ('lib/a1599/hdq3.ko', (HERE / 'out/hdq3.ko').read_bytes(), stat.S_IFREG | 0o644),
        ('sbin/d2089-watchdog', (HERE / 'rootfs/sbin/d2089-watchdog').read_bytes(), stat.S_IFREG | 0o755),
        ('sbin/battery', (HERE / 'rootfs/sbin/battery').read_bytes(), stat.S_IFREG | 0o755),
    ]
assert set(replace) <= names and not {a[0] for a in add} & names

ino = max(f[0] for n, f, d in entries) + 1
chunks = []
for name, fields, data in entries:
    if name == 'TRAILER!!!':
        tmpl = list(next(f for n, f, d in entries if n == 'sbin/setup-gadget'))
        for path, content, mode in add:
            f = list(tmpl); f[0] = ino; ino += 1; f[1] = mode; f[4] = 2 if content is None else 1
            chunks.append(cpio.record(path, tuple(f), content or b''))
    chunks.append(cpio.record(name, fields, replace.get(name, data)))
raw = b''.join(chunks)
cpio.parse(raw)  # 结构自检

out = io.BytesIO()
with gzip.GzipFile(fileobj=out, mode='wb', compresslevel=9, mtime=0, filename='') as z:
    z.write(raw)
newbundle = prefix + out.getvalue()
(HERE / f'out/a1599-gud{SUFFIX}.bundle').write_bytes(newbundle)
info = {'base_bundle_sha256': BUNDLE_SHA, 'boot_prefix_sha256': sha(prefix),
        'board': BOARD, 'bundle': f'out/a1599-gud{SUFFIX}.bundle', 'bundle_bytes': len(newbundle), 'bundle_sha256': sha(newbundle),
        'replaced': sorted(replace), 'added': [a[0] for a in add],
        'files_sha256': {p: sha(c) for p, c, m in add if c} | {p: sha(c) for p, c in replace.items()}}
(HERE / f'out/IMAGE{SUFFIX}.json').write_text(json.dumps(info, indent=2, ensure_ascii=False) + '\n')
print(json.dumps(info, indent=2, ensure_ascii=False))
