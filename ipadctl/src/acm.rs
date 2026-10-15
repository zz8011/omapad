//! 宿主 → 设备的命令通道，走维护 ACM（设备端 `/dev/ttyGS0` 上的 root shell）。
//!
//! 协议照搬 `gud/a1599sh.py`，就三行：写 `stty -echo; {cmd}; echo __END<tag>_$?__`，
//! 然后在回包里找那个标记。标记里带随机 tag 和 `$?`，所以**能区分「命令没跑完」和
//! 「跑完了但失败」** —— 这是它比"看提示符"可靠的地方。
//!
//! 比 Python 版多做的三件事（常驻程序必须自己补）：
//!   1. 超时抛**可处理的错误**（Python 版是未捕获的 TimeoutError，会打 traceback）
//!   2. 打开前校验设备节点仍然指向预期端口（拔插后 `/dev/ttyACM*` 会重新分配）
//!   3. 非阻塞读 + poll，不会卡死

use anyhow::{bail, Context, Result};
use nix::fcntl::{OFlag};
use nix::poll::{poll, PollFd, PollFlags};
use std::io::{Read, Write};
use std::os::fd::{AsFd, BorrowedFd};
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

pub struct Acm {
    file: std::fs::File,
    pub path: PathBuf,
    pub port: String,
}

impl Acm {
    /// 打开维护口。`expected_port` 是预期的 sysfs 端口名（如 `1-9.1`）——
    /// 不匹配就拒绝打开，避免拔插后把命令发给了另一台 iPad。
    pub fn open(tty: &Path, expected_port: &str) -> Result<Self> {
        let name = tty
            .file_name()
            .context("tty 路径没有文件名")?
            .to_string_lossy()
            .into_owned();
        let (port, iface) = crate::usb::tty_endpoint(&name)
            .with_context(|| format!("{name} 不是 USB 串口设备（或已被拔掉）"))?;
        if port != expected_port {
            bail!("{name} 现在属于端口 {port}，期望 {expected_port} —— 设备可能已被重新枚举");
        }
        if !iface.ends_with(".0") {
            bail!("{name} 是接口 {iface}，维护口应当是 :1.0");
        }

        let fd = nix::fcntl::open(
            tty,
            OFlag::O_RDWR | OFlag::O_NOCTTY | OFlag::O_NONBLOCK,
            nix::sys::stat::Mode::empty(),
        )
        .with_context(|| format!("打不开 {}", tty.display()))?;

        // 原先把 tty 设成裸模式。cfmakeraw 会关掉 ECHO/ICANON 等，
        // 效果与 a1599sh.py 手工清 iflag/oflag/lflag 一致。
        let mut t = nix::sys::termios::tcgetattr(&fd)?;
        nix::sys::termios::cfmakeraw(&mut t);
        nix::sys::termios::tcsetattr(&fd, nix::sys::termios::SetArg::TCSANOW, &t)?;

        Ok(Self {
            file: std::fs::File::from(fd),
            path: tty.to_owned(),
            port: port.to_owned(),
        })
    }

    /// 在设备上执行一条命令，返回 `(输出, 设备端退出码)`。
    ///
    /// `cmd` **原样拼进设备端的 shell 行**，调用方自己负责引号 —— 与 a1599sh.py 一致。
    ///
    /// 超时后退避重试一次：超时通常意味着设备端的行缓冲里有残留把命令行搅乱了，
    /// 而**只清宿主缓冲是不够的**（见 `run_once` 里的空行冲洗）。
    pub fn run(&mut self, cmd: &str, timeout: Duration) -> Result<(String, i32)> {
        match self.run_once(cmd, timeout) {
            Ok(v) => Ok(v),
            Err(first) => {
                let _ = self.write_all(b"\n");
                self.drain(300);
                self.run_once(cmd, timeout)
                    .with_context(|| format!("重试后仍失败；首次错误：{first:#}"))
            }
        }
    }

    fn run_once(&mut self, cmd: &str, timeout: Duration) -> Result<(String, i32)> {
        let tag = random_tag()?;
        let end = format!("__END{tag}_");

        // 冲洗设备端的行缓冲：先送一个空行，让 shell 把可能残留的半截命令结掉。
        // 少了这一步会踩到：上一次被中断的命令留在规范模式缓冲里，和这次拼成
        // 一条乱码命令（实测到过 `/bin/sh: /: Permission denied`）。
        self.write_all(b"\n")?;
        self.drain(120);

        self.write_all(format!("stty -echo; {cmd}; echo {end}$?__\n").as_bytes())?;

        let deadline = Instant::now() + timeout;
        let mut buf = Vec::new();
        loop {
            if let Some(rc) = parse_end(&buf, &end) {
                let text = String::from_utf8_lossy(&buf).into_owned();
                // 丢掉两样东西：含标记的那一行（tty 回显也带标记），
                // 以及设备端 shell 的提示符（实测是 `~ #`，出现时机不定，
                // 会污染按行解析的结果）。
                let out = text
                    .lines()
                    .filter(|l| !l.contains(&end) && l.trim() != "~ #")
                    .collect::<Vec<_>>()
                    .join("\n");
                return Ok((strip_ansi(&out).trim().to_owned(), rc));
            }
            let left = deadline.saturating_duration_since(Instant::now());
            if left.is_zero() {
                bail!(
                    "{} 上执行超时（{} 秒）：{}\n已收到的输出：{}",
                    self.path.display(),
                    timeout.as_secs(),
                    cmd,
                    String::from_utf8_lossy(&buf[..buf.len().min(400)])
                );
            }
            if self.wait_readable(left)? {
                let mut chunk = [0u8; 4096];
                match self.file.read(&mut chunk) {
                    Ok(0) => {}
                    Ok(n) => buf.extend_from_slice(&chunk[..n]),
                    Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {}
                    Err(e) => return Err(e).context("读串口"),
                }
            }
        }
    }

    fn wait_readable(&mut self, timeout: Duration) -> Result<bool> {
        let ms: u16 = timeout.as_millis().min(u16::MAX as u128) as u16;
        let borrowed: BorrowedFd<'_> = self.file.as_fd();
        let mut fds = [PollFd::new(borrowed, PollFlags::POLLIN)];
        Ok(poll(&mut fds, ms)? > 0)
    }

    fn write_all(&mut self, bytes: &[u8]) -> Result<()> {
        self.file.write_all(bytes).context("写串口")?;
        self.file.flush()?;
        Ok(())
    }

    /// 读干净并丢弃，用于发送前的同步。
    fn drain(&mut self, ms: u64) {
        let deadline = Instant::now() + Duration::from_millis(ms);
        let mut chunk = [0u8; 4096];
        while Instant::now() < deadline {
            let left = deadline.saturating_duration_since(Instant::now());
            if !self.wait_readable(left).unwrap_or(false) {
                break;
            }
            match self.file.read(&mut chunk) {
                Ok(0) => break,
                Ok(_) => {}
                Err(_) => break,
            }
        }
    }
}

/// 去掉 ANSI 转义序列。设备端的 shell/getty 会往输出里混进 `ESC[6n`
/// 这类光标查询，不清掉的话每个解析器都要各自处理一遍。
fn strip_ansi(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    let mut it = s.chars();
    while let Some(c) = it.next() {
        if c != '\x1b' {
            out.push(c);
            continue;
        }
        let mut peek = it.clone();
        if peek.next() == Some('[') {
            it.next();
            // ESC [ ... 直到一个字母为止
            for c2 in it.by_ref() {
                if c2.is_ascii_alphabetic() {
                    break;
                }
            }
        }
    }
    out
}

/// 在缓冲里找 `<end><digits>__`，返回设备端退出码。
fn parse_end(buf: &[u8], end: &str) -> Option<i32> {
    let s = String::from_utf8_lossy(buf);
    let i = s.rfind(end)? + end.len();
    let rest = &s[i..];
    let digits: String = rest.chars().take_while(|c| c.is_ascii_digit()).collect();
    if digits.is_empty() || !rest[digits.len()..].starts_with("__") {
        return None;
    }
    digits.parse().ok()
}

/// 4 字节随机数的十六进制表示。读 /dev/urandom —— 只为让标记不会出现在输出里，
/// 不是安全用途，所以不必引 rand crate。
fn random_tag() -> Result<String> {
    let mut b = [0u8; 4];
    std::fs::File::open("/dev/urandom")
        .context("打开 /dev/urandom")?
        .read_exact(&mut b)
        .context("读 /dev/urandom")?;
    Ok(b.iter().map(|x| format!("{x:02x}")).collect())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn 能找到结束标记并取出退出码() {
        let end = "__ENDab12cd34_";
        assert_eq!(parse_end(b"hello\n__ENDab12cd34_0__\n", end), Some(0));
        assert_eq!(parse_end(b"__ENDab12cd34_7__", end), Some(7));
        // 只有前半截时不该误判
        assert_eq!(parse_end(b"__ENDab12cd34_", end), None);
        assert_eq!(parse_end(b"__ENDab12cd34_12", end), None);
        // 没出现标记
        assert_eq!(parse_end(b"nothing here", end), None);
        // 回显行和结果行都在时，应取最后一个
        assert_eq!(parse_end(b"__ENDab12cd34_$?__\nblah\n__ENDab12cd34_3__", end), Some(3));
    }

    #[test]
    fn 随机_tag_是_8_位十六进制且不重复() {
        // 4 字节 → 8 个十六进制字符，与 a1599sh.py 的 secrets.token_hex(4) 一致
        let a = random_tag().unwrap();
        let b = random_tag().unwrap();
        assert_eq!(a.len(), 8);
        assert!(a.chars().all(|c| c.is_ascii_hexdigit()));
        assert_ne!(a, b);
    }

    #[test]
    fn 去掉_ansi_转义但保留正文() {
        assert_eq!(strip_ansi("~ # \x1b[6n"), "~ # ");
        assert_eq!(strip_ansi("\x1b[1;34mls\x1b[0;0m"), "ls");
        assert_eq!(strip_ansi("没有转义"), "没有转义");
        // 半截的 ESC 不应吃掉后面的正文
        assert_eq!(strip_ansi("abc\x1b"), "abc");
    }
}
