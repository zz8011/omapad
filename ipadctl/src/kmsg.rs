//! 从内核日志里读 USB 枚举失败与掉线。
//!
//! **为什么需要**：设备插着但枚举失败时，sysfs 里什么都没有 —— 工具只会说
//! "没找到设备"，而真正有用的信息（哪个口、什么错误、是不是已经放弃重试）
//! 只在内核日志里。2026-10-16 实测踩到：mini 2 插在 hub 2 号口连续 10 次
//! `error -71`，从 sysfs 完全看不出来。
//!
//! **为什么用 `journalctl -k` 而不是 `dmesg`**：本机 `kernel.dmesg_restrict = 1`，
//! 普通用户读 dmesg 会 `不允许的操作`，但 `journalctl -k` 可以（实测）。
//! 这样工具不必为了看一眼日志就要 root。

use std::process::Command;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Kind {
    /// `device descriptor read/64, error -71` / `device not accepting address`
    EnumerateFailed,
    /// `unable to enumerate USB device` —— 内核已经放弃重试，需要人工干预
    GaveUp,
    /// `USB disconnect`
    Disconnected,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Event {
    /// 归一化后的 sysfs 端口名，如 `1-9.2`
    pub port: String,
    pub kind: Kind,
    /// 原始行（截断显示用）
    pub line: String,
}

/// 读最近 `secs` 秒的内核 USB 事件。读不到就返回空 —— 诊断信息缺失不该让主流程失败。
pub fn recent(secs: u64) -> Vec<Event> {
    let Ok(out) = Command::new("journalctl")
        .args([
            "-k",
            "--no-pager",
            "-n",
            "400",
            "--since",
            &format!("-{secs}s"),
        ])
        .output()
    else {
        return Vec::new();
    };
    parse(&String::from_utf8_lossy(&out.stdout))
}

pub fn parse(text: &str) -> Vec<Event> {
    let mut out = Vec::new();
    for line in text.lines() {
        let Some(rest) = line.split("usb ").nth(1) else {
            continue;
        };
        let Some((dev, msg)) = rest.split_once(": ") else {
            continue;
        };
        let Some(port) = normalize_port(dev) else {
            continue;
        };
        let kind = if msg.contains("unable to enumerate") {
            Kind::GaveUp
        } else if msg.contains("error -71")
            || msg.contains("not accepting address")
            || msg.contains("not responding to setup address")
            || msg.contains("device descriptor read")
        {
            Kind::EnumerateFailed
        } else if msg.contains("USB disconnect") {
            Kind::Disconnected
        } else {
            continue;
        };
        out.push(Event {
            port,
            kind,
            line: msg.trim().to_owned(),
        });
    }
    out
}

/// `1-9.2` → `1-9.2`；`1-9-port2` → `1-9.2`（内核两种写法都出现过）。
fn normalize_port(s: &str) -> Option<String> {
    let s = s.trim();
    if let Some((hub, n)) = s.rsplit_once("-port") {
        if n.bytes().all(|b| b.is_ascii_digit()) && hub.contains('-') {
            return Some(format!("{hub}.{n}"));
        }
        return None;
    }
    let ok = s.split_once('-').is_some_and(|(bus, rest)| {
        bus.bytes().all(|b| b.is_ascii_digit())
            && rest
                .split('.')
                .all(|seg| !seg.is_empty() && seg.bytes().all(|b| b.is_ascii_digit()))
    });
    ok.then(|| s.to_owned())
}

/// 把事件归并成「每个端口当前的问题」，只保留每个端口**最后一条**。
///
/// 只看最后一个是因为要回答"这个口现在怎么了"：一串 `error -71` 后面跟着
/// `unable to enumerate`，结论是内核已经放弃；而末尾是 `USB disconnect` 则说明
/// 设备是被正常拔掉的，不是故障。
pub fn summarize(events: &[Event]) -> Vec<Event> {
    let mut last: Vec<Event> = Vec::new();
    for e in events {
        match last.iter_mut().find(|x| x.port == e.port) {
            Some(slot) => *slot = e.clone(),
            None => last.push(e.clone()),
        }
    }
    last.sort_by(|a, b| a.port.cmp(&b.port));
    last
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn 端口名两种写法都归一化() {
        assert_eq!(normalize_port("1-9.2").as_deref(), Some("1-9.2"));
        assert_eq!(normalize_port("1-4").as_deref(), Some("1-4"));
        assert_eq!(normalize_port("1-9-port2").as_deref(), Some("1-9.2"));
        assert_eq!(normalize_port("usb1"), None);
        assert_eq!(normalize_port("1-9-portX"), None);
    }

    #[test]
    fn 解析实测日志片段() {
        // 2026-10-16 实际抓到的行
        let text = "\
10月 16 01:19:38 omarchy kernel: usb 1-9.2: new high-speed USB device number 88 using xhci_hcd
10月 16 01:19:38 omarchy kernel: usb 1-9.2: Device not responding to setup address.
10月 16 01:19:39 omarchy kernel: usb 1-9.2: device not accepting address 88, error -71
10月 16 01:19:39 omarchy kernel: usb 1-9.2: device descriptor read/64, error -71
10月 16 01:19:41 omarchy kernel: usb 1-9-port2: unable to enumerate USB device
10月 16 01:19:28 omarchy kernel: usb 1-9.3: USB disconnect, device number 73";
        let ev = parse(text);
        // "new high-speed USB device" 不是错误，应被忽略
        assert_eq!(ev.len(), 5);
        assert_eq!(ev[0].port, "1-9.2");
        assert_eq!(ev[0].kind, Kind::EnumerateFailed);
        assert_eq!(ev.last().unwrap().port, "1-9.3");
        assert_eq!(ev.last().unwrap().kind, Kind::Disconnected);

        let sum = summarize(&ev);
        assert_eq!(sum.len(), 2);
        // 1-9.2 的最后一条应当是端口归一化后的 GaveUp
        let p2 = sum.iter().find(|e| e.port == "1-9.2").unwrap();
        assert_eq!(p2.kind, Kind::GaveUp);
    }

    #[test]
    fn 无关日志被忽略() {
        assert!(parse("10月 16 01:00:00 omarchy kernel: usb 1-2: reset high-speed USB device").is_empty());
        assert!(parse("完全不是日志").is_empty());
    }
}
