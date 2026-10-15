//! 机型表：把「一台 iPad」从两条路径都能认出来。
//!
//! - **DFU 阶段**用 `CPID`/`BDID`（DFU 描述符里就有，无需握手）
//! - **上线后**用 `1d50:614d` 的 USB 产品串 —— GUD 设备的 `serial` 是**空的**
//!   （`gud/rootfs/sbin/setup-gadget` 从不写 `serialnumber`），产品串是唯一字段
//!
//! ⚠️ 产品串由 `setup-gadget` 按设备树第一个 compatible 决定。板名字符串有**三个消费方**
//! （m1n1 的 compatible 检查 / setup-gadget 的充电分支 / gudd.c 的 EDID 表），
//! 改动时必须三处同改。详见 `gud/README.md`。

use anyhow::{bail, Result};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Model {
    /// 给人看的名字
    pub name: &'static str,
    /// 短名，用于配置文件的键和托盘菜单
    pub slug: &'static str,
    /// DFU 里报的 CPID（SoC）
    pub cpid: u16,
    /// DFU 里报的 BDID（主板）
    pub bdid: u16,
    /// 上线后 `1d50:614d` 的 USB 产品串
    pub product: &'static str,
    /// 投放用的 wrapper 文件名（相对 `gud/launch/`）。
    /// **不能拷贝 wrapper** —— 它按 `Path(__file__).resolve()` 找 bundle。
    pub wrapper: &'static str,
    /// 设备端充电限流的运行时文件；两个 watchdog 各自每 30 s 读一次。
    /// 是 tmpfs，重启即失效。
    pub charge_limit_file: &'static str,
    /// 上面那个文件的默认值（十六进制，不带 `0x`）
    pub charge_limit_default: &'static str,
    /// 充电限流的单位说明，给人看的
    pub charge_limit_hint: &'static str,
    /// 面板原始分辨率
    pub panel: (u32, u32),
    /// 画布上的短标签（全名太长画不下）
    pub short: &'static str,
    /// Hyprland 里用来匹配这块屏的 `desc:` 串。
    ///
    /// **接口名 `USB-N` 每次重连都会变，只有 desc 稳定**，所以布局规则必须按它匹配。
    /// 构成为 `"Apple Computer Inc" + EDID 名(0xFC) + EDID 序号(0xFF)`：
    /// 后两段来自 `gud/gudd.c` 的 `edid_init()` 机型表，第一段是 Hyprland 对 EDID
    /// 厂商 PNP ID `APP` 的映射。**改 gudd.c 那张表就得同步这里。**
    ///
    /// ⚠️ desc 来自 EDID，所以**同型号两台会撞**；而且 Hyprland 会残留已断开连接器的
    /// 条目，那些幽灵也会匹配同一条 desc。定位真实连接器要交叉校验
    /// `/sys/class/drm/card*-USB-*/device` 指到哪个 USB 端口。
    pub desc: &'static str,
}

pub const MODELS: &[Model] = &[
    Model {
        name: "iPad mini 3 Wi-Fi（A1599 / J85M）",
        slug: "a1599",
        cpid: 0x8960,
        bdid: 0x32,
        product: "A1599 GUD Display",
        wrapper: "alpine_dfu_wrapper.py",
        charge_limit_file: "/run/d2089-limit",
        charge_limit_default: "bf",
        charge_limit_hint: "D2089 0x4C0，约 12~20 mA/码；bf≈+650~750 mA，7f≈+206 mA",
        panel: (1536, 2048),
        short: "mini 3",
        desc: "Apple Computer Inc iPad mini 3 A1599",
    },
    Model {
        name: "iPad mini 2 Cellular（J86 / iPad4,5）",
        slug: "j86",
        cpid: 0x8960,
        bdid: 0x0C,
        product: "iPad mini 2 GUD Display",
        wrapper: "alpine_dfu_wrapper_j86.py",
        charge_limit_file: "/run/d2089-limit",
        charge_limit_default: "bf",
        charge_limit_hint: "同 A1599：D2089 0x4C0，bf≈+650~750 mA",
        panel: (1536, 2048),
        short: "mini 2",
        desc: "Apple Computer Inc iPad mini 2 A1490",
    },
    Model {
        name: "iPad Pro 10.5（A1701 / J207）",
        slug: "j207",
        cpid: 0x8011,
        bdid: 0x04,
        product: "iPad Pro 10.5 GUD Display",
        wrapper: "alpine_dfu_wrapper_j207.py",
        charge_limit_file: "/run/d2355-limit",
        charge_limit_default: "64",
        charge_limit_hint: "D2355 0x44e，单位 10 mA；64=1000 mA，32=500 mA",
        panel: (1668, 2224),
        short: "Pro 10.5",
        desc: "Apple Computer Inc iPad Pro 10.5 A1701",
    },
];

/// 从 DFU 的 serial 串里认出机型。
///
/// 实际形态（大小写按设备报的原样）：
/// `CPID:8960 CPRV:11 CPFM:03 SCEP:01 BDID:0C ECID:000000768C9CDEE4 IBFL:1C SRTG:[iBoot-1704.10]`
pub fn from_dfu_serial(serial: &str) -> Option<&'static Model> {
    let cpid = field(serial, "CPID")?;
    let bdid = field(serial, "BDID")?;
    MODELS.iter().find(|m| m.cpid == cpid && m.bdid == bdid)
}

/// 从上线后的 USB 产品串认出机型。
pub fn from_product(product: &str) -> Option<&'static Model> {
    MODELS.iter().find(|m| m.product == product)
}

pub fn by_slug(slug: &str) -> Option<&'static Model> {
    MODELS.iter().find(|m| m.slug == slug)
}

/// 取 `KEY:HEX` 的值。字段之间用空格分隔。
fn field(s: &str, key: &str) -> Option<u16> {
    s.split_whitespace()
        .find_map(|tok| tok.strip_prefix(key)?.strip_prefix(':'))
        .and_then(|hex| u16::from_str_radix(hex, 16).ok())
}

/// 把 wrapper 的名字解析成绝对路径，并确认它和对应的 bundle 都在。
///
/// `project_root` 是仓库根目录（含 `gud/`）。
pub fn wrapper_path(project_root: &std::path::Path, model: &Model) -> Result<std::path::PathBuf> {
    let w = project_root.join("gud/launch").join(model.wrapper);
    if !w.is_file() {
        bail!("找不到启动器 {}", w.display());
    }
    Ok(w)
}

/// 从 `gud/out/IMAGE*.json` 读该机型 bundle 的权威哈希/大小。
///
/// **不要抄文档里的哈希** —— bundle 会被重建，HANDOFF.md 里记的已经过期。
/// 这里的 JSON 由 `gud/build_image.py` 每次构建时写出。
pub fn bundle_info(
    project_root: &std::path::Path,
    model: &Model,
) -> Result<(std::path::PathBuf, u64, String)> {
    let info = project_root
        .join("gud/out")
        .join(match model.slug {
            "a1599" => "IMAGE.json".to_owned(),
            s => format!("IMAGE-{s}.json"),
        });
    let text = std::fs::read_to_string(&info)
        .map_err(|e| anyhow::anyhow!("读不到 {}: {e}（先跑 gud/build_image.py）", info.display()))?;
    let v: serde_json::Value = serde_json::from_str(&text)?;
    let rel = v["bundle"].as_str().unwrap_or_default();
    let bytes = v["bundle_bytes"].as_u64().unwrap_or(0);
    let sha = v["bundle_sha256"].as_str().unwrap_or_default().to_owned();
    let path = project_root.join("gud").join(rel);
    if !path.is_file() {
        bail!("找不到 bundle {}", path.display());
    }
    Ok((path, bytes, sha))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn 三种机型的_dfu_串都能认出() {
        for m in MODELS {
            let s = format!(
                "CPID:{:04X} CPRV:11 CPFM:03 SCEP:01 BDID:{:02X} ECID:000000768C9CDEE4 IBFL:1C",
                m.cpid, m.bdid
            );
            assert_eq!(from_dfu_serial(&s).map(|x| x.slug), Some(m.slug), "串: {s}");
        }
    }

    #[test]
    fn 实测样本能认成_j86() {
        // 2026-10-15 实测原串（见 .scratch/usb-charge/j85m-charging-diagnosis.md）
        let s = "CPID:8960 CPVR:11 CPFM:03 SCEP:01 BDID:0C ECID:000000768C9CDEE4 IBFL:1C SRTG:[iBoot-1704.10]";
        assert_eq!(from_dfu_serial(s).map(|m| m.slug), Some("j86"));
    }

    #[test]
    fn 值大小写都认_键故意只认大写() {
        // 值：设备报 `BDID:0C`，也可能报小写 —— 两种都认
        assert_eq!(from_dfu_serial("CPID:8960 BDID:0c").map(|m| m.slug), Some("j86"));
        assert_eq!(from_dfu_serial("CPID:8011 BDID:4").map(|m| m.slug), Some("j207"));
        // 带 0x 前缀不认（sysfs 里不带）
        assert_eq!(from_dfu_serial("CPID:8960 BDID:0xC").map(|m| m.slug), None);
        // 键：实测设备报大写。小写键**故意不认** —— 认不出来就拒绝，
        // 比按模糊匹配递一个包上去安全。
        assert_eq!(from_dfu_serial("cpid:8011 bdid:04").map(|m| m.slug), None);
    }

    #[test]
    fn 不认识的_bdid_返回_none() {
        // 同 SoC 但别的板子 —— 必须拒绝，不能猜
        assert!(from_dfu_serial("CPID:8960 BDID:99").is_none());
        assert!(from_dfu_serial("CPID:9999 BDID:0C").is_none());
        assert!(from_dfu_serial("").is_none());
        assert!(from_dfu_serial("完全不是这个格式").is_none());
    }

    #[test]
    fn 产品串认得出来且互不相同() {
        for m in MODELS {
            assert_eq!(from_product(m.product).map(|x| x.slug), Some(m.slug));
        }
        assert!(from_product("iPad mini 2 GUD Display").unwrap().cpid == 0x8960);
        assert!(from_product("别的设备").is_none());
    }
}
