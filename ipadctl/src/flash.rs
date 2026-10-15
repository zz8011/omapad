//! DFU 投放：编排现有的 Python wrapper，**不重写** checkm8/Pongo 那条链路。
//!
//! wrapper 的契约（读源码确认，见 `gud/launch/alpine_dfu_wrapper.py`）：
//!   - `--start` → 成功时 stdout 有两行，session id 混在第一句英文里
//!   - `--upload <session>` → 成功时 stdout 两行，**不含任何 id**，只能看退出码
//!   - 退出码 0 = 成功；**2 = 受控拒绝**（stderr 有 `STOP: ` 前缀）；其他 = 异常
//!   - 每个子命令都要 root
//!
//! 与 `gud/boot-ipad.sh` 相比修正的两点：
//!   1. **`--upload` 的退出码必须检查** —— 原脚本写的是
//!      `sudo ... --upload "$S" 2>&1 | tail -1`，管道的退出码等于 `tail`，恒为 0，
//!      `set -e` 抓不到，失败只能靠后面 90 秒超时兜底。
//!   2. 不做 U 盘式的"解析输出文案"判断，一律看退出码 + 差分端口集合。
//!
//! `--resume-upload` **不实现**：它的门禁读 `pongo_banner_check_result` /
//! `observed_banner` / `dfu_sysfs_name` 这几个事件，而写入方用的是
//! `pongo_live_banner_result` 等另一套名字（grep 确认只读不写），必然失败。
//!
//! P1a 起投放流程**事件化**（`flash_with_progress`）：GUI/CLI 都通过回调消费进度，
//! 库本身不打印。自动投放的决策在 [`auto_decide`]（纯函数，可测试），
//! 前置检查在 [`preflight`]。

use crate::model::{self, Model};
use crate::usb::{self, UsbDevice};
use anyhow::{Context, Result, bail};
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::{Duration, Instant};

/// 投放前的端口快照。多台 iPad 可以同时接着，必须只把**新出现的**算作本次成功 ——
/// 否则一台已在线会掩盖另一台的失败（`gud/boot-ipad.sh` 的注释记了这次踩坑）。
#[derive(Debug, Default, Clone)]
pub struct Snapshot {
    pub gud: Vec<String>,
    pub uartproxy: Vec<String>,
}

pub fn snapshot() -> Snapshot {
    let ports = |id: (&str, &str)| -> Vec<String> {
        usb::with_id(id.0, id.1)
            .into_iter()
            .map(|d| d.port)
            .collect()
    };
    Snapshot {
        gud: ports(usb::GUD),
        uartproxy: ports(usb::UARTPROXY),
    }
}

#[derive(Debug)]
pub struct Outcome {
    pub model: &'static Model,
    pub session: String,
    /// 上线后的 sysfs 端口
    pub port: String,
}

/// 投放过程中的进度事件。GUI 把 `Step`/`Session`/`UploadLine` 映射成状态行，
/// CLI 原样打印（保持 P0 之前的输出文案）。
#[derive(Debug, Clone)]
pub enum FlashEvent {
    /// 预检通过，开始投放：机型、DFU 端口、启动器与镜像信息
    Starting {
        model: &'static Model,
        dfu_port: String,
        wrapper: PathBuf,
        bundle: PathBuf,
        bytes: u64,
        sha: String,
    },
    /// 已有 n 台 GUD 在线（ports 空格分隔），本次只认新出现的那个
    ExistingOnline { count: usize, ports: String },
    /// 步骤开始：1 = checkm8+Pongo，2 = 上传，3 = 等上线
    Step(u8),
    /// `--start` 成功，解析出的 session id
    Session(String),
    /// `--upload` 完成时 wrapper 输出的末行
    UploadLine(String),
    /// 新的 GUD 设备出现在该 sysfs 端口（成功判据第 1 级：USB 上线）
    Online { port: String },
}

/// 完整投放一台：`--start` → `--upload` → 等它上线。
///
/// `expect` = 决策时的 (DFU 端口, 机型)：**预检与执行之间设备可能被换**
/// （拔 A 插 B），执行前必须复核身份一致，不一致立即中止（评审 🔴2）。
/// CLI 传 `None`（它自己刚做完决策，无需绑定）。
///
/// 返回 `Ok(Outcome)` 只代表 **USB 上线**（判据第 1 级）；连接器可用/桌面生效
/// 要用 [`crate::hypr::takeover`] 继续验证（DESIGN.md §5 分级成功判据）。
pub fn flash_with_progress<F: FnMut(&FlashEvent)>(
    root: &Path,
    wait_timeout: Duration,
    expect: Option<(&str, &'static Model)>,
    on: &mut F,
) -> Result<Outcome> {
    let (dfu, m) = sole_dfu_device()?;
    if let Some((pport, pm)) = expect
        && (dfu.port != pport || m.slug != pm.slug)
    {
        bail!(
            "决策与执行之间 DFU 设备变了（决策：{} @ {pport}，实际：{} @ {}）——已中止，不做投放",
            pm.name,
            m.name,
            dfu.port
        );
    }
    let wrapper = model::wrapper_path(root, m)?;

    // 先确认包和清单都在，再动手（哈希的真正校验在 wrapper 内部，见 preflight 注释）
    let (bundle, bytes, sha) = model::bundle_info(root, m)?;
    on(&FlashEvent::Starting {
        model: m,
        dfu_port: dfu.port.clone(),
        wrapper: wrapper.clone(),
        bundle,
        bytes,
        sha: sha.clone(),
    });

    let before = snapshot();
    if !before.gud.is_empty() {
        on(&FlashEvent::ExistingOnline {
            count: before.gud.len(),
            ports: before.gud.join(" "),
        });
    }

    on(&FlashEvent::Step(1));
    let start_out = run_wrapper(&wrapper, &["--start"])?;
    let session = parse_session(&start_out).with_context(|| {
        format!("没能从 --start 的输出里解析出 session id。原始输出：\n{start_out}")
    })?;
    on(&FlashEvent::Session(session.clone()));

    on(&FlashEvent::Step(2));
    // 关键：这里**必须**看退出码，不能管道给 tail
    let upload_out = run_wrapper(&wrapper, &["--upload", &session])?;
    if let Some(last) = upload_out.lines().last() {
        on(&FlashEvent::UploadLine(last.to_owned()));
    }

    on(&FlashEvent::Step(3));
    let port = wait_online(&before, wait_timeout, m)?;
    on(&FlashEvent::Online { port: port.clone() });

    Ok(Outcome {
        model: m,
        session,
        port,
    })
}

/// 自动投放的前置检查（DESIGN.md §5）。
///
/// **如实边界**：`bundle_info` 只确认 bundle 文件在位 + IMAGE*.json 清单可读；
/// 真正的 SHA256 校验发生在 Python wrapper 内部（不符时它以退出码 2 拒绝）。
pub fn preflight(root: &Path, m: &Model) -> Result<()> {
    model::wrapper_path(root, m)?;
    model::bundle_info(root, m)?;
    sudo_noninteractive()
}

/// `sudo -n true`：常驻程序不能卡在密码提示上（与 run_wrapper 的 `-n` 同一前提）。
pub fn sudo_noninteractive() -> Result<()> {
    let out = Command::new("sudo")
        .arg("-n")
        .arg("true")
        .output()
        .context("执行 sudo -n true 失败")?;
    if out.status.success() {
        Ok(())
    } else {
        bail!("sudo 免密不可用（需要 NOPASSWD；是否配置由主人决定）")
    }
}

/// 自动投放决策（纯函数，扫描线程每轮调用）。安全规则全部在这里：
/// 开关关/正忙 → 不动；多台 DFU → 拒绝（checkra1n 无法指定目标）；
/// 机型不认识 → 拒绝；本周期已失败 → 退避。预检由调用方在 `Trigger` 后执行。
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum AutoAction {
    /// 无事可做（无 DFU / 开关关 / 已有投放在跑）
    Sleep,
    /// 多台 DFU：必须人工减到一台
    RefuseMultiple { ports: Vec<String> },
    /// 机型不认识：显式拒绝，绝不递包
    RefuseUnknown { port: String, serial: String },
    /// 本 DFU 出现周期内已失败过，等设备消失或人工重试
    Backoff { port: String },
    /// 可以投放（调用方接着跑 preflight）
    Trigger { port: String },
}

pub fn auto_decide(
    dfu: &[UsbDevice],
    auto_enabled: bool,
    busy: bool,
    failed_ports: &[String],
) -> AutoAction {
    if !auto_enabled || busy || dfu.is_empty() {
        return AutoAction::Sleep;
    }
    if dfu.len() > 1 {
        return AutoAction::RefuseMultiple {
            ports: dfu.iter().map(|d| d.port.clone()).collect(),
        };
    }
    let d = &dfu[0];
    if model::from_dfu_serial(&d.serial).is_none() {
        return AutoAction::RefuseUnknown {
            port: d.port.clone(),
            serial: d.serial.clone(),
        };
    }
    if failed_ports.contains(&d.port) {
        return AutoAction::Backoff {
            port: d.port.clone(),
        };
    }
    AutoAction::Trigger {
        port: d.port.clone(),
    }
}

/// 找当前处于 DFU 的那**唯一**一台。
///
/// 多于一台必须拒绝：`checkra1n` 没有设备选择参数（`--help` 全表无 `--ecid`），
/// 它自己扫 `05ac:1227` 扫到哪个算哪个，所以"恰好一台"是唯一能防刷错机器的条件。
/// 这条与 wrapper 内部的 `require_exactly_one` 呼应，但在这里提前给出可读的提示。
pub fn sole_dfu_device() -> Result<(UsbDevice, &'static Model)> {
    let dfu = usb::with_id(usb::DFU.0, usb::DFU.1);
    match dfu.len() {
        0 => bail!(
            "没有处于 DFU 的 iPad。先按住 Home + 电源直到屏幕全黑，松开电源继续按 Home 约 10 秒。"
        ),
        1 => {}
        n => {
            let list = dfu
                .iter()
                .map(|d| format!("  {}  {}", d.port, d.serial))
                .collect::<Vec<_>>()
                .join("\n");
            bail!(
                "有 {n} 台在 DFU，但 checkra1n 无法指定目标 —— 请先只留一台（拔掉其余的线即可，DFU 不会因拔线退出）：\n{list}"
            );
        }
    }
    let d = dfu.into_iter().next().unwrap();
    let m = model::from_dfu_serial(&d.serial).with_context(|| {
        // 认不出来就拒绝，绝不猜一个包递上去
        format!(
            "不认识的设备（CPID/BDID 不在机型表里），拒绝投放：{}",
            d.serial
        )
    })?;
    Ok((d, m))
}

/// 跑一个 wrapper 子命令，返回 stdout+stderr。退出码语义见模块头注释。
fn run_wrapper(wrapper: &Path, args: &[&str]) -> Result<String> {
    let out = Command::new("sudo")
        // -n：不交互。常驻程序不能卡在密码提示上；需要密码就明确失败。
        .arg("-n")
        .arg("python3")
        .arg(wrapper)
        .args(args)
        .output()
        .with_context(|| format!("执行 sudo python3 {} 失败", wrapper.display()))?;

    let text = format!(
        "{}{}",
        String::from_utf8_lossy(&out.stdout),
        String::from_utf8_lossy(&out.stderr)
    );

    match out.status.code() {
        Some(0) => Ok(text),
        // 2 = wrapper 的受控拒绝，原因是设计内的（缺权限、多台 DFU、哈希不符…）
        Some(2) => {
            let reason = text
                .lines()
                .find_map(|l| l.trim().strip_prefix("STOP: "))
                .unwrap_or("（未给出原因）");
            bail!("启动器拒绝了这次操作：{reason}")
        }
        Some(c) => bail!("启动器异常退出（码 {c}）：\n{}", text.trim()),
        None => bail!("启动器被信号杀掉：\n{}", text.trim()),
    }
}

/// 从 `--start` 的输出里取 session。
/// 形如 `Pongo startup returned successfully; next verify the banner with session 20261015T160541Z-a9ad1d39e5f1.`
pub fn parse_session(text: &str) -> Option<String> {
    for tok in text.split_whitespace() {
        let t = tok.trim_end_matches('.');
        // 注意：这里不能写 `?` —— 那会在第一个不含 `-` 的 token 上直接返回 None
        let Some((stamp, nonce)) = t.split_once('-') else {
            continue;
        };
        let stamp_ok = stamp.len() == 16
            && stamp.ends_with('Z')
            && stamp[..8].bytes().all(|b| b.is_ascii_digit())
            && stamp.as_bytes()[8] == b'T'
            && stamp[9..15].bytes().all(|b| b.is_ascii_digit());
        if stamp_ok && nonce.len() == 12 && nonce.bytes().all(|b| b.is_ascii_hexdigit()) {
            return Some(t.to_owned());
        }
    }
    None
}

/// 等一台**新的** GUD 设备出现；同时把"停在 uartproxy"当作失败提前返回。
///
/// 新端口还要**机型对得上**才认领（评审 🔴3）：等待期间若另一台 iPad 上线，
/// 不能把它当成本次投放的结果。
fn wait_online(before: &Snapshot, timeout: Duration, expected: &Model) -> Result<String> {
    let deadline = Instant::now() + timeout;
    loop {
        for d in usb::with_id(usb::GUD.0, usb::GUD.1) {
            if before.gud.contains(&d.port) {
                continue;
            }
            // 只认领机型一致的上线；同型号两台无法区分（EDID desc 相同，已知限制）
            if model::from_product(&d.product).is_some_and(|m| m.slug == expected.slug) {
                return Ok(d.port);
            }
        }
        for d in usb::with_id(usb::UARTPROXY.0, usb::UARTPROXY.1) {
            if !before.uartproxy.contains(&d.port) {
                bail!(
                    "iPad 停在 m1n1 uartproxy（端口 {}）—— m1n1 起来了但内核没有引导。\n\
                     常见原因：该机型的 DTB 与 m1n1 的 compatible 检查不匹配（见 gud/dtb/README.md）。",
                    d.port
                );
            }
        }
        if Instant::now() >= deadline {
            bail!(
                "{} 秒内既没出现新的 GUD 显示器，也没出现 m1n1 uartproxy。",
                timeout.as_secs()
            );
        }
        std::thread::sleep(Duration::from_millis(500));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn 能解析_session_串() {
        let s = "Pongo startup returned successfully; next verify the banner with session 20261015T160541Z-a9ad1d39e5f1.\nlocal log: /run/…";
        assert_eq!(
            parse_session(s).as_deref(),
            Some("20261015T160541Z-a9ad1d39e5f1")
        );
    }

    #[test]
    fn 输出里没有_session_时返回_none() {
        assert_eq!(parse_session("STOP: something went wrong"), None);
        assert_eq!(parse_session(""), None);
        // 长度不对的不认
        assert_eq!(parse_session("session 2026101T160541Z-a9ad1d39e5f1"), None);
        assert_eq!(parse_session("session 20261015T160541Z-a9ad1d39e5f"), None);
    }

    fn dev(port: &str, serial: &str) -> UsbDevice {
        UsbDevice {
            port: port.into(),
            vid: "05ac".into(),
            pid: "1227".into(),
            product: String::new(),
            serial: serial.into(),
        }
    }

    #[test]
    fn 自动决策覆盖全部安全分支() {
        let ok = "CPID:8960 CPRV:11 CPFM:03 SCEP:01 BDID:0C ECID:000000768C9CDEE4 IBFL:1C";
        // 无 DFU / 开关关 / 正忙 → Sleep
        assert!(matches!(
            auto_decide(&[], true, false, &[]),
            AutoAction::Sleep
        ));
        assert!(matches!(
            auto_decide(&[dev("1-4", ok)], false, false, &[]),
            AutoAction::Sleep
        ));
        assert!(matches!(
            auto_decide(&[dev("1-4", ok)], true, true, &[]),
            AutoAction::Sleep
        ));
        // 多台 DFU → 拒绝并给出端口
        match auto_decide(&[dev("1-4", ok), dev("1-5", ok)], true, false, &[]) {
            AutoAction::RefuseMultiple { ports } => assert_eq!(ports, vec!["1-4", "1-5"]),
            other => panic!("应是 RefuseMultiple，得到 {other:?}"),
        }
        // 机型不认识 → 拒绝
        assert!(matches!(
            auto_decide(&[dev("1-4", "CPID:9999 BDID:01")], true, false, &[]),
            AutoAction::RefuseUnknown { .. }
        ));
        // 本周期已失败 → 退避；别的端口失败不影响这台
        assert!(matches!(
            auto_decide(&[dev("1-4", ok)], true, false, &["1-4".to_owned()]),
            AutoAction::Backoff { .. }
        ));
        match auto_decide(&[dev("1-4", ok)], true, false, &["1-9".to_owned()]) {
            AutoAction::Trigger { port } => assert_eq!(port, "1-4"),
            other => panic!("应是 Trigger，得到 {other:?}"),
        }
    }
}
