//! ipadctl —— iPad 副屏常驻工具（CLI）。
//!
//! 实现都在本包 lib（`ipadctl::…`）里，GUI 是 crates/gui 的 omapad 包。

use ipadctl::{config, device, flash, hypr, kmsg, model, paths, usb};

use anyhow::{bail, Context, Result};
use clap::{Parser, Subcommand};
use std::path::{Path, PathBuf};

#[derive(Parser)]
#[command(name = "ipadctl", about = "iPad 副屏常驻工具", version)]
struct Cli {
    /// 仓库根目录（包含 `gud/`）。不给就从可执行文件位置往上找。
    #[arg(long, global = true, env = "IPADCTL_ROOT")]
    root: Option<PathBuf>,

    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Subcommand)]
enum Cmd {
    /// 列出所有 iPad 及其所处阶段
    Detect,
    /// 每台已上线设备的机型、端口、维护口、充电限流、电量计电流
    Status,
    /// 在设备上执行一条命令（调试用；命令原样交给设备端 shell，注意引号）
    Exec {
        /// sysfs 端口名，如 `1-9.1`
        port: String,
        /// 命令
        cmd: String,
    },
    /// 把 DFU 里的那台 iPad 推送进 Linux（按 CPID/BDID 自动选机型包）
    Flash {
        /// 等它上线的最长秒数
        #[arg(long, default_value_t = 120)]
        wait: u64,
    },
    /// 每台 iPad 的布局与设置
    Config {
        #[command(subcommand)]
        action: ConfigAction,
    },
}

#[derive(Subcommand)]
enum ConfigAction {
    /// 显示设置，以及它落在哪个连接器上
    Show,
    /// 下发设置（亮度、充电限流）并重新生成 Hyprland 布局文件
    Apply,
    /// 改一项设置（不立即下发，之后跑 apply）
    Set {
        /// 机型 slug：a1599 / j86 / j207
        slug: String,
        /// 键：transform / position / scale / brightness / charge_limit
        key: String,
        value: String,
    },
}

fn main() -> Result<()> {
    let cli = Cli::parse();
    let root = match cli.root {
        Some(r) => r,
        None => paths::project_root()?,
    };
    match cli.cmd {
        Cmd::Detect => detect(&root),
        Cmd::Status => status(),
        Cmd::Exec { port, cmd } => {
            let mut acm = device::open_acm(&port)?;
            let (out, rc) = acm.run(&cmd, std::time::Duration::from_secs(30))?;
            println!("{out}");
            println!("--- 设备端退出码 {rc}");
            Ok(())
        }
        Cmd::Flash { wait } => {
            use flash::FlashEvent;
            let out = flash::flash_with_progress(
                &root,
                std::time::Duration::from_secs(wait),
                None, // CLI 自己刚做完决策，无需绑定端口/机型
                &mut |ev: &FlashEvent| match ev {
                    FlashEvent::Starting {
                        model,
                        dfu_port,
                        wrapper,
                        bundle,
                        bytes,
                        sha,
                    } => {
                        println!("机型   {}", model.name);
                        println!("端口   {dfu_port}（DFU）");
                        println!("启动器 {}", wrapper.display());
                        println!("镜像   {}  {bytes} B  {}", bundle.display(), &sha[..16]);
                        println!();
                    }
                    FlashEvent::ExistingOnline { count, ports } => println!(
                        "注意：已有 {count} 台 GUD 在线（{ports}），本次只认新出现的那个"
                    ),
                    FlashEvent::Step(1) => println!("[1/3] checkm8 + PongoOS…"),
                    FlashEvent::Session(s) => println!("      session {s}"),
                    FlashEvent::Step(2) => println!("[2/3] 上传 Linux 并启动…"),
                    FlashEvent::UploadLine(l) => println!("      {l}"),
                    FlashEvent::Step(3) => println!("[3/3] 等它上线…"),
                    FlashEvent::Online { .. } => {}
                    // Step 的其他取值不存在于当前流程，兜底分支保证穷尽
                    _ => {}
                },
            )?;
            println!("      完成：{} 已在 {} 上线", out.model.name, out.port);
            Ok(())
        }
        Cmd::Config { action } => match action {
            ConfigAction::Show => config_show(),
            ConfigAction::Apply => config_apply(),
            ConfigAction::Set { slug, key, value } => config_set(&slug, &key, &value),
        },
    }
}

fn config_show() -> Result<()> {
    let cfg = config::load()?;
    let conns = hypr::connectors();
    let gud = usb::with_id(usb::GUD.0, usb::GUD.1);
    println!("配置文件 {}\n", paths::config_path()?.display());
    for m in model::MODELS {
        let d = cfg.get(m);
        // 找这台机型当前落在哪个连接器上（按 USB 端口交叉校验，不看 desc）
        let port = gud.iter().find(|u| u.product == m.product).map(|u| u.port.clone());
        let conn = port.as_ref().and_then(|p| conns.iter().find(|c| &c.usb_port == p));
        println!("{}  （{}）", m.name, m.slug);
        println!(
            "    接口    {}",
            match conn {
                Some(c) if c.connected => format!("{}  在 USB 端口 {}", c.name, c.usb_port),
                Some(c) => format!("{}  但已断开", c.name),
                None => "未连接".to_owned(),
            }
        );
        println!(
            "    布局    {}  {}  scale {}",
            d.position,
            d.describe_transform(),
            d.scale
        );
        println!(
            "    背光    {}{}",
            d.brightness,
            conn.and_then(|c| c.brightness())
                .map(|b| format!("   当前实际 {b}%"))
                .unwrap_or_default()
        );
        println!("    限流    {} = {}", m.charge_limit_file, d.charge_limit);
        println!();
    }
    if !hypr::hooked() {
        println!("⚠️ {} 里还没有加载本工具的布局文件。加上这一行就生效：", paths::monitors_lua_path()?.display());
        println!("    require(\"hypr.ipadctl\")");
    }
    Ok(())
}

fn config_apply() -> Result<()> {
    let cfg = config::load()?;
    config::save(&cfg)?; // 首次运行时把默认值落盘，方便用户改
    let gud = usb::with_id(usb::GUD.0, usb::GUD.1);
    let conns = hypr::connectors();

    for c in conns.iter().filter(|c| c.connected) {
        let Some(u) = gud.iter().find(|u| u.port == c.usb_port) else {
            continue;
        };
        let Some(m) = model::from_product(&u.product) else {
            continue;
        };
        let d = cfg.get(m);
        println!("{}  {}（{}）", c.name, m.name, c.usb_port);

        // 背光走宿主侧 —— 直接写设备 sysfs 会被 gudd 的 bl_pct 覆盖回去
        match c.set_brightness(d.brightness) {
            Ok(()) => println!("    背光     → {}%", d.brightness),
            Err(e) => println!("    背光     失败：{e:#}"),
        }

        // 充电限流写 watchdog 的目标文件，最迟 30 s 生效
        match device::open_acm(&c.usb_port) {
            Ok(mut acm) => {
                match device::set_charge_limit(&mut acm, m, &d.charge_limit) {
                    Ok(()) => println!(
                        "    限流     → {}（{}，最迟 30 秒生效）",
                        d.charge_limit, m.charge_limit_file
                    ),
                    Err(e) => println!("    限流     失败：{e:#}"),
                }
            }
            Err(e) => println!("    限流     跳过（维护口打不开：{e:#}）"),
        }
    }

    let p = hypr::write_lua(&cfg)?;
    println!("\n已生成 {}", p.display());
    if hypr::hooked() {
        println!("{} 已经加载它了。跑 `hyprctl reload` 让改动生效。", paths::monitors_lua_path()?.display());
    } else {
        println!("⚠️ 还差一步：在 {} 里加一行", paths::monitors_lua_path()?.display());
        println!("    require(\"hypr.ipadctl\")");
        println!("并**删掉**那里原有的 iPad 规则（hl.monitor 是顺序生效的，重复规则会打架）。");
    }
    Ok(())
}

fn config_set(slug: &str, key: &str, value: &str) -> Result<()> {
    let m = model::by_slug(slug).with_context(|| {
        format!(
            "不认识的机型 slug {slug:?}；可用：{}",
            model::MODELS.iter().map(|m| m.slug).collect::<Vec<_>>().join(" / ")
        )
    })?;
    let mut cfg = config::load()?;
    let mut d = cfg.get(m);
    match key {
        "transform" => d.transform = value.parse().context("transform 应为 0/1/2/3")?,
        "position" => {
            if !value.contains('x') {
                bail!("position 形如 3200x1080");
            }
            d.position = value.to_owned();
        }
        "scale" => d.scale = value.parse().context("scale 应为 1 或 2")?,
        "brightness" => {
            let b: u8 = value.parse().context("brightness 应为 0~100")?;
            if b > 100 {
                bail!("brightness 不能大于 100");
            }
            d.brightness = b;
        }
        "charge_limit" => d.charge_limit = value.to_owned(),
        _ => bail!("不认识的键 {key:?}；可用：transform / position / scale / brightness / charge_limit"),
    }
    cfg.devices.insert(slug.to_owned(), d);
    config::save(&cfg)?;
    println!("已写入 {}。跑 `ipadctl config apply` 让改动生效。", paths::config_path()?.display());
    Ok(())
}

fn status() -> Result<()> {
    let gud = usb::with_id(usb::GUD.0, usb::GUD.1);
    if gud.is_empty() {
        println!("没有已上线的 iPad。");
        return Ok(());
    }
    for d in &gud {
        let Some(m) = model::from_product(&d.product) else {
            println!("{}  ⚠️ 产品串不认识：{:?}", d.port, d.product);
            continue;
        };
        println!("{}  {}", d.port, m.name);
        match device::open_acm(&d.port) {
            Ok(mut acm) => {
                println!("    维护口       {}", acm.path.display());
                let st = device::read_state(&mut acm, m);
                println!("    充电限流     {} = {}", m.charge_limit_file, st.charge_limit);
                println!("    限流说明     {}", m.charge_limit_hint);
                println!("    电量计       {}", st.battery);
            }
            // {:#} 打出 anyhow 的完整错误链 —— 不然只见"打不开"看不到 errno
            Err(e) => println!("    维护口打不开：{e:#}"),
        }
    }
    Ok(())
}

fn detect(root: &Path) -> Result<()> {
    let dfu = usb::with_id(usb::DFU.0, usb::DFU.1);
    let gud = usb::with_id(usb::GUD.0, usb::GUD.1);
    let pxy = usb::with_id(usb::UARTPROXY.0, usb::UARTPROXY.1);

    println!("仓库根目录：{}", root.display());
    println!();

    println!("DFU（等待投放）—— {} 台", dfu.len());
    for d in &dfu {
        match model::from_dfu_serial(&d.serial) {
            Some(m) => {
                let (bundle, bytes, sha) = model::bundle_info(root, m)?;
                println!(
                    "  {}  {}  → {}",
                    d.port,
                    m.name,
                    bundle.file_name().unwrap_or_default().to_string_lossy()
                );
                println!(
                    "         ECID {}  bundle {} B / {}",
                    field(&d.serial, "ECID").unwrap_or_default(),
                    bytes,
                    &sha[..16]
                );
            }
            None => {
                // 不认识的机型必须**显式报出来**并拒绝，不能猜一个包递上去
                println!("  {}  ⚠️ 不支持的设备，拒绝投放", d.port);
                println!("         serial: {}", d.serial);
            }
        }
    }

    println!();
    println!("已上线（GUD）—— {} 台", gud.len());
    for d in &gud {
        match model::from_product(&d.product) {
            Some(m) => {
                let tty = usb::maintenance_tty(&d.port);
                println!("  {}  {}", d.port, m.name);
                println!(
                    "         产品串 {:?}  维护口 {}",
                    d.product,
                    tty.map(|t| t.display().to_string())
                        .unwrap_or_else(|| "（未找到）".into())
                );
            }
            None => println!("  {}  ⚠️ 产品串不认识：{:?}", d.port, d.product),
        }
    }

    if !pxy.is_empty() {
        println!();
        println!("引导失败（停在 m1n1 uartproxy）—— {} 台", pxy.len());
        for d in &pxy {
            println!("  {}  m1n1 起来了但内核没有引导", d.port);
        }
    }

    let acm_only = usb::with_id(usb::ACM_ONLY.0, usb::ACM_ONLY.1);
    if !acm_only.is_empty() {
        println!();
        println!("GUD 失败的退路（只有 2×ACM 维护口）—— {} 台", acm_only.len());
        for d in &acm_only {
            println!("  {}  {}", d.port, d.product);
        }
    }

    // 内核日志里的枚举故障。
    // 设备插着但枚举失败时 **sysfs 里什么都没有** —— 只报"没找到设备"会让人以为是没插好，
    // 真正的原因（哪个口、什么错误、内核是否已放弃）只在内核日志里。
    // 掉线（正常拔除）不算故障，不在这里刷屏。
    let problems: Vec<_> = kmsg::summarize(&kmsg::recent(180))
        .into_iter()
        .filter(|e| e.kind != kmsg::Kind::Disconnected)
        .collect();
    if !problems.is_empty() {
        println!();
        println!("⚠️ 最近 3 分钟内核报的 USB 枚举问题");
        for e in &problems {
            println!(
                "  {}  {}",
                e.port,
                match e.kind {
                    kmsg::Kind::GaveUp => "内核已放弃重试 —— 换口或拔插，重插未必够",
                    kmsg::Kind::EnumerateFailed => "枚举失败 —— 重插通常可恢复",
                    kmsg::Kind::Disconnected => "已断开",
                }
            );
            println!("        {}", e.line);
        }
    }

    Ok(())
}

/// 从 `KEY:VALUE` 串里取 VALUE（只用于显示 ECID）。
fn field(s: &str, key: &str) -> Option<String> {
    s.split_whitespace()
        .find_map(|t| t.strip_prefix(key)?.strip_prefix(':'))
        .map(str::to_owned)
}
