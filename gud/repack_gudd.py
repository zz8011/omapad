#!/usr/bin/env python3
"""在已实机启动过的 bundle 上只替换 initrd 里的 usr/sbin/gudd，其余字节不动，并自检。
用法: repack.py 输入.bundle 输出.bundle 新gudd"""
import gzip, hashlib, importlib.util, io, json, sys, zlib
from pathlib import Path
OLD = Path('/home/zz8011/Documents/Codex/2026-10-12/task-2/a1599-alpine-fixed/diagnostic-v2/gadget-fix')
spec = importlib.util.spec_from_file_location('cpio', OLD / 'build_gadget_fix.py')
cpio = importlib.util.module_from_spec(spec); spec.loader.exec_module(cpio)
sha = lambda b: hashlib.sha256(b).hexdigest()
src, dst, new_gudd = Path(sys.argv[1]).read_bytes(), Path(sys.argv[2]), Path(sys.argv[3]).read_bytes()
M1N1 = 1196032
assert src[M1N1:M1N1 + 16] == b'chosen.bootargs='
j = src.index(b'\n', M1N1) + 1
assert src[j:j + 4] == bytes.fromhex('d00dfeed')
k = j + int.from_bytes(src[j + 4:j + 8], 'big')          # DTB 末尾 = Image.gz 起点
assert src[k:k + 2] == b'\x1f\x8b'
z = zlib.decompressobj(16 + zlib.MAX_WBITS); z.decompress(src[k:])  # 跑完 Image.gz
plen = len(src) - len(z.unused_data)
prefix, initrd = src[:plen], src[plen:]
entries = cpio.parse(gzip.decompress(initrd))
assert sum(n == 'usr/sbin/gudd' for n, _, _ in entries) == 1
old_gudd = next(d for n, _, d in entries if n == 'usr/sbin/gudd')
raw = b''.join(cpio.record(n, f, new_gudd if n == 'usr/sbin/gudd' else d) for n, f, d in entries)
out = io.BytesIO()
with gzip.GzipFile(fileobj=out, mode='wb', compresslevel=9, mtime=0, filename='') as g:
    g.write(raw)
new = prefix + out.getvalue()
# 自检：前缀同字节；除 gudd 外每个条目名/元数据/内容都相同
chk = new[:plen] == prefix
e2 = cpio.parse(gzip.decompress(new[plen:]))
assert chk and len(e2) == len(entries)
for (n1, f1, d1), (n2, f2, d2) in zip(entries, e2):
    assert n1 == n2 and f1[:6] == f2[:6] or n1 == 'usr/sbin/gudd', n1
    assert d1 == d2 or n1 == 'usr/sbin/gudd', n1
assert next(d for n, _, d in e2 if n == 'usr/sbin/gudd') == new_gudd
dst.write_bytes(new)
print(json.dumps({'src': sys.argv[1], 'src_sha256': sha(src), 'bundle': str(dst), 'bundle_sha256': sha(new), 'bundle_bytes': len(new),
                  'boot_prefix_sha256': sha(prefix), 'bootargs': src[M1N1:j].decode().strip(), 'entries': len(entries),
                  'usr/sbin/gudd': {'old_sha256': sha(old_gudd), 'new_sha256': sha(new_gudd)}}, indent=2, ensure_ascii=False))
