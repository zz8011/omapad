#!/usr/bin/env python3
"""通过 A1599 维护 ACM（目标 ttyGS0 root shell）执行命令 / 上传文件。

用法：
  a1599sh.py [-d /dev/ttyACM0] run '命令'
  a1599sh.py [-d /dev/ttyACM0] put 本地文件 目标路径
"""
import argparse, base64, re, hashlib, os, secrets, select, sys, termios, time


def open_tty(dev):
    fd = os.open(dev, os.O_RDWR | os.O_NOCTTY)
    a = termios.tcgetattr(fd)
    a[0] = a[1] = a[3] = 0
    a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    a[6][termios.VMIN] = 0; a[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, a)
    return fd


def drain(fd, t=0.3):
    out = b''
    while select.select([fd], [], [], t)[0]:
        d = os.read(fd, 65536)
        if not d: break
        out += d
    return out


def run(fd, cmd, timeout=60):
    """执行一条命令，返回 (输出, 退出码)。用随机标记界定输出。"""
    tag = secrets.token_hex(4)
    os.write(fd, f"stty -echo; {cmd}; echo __END{tag}_$?__\n".encode())
    buf = b''; end = time.time() + timeout
    pat = re.compile(rb"__END" + tag.encode() + rb"_(\d+)__")
    while time.time() < end:
        if select.select([fd], [], [], 0.5)[0]:
            buf += os.read(fd, 65536)
            m = pat.search(buf)
            if m:
                # 去掉命令回显行
                lines = [l for l in buf[:m.start()].decode(errors='replace').splitlines()
                         if f"__END{tag}_" not in l]
                return '\n'.join(lines).strip(), int(m.group(1))
    raise TimeoutError(buf.decode(errors='replace')[-2000:])


def put(fd, src, dst):
    data = open(src, 'rb').read()
    run(fd, f"rm -f {dst}.b64")
    b = base64.b64encode(data).decode()
    for i in range(0, len(b), 3000):
        run(fd, f"printf %s '{b[i:i+3000]}' >> {dst}.b64")
    out, rc = run(fd, f"base64 -d {dst}.b64 > {dst} && rm {dst}.b64 && sha256sum {dst}")
    want = hashlib.sha256(data).hexdigest()
    if rc or want not in out:
        raise SystemExit(f"上传校验失败: {out}")
    print(f"{dst}: {len(data)} bytes sha256={want}")


def main():
    p = argparse.ArgumentParser()
    p.add_argument('-d', default='/dev/ttyACM0')
    p.add_argument('-t', type=float, default=60)
    sub = p.add_subparsers(dest='op', required=True)
    r = sub.add_parser('run'); r.add_argument('cmd')
    u = sub.add_parser('put'); u.add_argument('src'); u.add_argument('dst')
    a = p.parse_args()
    fd = open_tty(a.d)
    os.write(fd, b"\n"); drain(fd)
    if a.op == 'run':
        out, rc = run(fd, a.cmd, a.t)
        print(out)
        sys.exit(rc)
    put(fd, a.src, a.dst)


if __name__ == '__main__':
    main()
