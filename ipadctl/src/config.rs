//! 每台 iPad 的持久化设置。存在 `~/.config/ipadctl/config.toml`。
//!
//! 键是机型的 `slug`（`a1599` / `j86` / `j207`），不用 USB 端口 ——
//! 端口名和接口名都会随重新插拔变化，机型才是稳定身份。

use crate::model::{MODELS, Model};
use crate::paths::config_path;
use anyhow::{Context, Result};
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct DeviceSettings {
    /// 0 = 竖屏（充电口朝下）；1 = 横屏、充电口朝左；3 = 横屏、充电口朝右
    pub transform: u8,
    /// Hyprland 逻辑坐标，如 `3200x1080`
    pub position: String,
    /// Hyprland 缩放，常用 1 或 2（旧配置里的整数照常读）。设备端只提供两档模式：
    /// 原生，或半分辨率（设备端 2 倍放大）；用半分辨率时要配 `scale = 1` 才能保持同样的逻辑尺寸。
    pub scale: f64,
    /// 宿主侧 `card*-USB-*-backlight`，0–100
    pub brightness: u8,
    /// 设备端 `/run/*-limit` 的十六进制值
    pub charge_limit: String,
    /// 显示模式串（EDID 的两档之一，如 `768x1024@60.01Hz`）。
    /// `None` = `preferred`（原生档）。2026-10-16 上机验证：三台 iPad 的 EDID
    /// 均只暴露原生 + 半分辨率两档（见 DESIGN.md §12 P1b）。
    #[serde(default)]
    pub mode: Option<String>,
    /// 触摸屏幕是否启用（仅 J207 Pro 支持）
    #[serde(default)]
    pub touch_enabled: bool,
    /// 自定义触摸校准参数（`a b c d e f` 格式，对应 u = a·x + b·y + c, v = d·x + e·y + f）。
    /// `None` 使用固件默认校准。
    #[serde(default)]
    pub touch_calibration: Option<String>,
}

impl DeviceSettings {
    pub fn describe_transform(&self) -> &'static str {
        match self.transform {
            0 => "竖屏（充电口朝下）",
            1 => "横屏（充电口朝左）",
            3 => "横屏（充电口朝右）",
            2 => "倒屏",
            _ => "翻转变体",
        }
    }
}

/// 全局设置（GUI 的「自动副屏」开关等）。文件里缺这一节时用默认值；
/// `#[serde(default)]` 允许**节内字段也缺省**——只写 `auto_connect = false`
/// 而没写 `flash_wait_secs` 的配置必须照常解析（评审 🔴1）。
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(default)]
pub struct Settings {
    /// 识别到 DFU 设备时自动投放 Linux 并接入桌面（DESIGN.md §5）
    pub auto_connect: bool,
    /// 投放后等它上线的最长秒数
    pub flash_wait_secs: u64,
}

impl Default for Settings {
    fn default() -> Self {
        Settings {
            auto_connect: true,
            flash_wait_secs: 120,
        }
    }
}

/// 宿主自带显示器（eDP/DP/HDMI…）在画布上拖过之后的布局，键是 Hyprland 接口名。
/// mode/scale/transform 取拖动时的现值原样写回，只为让覆盖规则不改动它们。
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct HostMonitor {
    pub position: String,
    pub mode: String,
    pub scale: f64,
    pub transform: u8,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct Config {
    #[serde(default)]
    pub settings: Settings,
    /// 接口名 → 宿主屏布局。没拖过的屏不在这里，沿用用户 monitors.lua 的规则
    #[serde(default)]
    pub hosts: BTreeMap<String, HostMonitor>,
    /// 机型 slug → 设置
    #[serde(flatten)]
    pub devices: BTreeMap<String, DeviceSettings>,
}

/// 2026-10-16 实测可用的一套布局,作为首次生成时的默认值。
///
/// 排布：下半排从左到右 = mini 2 → iPad Pro → HDMI-A-1，mini 3 在 HDMI-A-1 右边。
fn defaults_for(m: &Model) -> DeviceSettings {
    let (transform, position) = match m.slug {
        // mini 2：横屏、充电口朝右，排在 Pro 左边，底边与 Pro 对齐
        "j86" => (3, "-856x1146"),
        // Pro：横屏、充电口朝右，紧贴 HDMI-A-1 左边
        "j207" => (3, "168x1080"),
        // mini 3：竖屏，紧贴 HDMI-A-1 右边
        _ => (0, "3200x1080"),
    };
    let touch_enabled = m.slug == "j207"; // 默认为 Pro 开启触摸
    DeviceSettings {
        transform,
        position: position.to_owned(),
        scale: 2.0,
        brightness: 100,
        charge_limit: m.charge_limit_default.to_owned(),
        mode: None,
        touch_enabled,
        touch_calibration: None,
    }
}

/// 读配置；不存在就生成一份默认的（不落盘，由调用方决定是否 save）。
pub fn load() -> Result<Config> {
    let p = config_path()?;
    if !p.is_file() {
        return Ok(Config {
            settings: Settings::default(),
            hosts: BTreeMap::new(),
            devices: MODELS
                .iter()
                .map(|m| (m.slug.to_owned(), defaults_for(m)))
                .collect(),
        });
    }
    let text = std::fs::read_to_string(&p).with_context(|| format!("读 {}", p.display()))?;
    let cfg: Config = toml::from_str(&text).with_context(|| format!("解析 {}", p.display()))?;
    Ok(cfg)
}

pub fn save(cfg: &Config) -> Result<()> {
    let p = config_path()?;
    if let Some(dir) = p.parent() {
        std::fs::create_dir_all(dir).with_context(|| format!("创建 {}", dir.display()))?;
    }
    let text = toml::to_string_pretty(cfg)?;
    // 原子替换：读者（CLI/扫描线程）不会看到半个文件（评审 🟡）
    let tmp = p.with_extension("toml.tmp");
    std::fs::write(&tmp, &text).with_context(|| format!("写 {}", tmp.display()))?;
    std::fs::rename(&tmp, &p).with_context(|| format!("替换 {}", p.display()))?;
    Ok(())
}

impl Config {
    /// 取某机型的设置；配置里缺这一项就补默认值，不会失败。
    pub fn get(&self, m: &Model) -> DeviceSettings {
        self.devices
            .get(m.slug)
            .cloned()
            .unwrap_or_else(|| defaults_for(m))
    }

    /// 可变取某机型设置；**缺项时插入默认值**再返回可变引用。
    /// 直接 `devices.get_mut` 会在旧配置缺该机型时静默丢改动（评审 🔴8）。
    pub fn entry_or_default(&mut self, m: &Model) -> &mut DeviceSettings {
        self.devices
            .entry(m.slug.to_owned())
            .or_insert_with(|| defaults_for(m))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn 默认配置能往返序列化() {
        let cfg = Config {
            settings: Settings::default(),
            hosts: BTreeMap::new(),
            devices: MODELS
                .iter()
                .map(|m| (m.slug.to_owned(), defaults_for(m)))
                .collect(),
        };
        let text = toml::to_string_pretty(&cfg).unwrap();
        let back: Config = toml::from_str(&text).unwrap();
        assert_eq!(cfg.settings, back.settings, "settings 往返不一致");
        for m in MODELS {
            assert_eq!(cfg.get(m), back.get(m), "机型 {} 往返不一致", m.slug);
        }
    }

    #[test]
    fn 旧配置缺_settings_节时回落默认() {
        // CLI 时代写出的 config.toml 没有全局节 —— 必须照常解析
        let old = r#"
[a1599]
transform = 0
position = "3200x1080"
scale = 2
brightness = 100
charge_limit = "bf"
"#;
        let cfg: Config = toml::from_str(old).unwrap();
        assert!(cfg.settings.auto_connect);
        assert_eq!(cfg.settings.flash_wait_secs, 120);
        assert_eq!(cfg.devices.len(), 1);
    }

    #[test]
    fn settings_随保存落盘() {
        let mut cfg = Config::default();
        cfg.settings.auto_connect = false;
        cfg.settings.flash_wait_secs = 60;
        let back: Config = toml::from_str(&toml::to_string_pretty(&cfg).unwrap()).unwrap();
        assert!(!back.settings.auto_connect);
        assert_eq!(back.settings.flash_wait_secs, 60);
    }

    #[test]
    fn hosts_与机型设置同存且互不吞并() {
        let mut cfg = Config::default();
        cfg.entry_or_default(&MODELS[0]);
        cfg.hosts.insert(
            "DP-1".into(),
            HostMonitor {
                position: "1280x0".into(),
                mode: "3840x2160@60.00".into(),
                scale: 2.0,
                transform: 0,
            },
        );
        let back: Config = toml::from_str(&toml::to_string_pretty(&cfg).unwrap()).unwrap();
        assert_eq!(back.hosts, cfg.hosts);
        assert_eq!(back.devices.len(), 1, "hosts 不应被当成机型");
    }

    #[test]
    fn 缺项时回落到默认而不是报错() {
        let cfg: Config = toml::from_str("").unwrap();
        for m in MODELS {
            assert_eq!(cfg.get(m), defaults_for(m));
        }
    }

    #[test]
    fn transform_描述对得上() {
        let mut s = defaults_for(&MODELS[0]);
        s.transform = 3;
        assert_eq!(s.describe_transform(), "横屏（充电口朝右）");
        s.transform = 0;
        assert_eq!(s.describe_transform(), "竖屏（充电口朝下）");
    }
}
