//! sysfs USB 枚举。
//!
//! 设备的**稳定身份是 sysfs 端口名**（`1-4`、`1-9.3`），不是 devnum（每次重枚举都变）。
//! 这套做法照搬 `gud/boot-ipad.sh` 的 `ports_of()`，但那版是 shell 循环，
//! 这里加上从 `/dev/ttyACM*` 反查端口的能力（常驻程序需要它定位设备）。

use std::path::{Path, PathBuf};

/// 各阶段设备的 VID:PID。
pub const DFU: (&str, &str) = ("05ac", "1227");
pub const PONGO: (&str, &str) = ("05ac", "4141");
/// m1n1 的 uartproxy —— 出现在这里代表**引导失败**（内核没起来）
pub const UARTPROXY: (&str, &str) = ("1209", "316d");
/// 正常跑起来、GUD 已就绪
pub const GUD: (&str, &str) = ("1d50", "614d");
/// GUD 失败时的退路：只有 2×ACM 维护口
pub const ACM_ONLY: (&str, &str) = ("1d6b", "0104");

pub const SYS_USB: &str = "/sys/bus/usb/devices";
pub const SYS_TTY: &str = "/sys/class/tty";

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct UsbDevice {
    /// sysfs 端口名，如 `1-4` / `1-9.3`
    pub port: String,
    pub vid: String,
    pub pid: String,
    pub product: String,
    /// ⚠️ GUD 设备的这个是空的（setup-gadget 不写 serialnumber）
    pub serial: String,
}

fn read_trim(p: &Path) -> Option<String> {
    std::fs::read_to_string(p).ok().map(|s| s.trim().to_owned())
}

/// 端口名形如 `1-4`、`1-9.3`；排除接口目录（`1-4:1.0`）和根 hub（`usb1`）。
fn is_device_dirname(name: &str) -> bool {
    let Some((bus, rest)) = name.split_once('-') else {
        return false;
    };
    !bus.is_empty()
        && bus.bytes().all(|b| b.is_ascii_digit())
        && !rest.is_empty()
        && rest
            .split('.')
            .all(|seg| !seg.is_empty() && seg.bytes().all(|b| b.is_ascii_digit()))
}

pub fn load_all() -> Vec<UsbDevice> {
    load_all_in(Path::new(SYS_USB))
}

pub fn load_all_in(sys_usb: &Path) -> Vec<UsbDevice> {
    let Ok(rd) = std::fs::read_dir(sys_usb) else {
        return Vec::new();
    };
    let mut out = Vec::new();
    for e in rd.flatten() {
        let port = e.file_name().to_string_lossy().into_owned();
        if !is_device_dirname(&port) {
            continue;
        }
        let d = e.path();
        let Some(vid) = read_trim(&d.join("idVendor")) else {
            continue; // 瞬态：设备正在被移除
        };
        out.push(UsbDevice {
            port,
            vid,
            pid: read_trim(&d.join("idProduct")).unwrap_or_default(),
            product: read_trim(&d.join("product")).unwrap_or_default(),
            serial: read_trim(&d.join("serial")).unwrap_or_default(),
        });
    }
    out.sort_by(|a, b| a.port.cmp(&b.port));
    out
}

/// 按 VID:PID 找设备。VID/PID 用十六进制小写字符串比较（sysfs 就是小写）。
pub fn with_id(vid: &str, pid: &str) -> Vec<UsbDevice> {
    load_all()
        .into_iter()
        .filter(|d| d.vid == vid && d.pid == pid)
        .collect()
}

/// 从 `/dev/ttyACM*` 反查它属于哪个 USB 设备和哪个接口。
///
/// `/sys/class/tty/ttyACM2/device` 是指向 `…/1-9.1:1.0` 的符号链接。
/// 返回 `(端口名, 接口号)`，例如 `("1-9.1", "1.0")`。
///
/// ⚠️ 每台设备有两个 ACM：`…:1.0` 是维护 shell（设备端 `/dev/ttyGS0`），
/// `…:1.2` 是第二个。**要用 `1.0` 那个。**
pub fn tty_endpoint(tty: &str) -> Option<(String, String)> {
    let link = std::fs::canonicalize(Path::new(SYS_TTY).join(tty).join("device")).ok()?;
    let iface = link.file_name()?.to_string_lossy().into_owned(); // 形如 1-9.1:1.0
    let (port, num) = iface.split_once(':')?;
    Some((port.to_owned(), num.to_owned()))
}

/// 枚举 `/dev` 下的 ttyACM*，返回 (设备节点, 端口名, 接口号)。
pub fn tty_devices() -> Vec<(PathBuf, String, String)> {
    let Ok(rd) = std::fs::read_dir("/dev") else {
        return Vec::new();
    };
    let mut out: Vec<_> = rd
        .flatten()
        .filter_map(|e| {
            let name = e.file_name().to_string_lossy().into_owned();
            if !name.starts_with("ttyACM") {
                return None;
            }
            let (port, iface) = tty_endpoint(&name)?;
            Some((e.path(), port, iface))
        })
        .collect();
    out.sort_by(|a, b| a.0.cmp(&b.0));
    out
}

/// 找某个 USB 端口对应的维护口（接口 `1.0`）设备节点。
pub fn maintenance_tty(port: &str) -> Option<PathBuf> {
    tty_devices()
        .into_iter()
        .find(|(_, p, i)| p == port && i.ends_with(".0"))
        .map(|(dev, _, _)| dev)
}

/// 差分：`now` 里有哪些端口不在 `before` 中。用于判断「本次新出现的设备」。
///
/// `gud/boot-ipad.sh` 用同样思路，注释说明了动机：多台 iPad 可以同时接着，
/// 必须只把新出现的算作本次成功，否则一台已在线会掩盖另一台的失败。
pub fn new_ports<'a>(before: &[String], now: &'a [UsbDevice]) -> Vec<&'a UsbDevice> {
    now.iter().filter(|d| !before.contains(&d.port)).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn 端口名过滤() {
        assert!(is_device_dirname("1-4"));
        assert!(is_device_dirname("1-9.3"));
        assert!(is_device_dirname("3-1.4.2"));
        assert!(!is_device_dirname("usb1"));
        assert!(!is_device_dirname("1-4:1.0")); // 接口
        assert!(!is_device_dirname("1-"));
        assert!(!is_device_dirname(""));
    }

    #[test]
    fn 能读本机_usb_设备() {
        let all = load_all();
        assert!(!all.is_empty(), "本机应该有 USB 设备");
        // 端口名都应是合法形态
        for d in &all {
            assert!(is_device_dirname(&d.port), "异常端口名: {}", d.port);
        }
    }

    #[test]
    fn 差分逻辑() {
        let before = vec!["1-4".to_owned(), "1-9.3".to_owned()];
        let now = vec![
            UsbDevice { port: "1-4".into(), vid: "1d50".into(), pid: "614d".into(), product: "x".into(), serial: String::new() },
            UsbDevice { port: "1-9.1".into(), vid: "1d50".into(), pid: "614d".into(), product: "y".into(), serial: String::new() },
        ];
        let new = new_ports(&before, &now);
        assert_eq!(new.len(), 1);
        assert_eq!(new[0].port, "1-9.1");
    }
}
