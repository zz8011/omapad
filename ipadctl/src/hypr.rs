//! 宿主侧：DRM 连接器 ↔ USB 端口的映射、背光亮度和 Hyprland 布局文件生成。
//!
//! **背光必须走宿主侧**（`/sys/class/backlight/card*-USB-*-backlight`，0–100）。
//! 直接写设备端 sysfs 会让 gudd 内部的 `bl_pct` 变陈旧，下一次 DPMS 开关或亮度设置
//! 就会被它覆盖回去。

use crate::config::Config;
use crate::paths::{lua_path, monitors_lua_path};
use crate::model::{Model, MODELS};
use anyhow::{bail, Context, Result};
use serde::Deserialize;
use std::path::{Path, PathBuf};
use std::process::Command;

pub const SYS_DRM: &str = "/sys/class/drm";
pub const SYS_BACKLIGHT: &str = "/sys/class/backlight";

/// 一块 GUD 显示器在宿主侧的完整身份。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Connector {
    /// 驱动名，如 `card3`
    pub card: String,
    /// Hyprland 连接器名，如 `USB-2`。**每次重连都会变，别用它做持久化键。**
    pub name: String,
    /// 它接到哪个 USB 端口，如 `1-9.3`。这是稳定身份。
    pub usb_port: String,
    pub connected: bool,
    /// `/sys/class/drm/<card>-<name>` 原始路径（读 `enabled` 等用）
    pub sysfs: PathBuf,
    /// `/sys/class/backlight/<card>-<name>-backlight`
    pub backlight: Option<PathBuf>,
}

impl Connector {
    /// 桌面是否已在这块屏上点灯（compositor 给它配了 CRTC）。
    /// 这是"桌面生效"判据（DESIGN.md §5 第 3 级），不用解析 hyprctl 输出。
    pub fn enabled(&self) -> bool {
        std::fs::read_to_string(self.sysfs.join("enabled"))
            .map(|s| s.trim() == "enabled")
            .unwrap_or(false)
    }
}

impl Connector {
    /// 写背光。`pct` 是 0–100，按 max_brightness 折算。
    ///
    /// backlight sysfs 实测常为 root:644（2026-10-16），普通用户直写报 EACCES ——
    /// 权限不足时降级走 `sudo -n sh -c echo`（与 DFU wrapper 同一免密授权面）。
    pub fn set_brightness(&self, pct: u8) -> Result<()> {
        let Some(bl) = &self.backlight else {
            bail!("{} 没有背光节点", self.name);
        };
        let max: u32 = std::fs::read_to_string(bl.join("max_brightness"))
            .with_context(|| format!("读 {}", bl.display()))?
            .trim()
            .parse()
            .context("max_brightness 不是数字")?;
        // 向上取整，避免 1% 被算成 0 导致关屏
        let v = (max * u32::from(pct.min(100)) + 99) / 100;
        let target = bl.join("brightness");
        match std::fs::write(&target, v.to_string()) {
            Ok(()) => return Ok(()),
            Err(e) if e.kind() == std::io::ErrorKind::PermissionDenied => {}
            Err(e) => return Err(e).with_context(|| format!("写 {}", target.display())),
        }
        // 降级：sudo -n。v 是程序算出的数字、target 是程序拼的路径，无注入面。
        let out = Command::new("sudo")
            .args(["-n", "sh", "-c", &format!("echo {v} > {}", target.display())])
            .output()
            .with_context(|| format!("sudo 写 {}", target.display()))?;
        if out.status.success() {
            Ok(())
        } else {
            bail!(
                "写 {} 失败：直写 EACCES，sudo 退出码 {:?}",
                target.display(),
                out.status.code()
            )
        }
    }

    pub fn brightness(&self) -> Option<u32> {
        let bl = self.backlight.as_ref()?;
        std::fs::read_to_string(bl.join("brightness"))
            .ok()?
            .trim()
            .parse()
            .ok()
    }

    /// 当前亮度百分比（0–100），无背光节点返回 `None`。
    pub fn brightness_pct(&self) -> Option<u8> {
        let bl = self.backlight.as_ref()?;
        let v = self.brightness()?;
        let max: u32 = std::fs::read_to_string(bl.join("max_brightness"))
            .ok()?
            .trim()
            .parse()
            .ok()?;
        if max == 0 {
            return None;
        }
        Some((v.min(max) * 100 / max) as u8)
    }
}

/// 枚举所有 GUD 连接器（含已断开的，那些是 Hyprland 的幽灵条目）。
pub fn connectors() -> Vec<Connector> {
    let Ok(rd) = std::fs::read_dir(SYS_DRM) else {
        return Vec::new();
    };
    let mut out = Vec::new();
    for e in rd.flatten() {
        let full = e.file_name().to_string_lossy().into_owned(); // card3-USB-2
        let Some((card, name)) = full.split_once('-') else {
            continue;
        };
        if !card.starts_with("card") || !name.starts_with("USB") {
            continue;
        }
        let dir = e.path();
        let Some(usb_port) = usb_port_of(&dir) else {
            continue;
        };
        let connected = std::fs::read_to_string(dir.join("status"))
            .map(|s| s.trim() == "connected")
            .unwrap_or(false);
        let bl = Path::new(SYS_BACKLIGHT).join(format!("{full}-backlight"));
        out.push(Connector {
            card: card.to_owned(),
            name: name.to_owned(),
            usb_port,
            connected,
            sysfs: dir,
            backlight: bl.is_dir().then_some(bl),
        });
    }
    out.sort_by(|a, b| a.usb_port.cmp(&b.usb_port));
    out
}

// ---- hyprctl monitors（画布数据源，P1b） ----

/// `hyprctl -j monitors` 的最小解析，只取画布需要的字段。
/// `availableModes` 是 `"1536x2048@60.00Hz"` 形式的字符串数组
/// （2026-10-16 上机确认的字段形态；`currentMode` 在这版 hyprctl 里不存在）。
#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct HyprMonitor {
    pub id: u32,
    pub name: String,
    /// EDID desc。iPad 用它对上机型（`USB-N` 每次重连都变，desc 才稳定）
    pub description: String,
    pub width: i32,
    pub height: i32,
    pub x: i32,
    pub y: i32,
    #[serde(default)]
    pub scale: f64,
    #[serde(default)]
    pub transform: u8,
    #[serde(default)]
    pub available_modes: Vec<String>,
}

/// 读当前活动显示器。GUI 里 hyprctl 依赖会话环境
/// （`HYPRLAND_INSTANCE_SIGNATURE`）；拿不到时调用方应把画布降级隐藏。
pub fn hyprctl_monitors() -> Result<Vec<HyprMonitor>> {
    let out = Command::new("hyprctl")
        .args(["-j", "monitors"])
        .output()
        .context("执行 hyprctl -j monitors 失败")?;
    if !out.status.success() {
        bail!("hyprctl 退出码非 0 —— 会话环境里可能没有 HYPRLAND_INSTANCE_SIGNATURE");
    }
    serde_json::from_slice(&out.stdout).context("解析 hyprctl monitors JSON 失败")
}

/// hyprctl 显示器属于哪个机型（按 EDID desc 匹配）。
pub fn monitor_model(m: &HyprMonitor) -> Option<&'static Model> {
    MODELS.iter().find(|md| md.desc == m.description)
}

/// 一块显示器占用的**逻辑矩形**：transform 1/3（90°/270°）交换宽高，再除以 scale。
/// 返回 `(x, y, w, h)`。
pub fn logical_rect(x: i32, y: i32, w: i32, h: i32, transform: u8, scale: f64) -> (f64, f64, f64, f64) {
    let s = if scale > 0.0 { scale } else { 1.0 };
    let (lw, lh) = if matches!(transform, 1 | 3) {
        (h as f64, w as f64)
    } else {
        (w as f64, h as f64)
    };
    (x as f64, y as f64, lw / s, lh / s)
}

/// 两个逻辑矩形是否重叠（1 px 容差，贴边不算重叠）。画布松手校验用。
pub fn rects_overlap(a: (f64, f64, f64, f64), b: (f64, f64, f64, f64)) -> bool {
    let (ax, ay, aw, ah) = a;
    let (bx, by, bw, bh) = b;
    ax < bx + bw - 1.0 && bx < ax + aw - 1.0 && ay < by + bh - 1.0 && by < ay + ah - 1.0
}

/// 旋转循环顺序：竖屏 → 横屏(口朝左) → 横屏(口朝右) → 倒屏 → 竖屏。
/// **不是数值 +1**：那会立刻落到 2（倒屏）上（DESIGN.md §4 画布细则）。
pub fn next_transform(t: u8) -> u8 {
    match t {
        0 => 1,
        1 => 3,
        3 => 2,
        _ => 0,
    }
}

/// 在 monitors.lua 末尾追加 `require("hypr.ipadctl")`（幂等；首次写入前做
/// `.ipadctl-bak` 备份）。路径参数版供测试。
pub fn insert_require_into(path: &Path) -> Result<()> {
    let text = std::fs::read_to_string(path).unwrap_or_default();
    if has_require(&text) {
        return Ok(()); // 已经接上，幂等（只认真正的 require 调用）
    }
    if path.exists() {
        let bak = path.with_extension("lua.ipadctl-bak");
        if !bak.exists() {
            std::fs::copy(path, &bak).with_context(|| format!("备份 {}", path.display()))?;
        }
    }
    let mut out = text;
    if !out.is_empty() && !out.ends_with('\n') {
        out.push('\n');
    }
    out.push_str("\n-- omarpad：加载 ipadctl 布局规则（画布排布写到 ipadctl.lua）\nrequire(\"hypr.ipadctl\")\n");
    std::fs::write(path, out).with_context(|| format!("写 {}", path.display()))?;
    Ok(())
}

pub fn insert_require() -> Result<PathBuf> {
    let p = monitors_lua_path()?;
    insert_require_into(&p)?;
    Ok(p)
}

/// 让 Hyprland 重读配置（写完布局文件后调用）。
pub fn reload_hyprland() -> Result<()> {
    let out = Command::new("hyprctl")
        .arg("reload")
        .output()
        .context("执行 hyprctl reload 失败")?;
    if out.status.success() {
        Ok(())
    } else {
        bail!("hyprctl reload 退出码非 0");
    }
}

/// 分级接管判据（DESIGN.md §5）：投放成功只证明 USB 上线，这块屏真正可用
/// 还要 DRM 连接器 connected，桌面真正点灯是 connector 的 `enabled` 属性。
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Takeover {
    /// 只有 USB 设备，没找到 DRM 连接器
    UsbOnly,
    /// 连接器已 connected，但还没有 compositor 接管（`enabled` != "enabled"）
    ConnectorReady(String),
    /// 桌面已在这块屏上点灯
    DesktopActive(String),
}

/// 判定某个 USB 端口上的 GUD 设备到了哪一级。调用方应已确认 USB 上线。
pub fn takeover(port: &str) -> Takeover {
    match connectors()
        .into_iter()
        .find(|c| c.usb_port == port && c.connected)
    {
        None => Takeover::UsbOnly,
        Some(c) if c.enabled() => Takeover::DesktopActive(c.name),
        Some(c) => Takeover::ConnectorReady(c.name),
    }
}

/// 从 `/sys/class/drm/card3-USB-2/device` 的符号链接解析出 USB 端口。
///
/// 链接终止于 `…/usb1/1-9/1-9.1/1-9.1:1.4/drm/card3`。
/// 从后往前找第一个含 `:` 的路径段，冒号前就是端口（`1-9.1`）。
/// **不能靠 `desc:` 定位** —— 幽灵连接器会和真实连接器拿到同一条 desc。
fn usb_port_of(connector_dir: &Path) -> Option<String> {
    let link = std::fs::canonicalize(connector_dir.join("device")).ok()?;
    for seg in link.components().rev() {
        let s = seg.as_os_str().to_string_lossy();
        if let Some((port, _iface)) = s.split_once(':') {
            if port.contains('-') && port.bytes().next()?.is_ascii_digit() {
                return Some(port.to_owned());
            }
        }
    }
    None
}

/// 文本里是否有**真正生效的** `require("hypr.ipadctl")` 调用。
/// 只查子串 `hypr.ipadctl` 会被注释误判（评审 🔴10）。
fn has_require(text: &str) -> bool {
    text.contains("require(\"hypr.ipadctl\")") || text.contains("require('hypr.ipadctl')")
}

/// 「用户的 monitors.lua 里有没有 require 我们」。
pub fn hooked() -> bool {
    let Ok(p) = monitors_lua_path() else {
        return false;
    };
    std::fs::read_to_string(p)
        .map(|t| has_require(&t))
        .unwrap_or(false)
}

/// 从 `availableModes` 里识别（原生档, 半分辨率档）。
/// 不依赖数组顺序：宽度大的是原生、小的是半分辨率（评审 🔵）；
/// 不足两档或无法解析宽度时返回 `None`（UI 不显示分辨率行）。
pub fn split_modes(modes: &[String]) -> Option<(String, String)> {
    if modes.len() < 2 {
        return None;
    }
    let width_of = |s: &str| -> Option<u32> {
        s.split('x').next()?.parse().ok()
    };
    let mut parsed: Vec<(u32, &String)> = modes
        .iter()
        .filter_map(|m| width_of(m).map(|w| (w, m)))
        .collect();
    parsed.sort_by_key(|(w, _)| *w);
    if parsed.len() < 2 {
        return None;
    }
    let half = parsed.first()?.1.clone();
    let native = parsed.last()?.1.clone();
    Some((native, half))
}

/// 生成布局文件正文。
///
/// 只对**当前连着的**机型写规则不合适（设备没插时布局就丢了），所以对机型表里
/// 每一台都写 —— Hyprland 对未连接的 `desc:` 规则只是不生效，没有副作用。
pub fn render_lua(cfg: &Config) -> String {
    let mut s = String::new();
    s.push_str("-- 由 ipadctl 生成，**不要手改**。\n");
    s.push_str("-- 改设置请用 `ipadctl config set ...`，然后 `ipadctl config apply` 重新生成。\n");
    s.push_str("-- 本文件由 ~/.config/hypr/monitors.lua 里的一行 `require(\"hypr.ipadctl\")` 加载。\n");
    s.push_str("--\n");
    s.push_str("-- 为什么按 desc: 匹配而不是 USB-N：接口名每次重连都会变，desc 才稳定。\n");
    s.push_str("-- 但 desc 来自 EDID，所以**同型号两台会撞**；Hyprland 里残留的已断开连接器\n");
    s.push_str("-- 也会匹配同一条 desc。要确认某条规则落在哪块屏上，看\n");
    s.push_str("-- /sys/class/drm/card*-USB-*/device 指向哪个 USB 端口。\n\n");
    for m in MODELS {
        let d = cfg.get(m);
        // mode 两档来自 EDID（None = preferred 原生档）；见 config.rs mode 注释
        let mode = d.mode.as_deref().unwrap_or("preferred");
        s.push_str(&format!("-- {}\n", m.name));
        s.push_str(&format!(
            "hl.monitor({{ output = \"desc:{}\", mode = \"{}\", position = \"{}\", scale = {}, transform = {} }})\n\n",
            m.desc, mode, d.position, d.scale, d.transform
        ));
    }
    s
}

/// 原子写布局文件。
pub fn write_lua(cfg: &Config) -> Result<PathBuf> {
    let p = lua_path()?;
    if let Some(dir) = p.parent() {
        std::fs::create_dir_all(dir).with_context(|| format!("创建 {}", dir.display()))?;
    }
    let text = render_lua(cfg);
    let tmp = p.with_extension("lua.tmp");
    std::fs::write(&tmp, &text).with_context(|| format!("写 {}", tmp.display()))?;
    std::fs::rename(&tmp, &p).with_context(|| format!("替换 {}", p.display()))?;
    Ok(p)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::config::Config;

    #[test]
    fn 能从真实链接里解析出_usb_端口() {
        let cs = connectors();
        assert!(!cs.is_empty(), "本机应当有 GUD 连接器");
        for c in &cs {
            // 端口名应当形如 1-9.3 / 1-4
            assert!(c.usb_port.contains('-'), "异常端口名: {}", c.usb_port);
            assert!(c.name.starts_with("USB"), "异常连接器名: {}", c.name);
            assert!(c.card.starts_with("card"), "异常 card: {}", c.card);
        }
    }

    #[test]
    fn 生成的_lua_含三台机型的规则且可被解析为文本() {
        let cfg = Config::default();
        let lua = render_lua(&cfg);
        for m in MODELS {
            assert!(lua.contains(&format!("desc:{}", m.desc)), "缺 {}", m.slug);
        }
        assert!(lua.contains("transform = "));
        assert!(lua.contains("position = "));
        // 规则行数 = 机型数
        assert_eq!(lua.matches("hl.monitor(").count(), MODELS.len());
    }

    #[test]
    fn 背光写入不超过上限() {
        // 只验证折算公式，不真写设备
        let f = |max: u32, pct: u8| (max * u32::from(pct.min(100)) + 99) / 100;
        assert_eq!(f(100, 0), 0);
        assert_eq!(f(100, 1), 1);
        assert_eq!(f(100, 100), 100);
        assert_eq!(f(2047, 100), 2047);
        assert_eq!(f(2047, 1), 21); // 向上取整，不会变成 0
        assert_eq!(f(2047, 0), 0);
    }

    #[test]
    fn 逻辑矩形_竖屏不交换_横屏交换再除_scale() {
        // 实测案例：USB-3（j86，1536×2048 竖屏面板，transform 3，scale 2）
        // 位于 (-856,1146)，逻辑尺寸应为 1024×768
        assert_eq!(
            logical_rect(-856, 1146, 1536, 2048, 3, 2.0),
            (-856.0, 1146.0, 1024.0, 768.0)
        );
        // transform 0：不交换
        assert_eq!(
            logical_rect(3200, 1080, 1536, 2048, 0, 2.0),
            (3200.0, 1080.0, 768.0, 1024.0)
        );
        // scale 0 视为 1，避免除零
        assert_eq!(logical_rect(0, 0, 100, 200, 0, 0.0).2, 100.0);
    }

    #[test]
    fn 重叠判定_贴边不算重叠() {
        let a = (0.0, 0.0, 100.0, 100.0);
        assert!(rects_overlap(a, (50.0, 50.0, 100.0, 100.0)));
        assert!(!rects_overlap(a, (100.0, 0.0, 100.0, 100.0))); // 恰好贴边
        assert!(!rects_overlap(a, (-500.0, -500.0, 100.0, 100.0))); // 远离
    }

    #[test]
    fn 旋转循环_按固定顺序不落倒屏() {
        // 0→1→3→2→0，全程走完一轮
        assert_eq!(next_transform(0), 1);
        assert_eq!(next_transform(1), 3);
        assert_eq!(next_transform(3), 2);
        assert_eq!(next_transform(2), 0);
    }

    #[test]
    fn 一键修复_追加一行_幂等_带备份() {
        let dir = std::env::temp_dir().join(format!("ipadctl-test-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let p = dir.join("monitors.lua");
        std::fs::write(&p, "hl.monitor({ output = \"DP-1\" })").unwrap();

        insert_require_into(&p).unwrap();
        let t1 = std::fs::read_to_string(&p).unwrap();
        assert!(t1.contains("require(\"hypr.ipadctl\")"));
        assert!(t1.starts_with("hl.monitor(")); // 原内容保留在前
        assert!(dir.join("monitors.lua.ipadctl-bak").is_file()); // 备份在

        // 第二次调用幂等：不再追加
        insert_require_into(&p).unwrap();
        let t2 = std::fs::read_to_string(&p).unwrap();
        assert_eq!(t1, t2);

        // 只在注释里提到 hypr.ipadctl 不算接入，必须追加真正的 require
        let p2 = dir.join("monitors2.lua");
        std::fs::write(&p2, "-- TODO: 以后加 hypr.ipadctl 布局\n").unwrap();
        insert_require_into(&p2).unwrap();
        assert!(std::fs::read_to_string(&p2).unwrap().contains("require(\"hypr.ipadctl\")"));

        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn 模式两档_按宽度识别_不依赖顺序() {
        // 顺序颠倒也能认出半分辨率档
        let a = vec![
            "768x1024@60.01Hz".to_owned(),
            "1536x2048@60.00Hz".to_owned(),
        ];
        assert_eq!(
            split_modes(&a),
            Some(("1536x2048@60.00Hz".into(), "768x1024@60.01Hz".into()))
        );
        let b = vec!["1668x2224@60.00Hz".to_owned(), "834x1112@59.99Hz".to_owned()];
        assert_eq!(
            split_modes(&b),
            Some(("1668x2224@60.00Hz".into(), "834x1112@59.99Hz".into()))
        );
        // 只有一档 / 解析不出宽度 → None（UI 不显示）
        assert_eq!(split_modes(&["1536x2048@60.00Hz".to_owned()]), None);
        assert_eq!(split_modes(&["垃圾".to_owned(), "也垃圾".to_owned()]), None);
    }

    #[test]
    fn 解析实测_hyprctl_json_样本() {
        // 字段形态来自 2026-10-16 实机 hyprctl -j monitors（驼峰 + 模式字符串数组）
        let json = r#"[{
            "id": 4, "name": "USB-1",
            "description": "Apple Computer Inc iPad mini 3 A1599",
            "width": 1536, "height": 2048, "x": 3200, "y": 1080,
            "scale": 2.0, "transform": 0,
            "availableModes": ["1536x2048@60.00Hz", "768x1024@60.01Hz"]
        }]"#;
        let ms: Vec<HyprMonitor> = serde_json::from_str(json).unwrap();
        assert_eq!(ms.len(), 1);
        let m = &ms[0];
        assert_eq!(monitor_model(m).map(|md| md.slug), Some("a1599"));
        assert_eq!(m.available_modes.len(), 2);
        assert_eq!(
            logical_rect(m.x, m.y, m.width, m.height, m.transform, m.scale),
            (3200.0, 1080.0, 768.0, 1024.0)
        );
        // 认不出的显示器（宿主屏）返回 None
        let host = HyprMonitor {
            id: 0,
            name: "DP-1".into(),
            description: "XX 0x1234".into(),
            width: 3840,
            height: 2160,
            x: 0,
            y: 0,
            scale: 2.0,
            transform: 0,
            available_modes: vec![],
        };
        assert!(monitor_model(&host).is_none());
    }

    #[test]
    fn 生成的_lua_带_mode_档位() {
        use crate::config::DeviceSettings;
        let mut cfg = Config::default();
        let mut d = cfg.get(&MODELS[0]); // a1599
        d.mode = Some("768x1024@60.01Hz".into());
        cfg.devices.insert("a1599".into(), d);
        let lua = render_lua(&cfg);
        assert!(lua.contains("mode = \"768x1024@60.01Hz\""), "半分辨率档要写进 lua");
        assert!(lua.contains("mode = \"preferred\""), "未设置的机型仍是 preferred");
        let _: Option<DeviceSettings> = None; // 引用一下类型，避免未使用告警
    }
}
