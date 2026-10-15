//! omarpad —— P1b：画布排列（拖拽/旋转/重叠校验/重载回读）+ monitors.lua
//! 一键接入横幅 + 亮度滑条（实时生效、松手持久化、热插拔套默认）+ 分辨率两档。
//!
//! 线程纪律（DESIGN.md §3）：主线程独占 Slint；扫描线程只做快操作 + 决策；
//! 投放/布局生效（hyprctl reload + 回读）在工作线程跑。配置读改写经
//! `config_mu` 串行化，且**不与 Shared 锁嵌套**（防死锁）。
//!
//! `--measure` 模式禁用自动投放（内存测量绝不刷机器）。

slint::include_modules!();

use flash::AutoAction;
use ipadctl::flash::{self, FlashEvent};
use ipadctl::hypr::Connector;
use ipadctl::{config, hypr, model, paths, usb};
use slint::{Brush, Color, ComponentHandle, ModelRc, VecModel, Weak};
use std::collections::{HashMap, HashSet};
use std::path::PathBuf;
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::{Duration, Instant};

// 语义色（Recordian 皮肤，DESIGN.md §8）：橄榄=在线、琥珀=进行中、
// 洋红=失败/警示（同时是品牌强调色）、灰绿=退路
const OK: (u8, u8, u8) = (0xb8, 0xc7, 0x7a);
const PENDING: (u8, u8, u8) = (0xf9, 0xe2, 0xaf);
const FAIL: (u8, u8, u8) = (0xff, 0x2e, 0x88);
const FALLBACK: (u8, u8, u8) = (0x9b, 0xa8, 0x9a);

fn brush(rgb: (u8, u8, u8)) -> Brush {
    Brush::SolidColor(Color::from_rgb_u8(rgb.0, rgb.1, rgb.2))
}

/// 中毒也不死锁：工作线程 panic 时不能拖死扫描/UI。
fn lock<T>(m: &Mutex<T>) -> MutexGuard<'_, T> {
    m.lock().unwrap_or_else(|e| e.into_inner())
}

struct FailedAttempt {
    reason: String,
    retryable: bool,
}

struct FlashRun {
    port: String,
    step: String,
}

/// 画布里一块显示器的逻辑矩形（含 EDID 两档模式串）。
#[derive(Clone)]
struct CanvasEntry {
    id: String,
    label: String,
    is_ipad: bool,
    x: f64,
    y: f64,
    w: f64,
    h: f64,
    transform: u8,
}

#[derive(Default)]
struct Shared {
    auto_connect: bool, // 启动时从配置读
    flash: Option<FlashRun>,
    failures: HashMap<String, FailedAttempt>,
    // P1b
    canvas: Vec<CanvasEntry>,
    canvas_ok: bool,
    /// slug → 连接器（亮度写宿主侧 sysfs 的路径）
    connectors: HashMap<String, Connector>,
    /// slug → (原生档, 半分辨率档)，来自 EDID availableModes
    modes: HashMap<String, (String, String)>,
    /// 已见过（套过默认亮度）的 GUD 端口；热插拔自动套默认的簿记
    seen_gud: HashSet<String>,
    /// 亮度滑条节流：slug → (上次写入时刻, 上次百分比)
    brightness_last: HashMap<String, (Instant, u8)>,
    /// 松手回显：slug → (提交时刻, 百分比)。5 s 内优先于 sysfs 读数，
    /// 让滑条松手后立刻对齐（扫描下一拍才会看到 sysfs 的新值）。
    bright_echo: HashMap<String, (Instant, u8)>,
    /// 配置文件读改写串行锁（独立于 Shared 自身的锁，不嵌套）
    config_mu: Arc<Mutex<()>>,
    /// 布局操作全串行锁：校验→存→写 lua→reload→回读一气呵成，
    /// 防止快速连续拖动/旋转/切档交错生效（评审 🔴6）
    layout_mu: Arc<Mutex<()>>,
    /// 首轮扫描基线是否已记（基线设备不套默认亮度，之后的新端口才套）
    baseline_done: bool,
}

enum TrayCommand {
    Toggle,
    Quit,
}

struct Tray {
    tx: std::sync::mpsc::Sender<TrayCommand>,
}

impl ksni::Tray for Tray {
    fn id(&self) -> String {
        "omarpad".into()
    }
    fn title(&self) -> String {
        "Omarpad".into()
    }
    fn icon_name(&self) -> String {
        "video-display".into()
    }

    fn activate(&mut self, _x: i32, _y: i32) {
        let _ = self.tx.send(TrayCommand::Toggle);
    }

    fn menu(&self) -> Vec<ksni::MenuItem<Self>> {
        use ksni::menu::StandardItem;
        vec![
            StandardItem {
                label: "打开 / 隐藏窗口".into(),
                activate: Box::new(|t: &mut Tray| {
                    let _ = t.tx.send(TrayCommand::Toggle);
                }),
                ..Default::default()
            }
            .into(),
            ksni::MenuItem::Separator,
            StandardItem {
                label: "退出".into(),
                activate: Box::new(|t: &mut Tray| {
                    let _ = t.tx.send(TrayCommand::Quit);
                }),
                ..Default::default()
            }
            .into(),
        ]
    }
}

struct ScanView {
    dfu: Vec<usb::UsbDevice>,
    gud: Vec<usb::UsbDevice>,
    uart: Vec<usb::UsbDevice>,
    acm: Vec<usb::UsbDevice>,
}

fn scan_all() -> ScanView {
    let grab = |id: (&str, &str)| usb::with_id(id.0, id.1);
    ScanView {
        dfu: grab(usb::DFU),
        gud: grab(usb::GUD),
        uart: grab(usb::UARTPROXY),
        acm: grab(usb::ACM_ONLY),
    }
}

/// 配置读改写（串行、独立锁）。回调里改单个机型或 settings。
fn modify_config(shared: &Arc<Mutex<Shared>>, f: impl FnOnce(&mut config::Config)) -> anyhow::Result<()> {
    let mu = lock(shared).config_mu.clone();
    let _g = lock(&mu);
    let mut cfg = config::load()?;
    f(&mut cfg);
    config::save(&cfg)
}

fn main() -> anyhow::Result<()> {
    let measure = std::env::args().any(|a| a == "--measure");
    let window = OmarpadWindow::new()?;

    // 配置读取失败时**关闭**自动投放并说明原因 —— 绝不能静默回退到默认开启（评审 🔴1）
    let (settings, wait_secs) = match config::load() {
        Ok(cfg) => (cfg.settings.clone(), cfg.settings.flash_wait_secs),
        Err(e) => {
            let msg = format!("⚠ 配置读取失败，自动投放已停用：{e:#}");
            if !measure {
                window.set_status_line(msg.into());
            }
            (config::Settings { auto_connect: false, flash_wait_secs: 120 }, 120)
        }
    };
    let shared: Arc<Mutex<Shared>> = Arc::new(Mutex::new(Shared {
        auto_connect: settings.auto_connect,
        ..Default::default()
    }));
    window.set_auto_connect(settings.auto_connect);

    let root: Option<PathBuf> = std::env::var_os("IPADCTL_ROOT")
        .map(PathBuf::from)
        .or_else(|| paths::project_root().ok());
    let root = match root {
        Some(r) => Some(r),
        None => {
            if !measure {
                window.set_status_line(
                    "⚠ 找不到仓库根目录（设 IPADCTL_ROOT 或把二进制放回仓库内）。自动投放已禁用。"
                        .into(),
                );
            }
            lock(&shared).auto_connect = false;
            window.set_auto_connect(false);
            None
        }
    };

    // 关窗=隐藏；托盘失败退化为关窗即退出
    let (tx, rx) = std::sync::mpsc::channel::<TrayCommand>();
    match {
        use ksni::blocking::TrayMethods;
        Tray { tx }.spawn()
    } {
        Ok(handle) => {
            let _handle = handle;
            window
                .window()
                .on_close_requested(|| slint::CloseRequestResponse::HideWindow);
        }
        Err(e) => {
            eprintln!("托盘启动失败（{e}）—— 本会话关窗将直接退出");
            window.window().on_close_requested(|| {
                let _ = slint::quit_event_loop();
                slint::CloseRequestResponse::HideWindow
            });
        }
    }

    // ---- P1a 回调 ----
    {
        let shared = shared.clone();
        let weak = window.as_weak();
        window.on_toggle_auto(move |v: bool| {
            lock(&shared).auto_connect = v;
            if let Some(w) = weak.upgrade() {
                w.set_auto_connect(v);
            }
            if let Err(e) = modify_config(&shared, |c| c.settings.auto_connect = v) {
                eprintln!("持久化开关失败：{e:#}");
            }
        });
    }
    {
        let shared = shared.clone();
        window.on_retry_flash(move |port: slint::SharedString| {
            lock(&shared).failures.remove(port.as_str());
        });
    }

    // ---- P1b 回调 ----
    {
        let shared = shared.clone();
        let weak = window.as_weak();
        window.on_canvas_select(move |id: slint::SharedString| {
            let is_ipad = lock(&shared).canvas.iter().any(|e| e.id == id.as_str() && e.is_ipad);
            if let Some(w) = weak.upgrade() {
                w.set_selected_id(id.clone());
                w.set_selected_is_ipad(is_ipad);
            }
        });
    }
    {
        let shared = shared.clone();
        let weak = window.as_weak();
        window.on_canvas_moved(move |id: slint::SharedString, x: f32, y: f32| {
            let shared2 = shared.clone();
            let weak2 = weak.clone();
            std::thread::spawn(move || canvas_moved(shared2, weak2, id.as_str().to_owned(), x as f64, y as f64));
        });
    }
    {
        let shared = shared.clone();
        let weak = window.as_weak();
        window.on_canvas_rotate(move |id: slint::SharedString| {
            let shared2 = shared.clone();
            let weak2 = weak.clone();
            std::thread::spawn(move || canvas_rotate(shared2, weak2, id.as_str().to_owned()));
        });
    }
    {
        let shared = shared.clone();
        let weak = window.as_weak();
        window.on_fix_lua(move || {
            match hypr::insert_require().and_then(|p| {
                hypr::reload_hyprland()?;
                Ok(p)
            }) {
                Ok(p) => {
                    set_status(&weak, format!("✓ 已接入 {} 并重载 Hyprland", p.display()));
                    if let Some(w) = weak.upgrade() {
                        w.set_banner(0);
                    }
                    refresh_canvas(&shared, &weak);
                }
                Err(e) => {
                    set_status(&weak, format!("写入失败：{e:#}"));
                    if let Some(w) = weak.upgrade() {
                        w.set_banner(1);
                    }
                }
            }
        });
    }
    {
        let weak = window.as_weak();
        window.on_fix_cancel(move || {
            if let Some(w) = weak.upgrade() {
                w.set_banner(1);
            }
        });
    }
    {
        let shared = shared.clone();
        window.on_brightness_live(move |slug: slint::SharedString, v: f32| {
            let pct = v.clamp(0.0, 100.0) as u8;
            let Some(conn) = lock(&shared).connectors.get(slug.as_str()).cloned() else {
                return;
            };
            let mut sh = lock(&shared);
            let now = Instant::now();
            let due = sh
                .brightness_last
                .get(slug.as_str())
                .map(|(t, _)| now.duration_since(*t) >= Duration::from_millis(150))
                .unwrap_or(true);
            if due {
                if let Err(e) = conn.set_brightness(pct) {
                    eprintln!("写背光失败：{e:#}");
                }
                sh.brightness_last.insert(slug.as_str().to_owned(), (now, pct));
            }
        });
    }
    {
        let shared = shared.clone();
        window.on_brightness_commit(move |slug: slint::SharedString, v: f32| {
            let pct = v.clamp(0.0, 100.0) as u8;
            // 终值兜底写一次（拖动节流可能错过最后一次），并记回显
            if let Some(conn) = lock(&shared).connectors.get(slug.as_str()).cloned() {
                if let Err(e) = conn.set_brightness(pct) {
                    eprintln!("写背光失败：{e:#}");
                }
            }
            lock(&shared)
                .bright_echo
                .insert(slug.as_str().to_owned(), (Instant::now(), pct));
            if let Err(e) = modify_config(&shared, |c| {
                if let Some(m) = model::by_slug(slug.as_str()) {
                    c.entry_or_default(m).brightness = pct;
                }
            }) {
                eprintln!("持久化亮度失败：{e:#}");
            }
        });
    }
    {
        let shared = shared.clone();
        let weak = window.as_weak();
        window.on_mode_set(move |slug: slint::SharedString, half: bool| {
            let shared2 = shared.clone();
            let weak2 = weak.clone();
            let s = slug.as_str().to_owned();
            std::thread::spawn(move || mode_set(shared2, weak2, s, half));
        });
    }

    // 托盘命令 → 主线程
    let weak = window.as_weak();
    std::thread::spawn(move || handle_tray(rx, weak));

    // 扫描线程：快操作 + 决策 + 定期画布刷新
    let weak = window.as_weak();
    let scan_shared = shared.clone();
    std::thread::spawn(move || scan_loop(weak, scan_shared, root, wait_secs, measure));

    if measure {
        start_measure(window.as_weak());
    } else {
        window.show()?;
    }

    slint::run_event_loop_until_quit()?;
    Ok(())
}

fn handle_tray(rx: std::sync::mpsc::Receiver<TrayCommand>, weak: Weak<OmarpadWindow>) {
    while let Ok(cmd) = rx.recv() {
        match cmd {
            TrayCommand::Toggle => {
                let weak = weak.clone();
                let _ = slint::invoke_from_event_loop(move || {
                    let Some(w) = weak.upgrade() else { return };
                    if w.window().is_visible() {
                        let _ = w.hide();
                    } else {
                        let _ = w.show();
                    }
                });
            }
            TrayCommand::Quit => {
                let _ = slint::invoke_from_event_loop(|| slint::quit_event_loop().unwrap());
                return;
            }
        }
    }
}

const REFUSE_MULTI_TEXT: &str =
    "⚠ 多台 iPad 处于 DFU：checkra1n 无法指定目标。拔掉多余的设备后自动继续。";

fn scan_loop(
    weak: Weak<OmarpadWindow>,
    shared: Arc<Mutex<Shared>>,
    root: Option<PathBuf>,
    wait_secs: u64,
    measure: bool,
) {
    let mut n: i32 = 0;
    let mut refuse_multi_shown = false;
    loop {
        n = n.wrapping_add(1);
        let v = scan_all();

        // ---- P1b：连接器映射 + 热插拔套默认亮度 + 模式缓存 ----
        update_connectors(&shared, &v);
        if n % 10 == 1 {
            refresh_canvas(&shared, &weak);
        }

        // ---- monitors.lua 接入横幅（只在边沿改，不覆盖展开状态）----
        // ⚠ Weak::upgrade 只能在组件创建线程（主线程）用——扫描线程直接 upgrade
        // 恒为 None（评审 🔴4）。hooked 的文件读取在扫描线程做完，UI 改动进 invoke。
        {
            let hooked = hypr::hooked();
            let weak = weak.clone();
            let _ = slint::invoke_from_event_loop(move || {
                let Some(w) = weak.upgrade() else { return };
                let banner = w.get_banner();
                if !hooked && banner == 0 {
                    w.set_banner(1);
                } else if hooked && banner != 2 {
                    w.set_banner(0);
                }
            });
        }

        // ---- 自动投放决策（measure 模式永不投放）----
        if !measure {
            if let Some(root) = &root {
                let mut sh = lock(&shared);
                let live: Vec<String> = v.dfu.iter().map(|d| d.port.clone()).collect();
                sh.failures.retain(|p, _| live.contains(p));

                let failed: Vec<String> = sh.failures.keys().cloned().collect();
                let busy = sh.flash.is_some();
                match flash::auto_decide(&v.dfu, sh.auto_connect, busy, &failed) {
                    AutoAction::Trigger { port } => {
                        let d = v.dfu.iter().find(|d| d.port == port).unwrap();
                        let m = model::from_dfu_serial(&d.serial).unwrap();
                        drop(sh); // 预检是子进程级操作，别占着 Shared 锁（评审 🟡）
                        match flash::preflight(root, m) {
                            Ok(()) => {
                                // 预检耗时后复核：仍是同一台、没被并发占用（决策与执行绑定，评审 🔴2）
                                let mut sh = lock(&shared);
                                let still_same = v
                                    .dfu
                                    .iter()
                                    .any(|x| x.port == port && model::from_dfu_serial(&x.serial).is_some_and(|y| y.slug == m.slug));
                                if !still_same || sh.flash.is_some() {
                                    continue;
                                }
                                sh.flash = Some(FlashRun {
                                    port: port.clone(),
                                    step: "预检通过，启动 checkm8…".into(),
                                });
                                drop(sh);
                                spawn_flash(
                                    shared.clone(),
                                    weak.clone(),
                                    root.clone(),
                                    port,
                                    m,
                                    wait_secs,
                                );
                            }
                            Err(e) => {
                                lock(&shared).failures.insert(
                                    port,
                                    FailedAttempt {
                                        reason: format!("预检失败：{e:#}"),
                                        retryable: true,
                                    },
                                );
                            }
                        }
                    }
                    AutoAction::RefuseMultiple { .. } => {
                        if !refuse_multi_shown {
                            refuse_multi_shown = true;
                            set_status(&weak, REFUSE_MULTI_TEXT.to_owned());
                        }
                    }
                    other => {
                        if refuse_multi_shown && !matches!(other, AutoAction::RefuseMultiple { .. }) {
                            refuse_multi_shown = false;
                            // 只在状态行仍是拒绝文案时清空——别抹掉期间出现的投放消息（评审 🔴9）
                            let weak = weak.clone();
                            let _ = slint::invoke_from_event_loop(move || {
                                if let Some(w) = weak.upgrade() {
                                    if w.get_status_line() == REFUSE_MULTI_TEXT {
                                        w.set_status_line("".into());
                                    }
                                }
                            });
                        }
                    }
                }
            }
        }

        // ---- 刷新卡片 ----
        let items = build_cards(&v, &shared);
        let weak = weak.clone();
        if slint::invoke_from_event_loop(move || {
            let Some(w) = weak.upgrade() else { return };
            w.set_devices(ModelRc::new(VecModel::from(items)));
            w.set_scan_count(n);
        })
        .is_err()
        {
            return;
        }
        std::thread::sleep(Duration::from_secs(1));
    }
}

/// 更新 slug → 连接器映射；新上线的 iPad 自动套配置里的默认亮度（P1b）。
/// 首轮扫描把已有设备记为基线（不套默认）；设备消失即从 seen 移除——
/// **同端口重插视为新出现，重新套默认**（评审 🔴5）。
fn update_connectors(shared: &Arc<Mutex<Shared>>, v: &ScanView) {
    let conns = hypr::connectors();
    let cfg = config::load().unwrap_or_default();
    let mut sh = lock(shared);
    let live: Vec<&String> = v.gud.iter().map(|d| &d.port).collect();
    sh.seen_gud.retain(|p| live.contains(&p));
    let baseline = sh.baseline_done;
    sh.baseline_done = true;
    for d in &v.gud {
        let Some(m) = model::from_product(&d.product) else { continue };
        let Some(c) = conns.iter().find(|c| c.usb_port == d.port) else { continue };
        sh.connectors.insert(m.slug.to_owned(), c.clone());
        if baseline {
            // 首轮之后的新端口（含同端口重插）才套默认：亮度即时写；限流属 P1c
            if sh.seen_gud.insert(d.port.clone()) {
                let b = cfg.get(m).brightness;
                if let Err(e) = c.set_brightness(b) {
                    eprintln!("热插拔套默认亮度失败（{}）：{e:#}", m.slug);
                }
            }
        } else {
            // 首轮基线：只记录，不写
            sh.seen_gud.insert(d.port.clone());
        }
    }
}

/// 从 hyprctl 重建画布数据并推给 UI。hyprctl 不可用则隐藏画布。
fn refresh_canvas(shared: &Arc<Mutex<Shared>>, weak: &Weak<OmarpadWindow>) {
    let mons = match hypr::hyprctl_monitors() {
        Ok(m) => m,
        Err(_) => {
            lock(shared).canvas_ok = false;
            let weak = weak.clone();
            let _ = slint::invoke_from_event_loop(move || {
                if let Some(w) = weak.upgrade() {
                    w.set_canvas_ok(false);
                }
            });
            return;
        }
    };
    let mut entries: Vec<CanvasEntry> = Vec::new();
    let mut modes: HashMap<String, (String, String)> = HashMap::new();
    for m in &mons {
        let md = hypr::monitor_model(m);
        let (x, y, w, h) = hypr::logical_rect(m.x, m.y, m.width, m.height, m.transform, m.scale);
        if let Some(md) = md {
            // 按宽度识别两档，不依赖 availableModes 顺序（评审 🔵）
            if let Some(two) = hypr::split_modes(&m.available_modes) {
                modes.insert(md.slug.to_owned(), two);
            }
        }
        entries.push(CanvasEntry {
            id: md.map(|md| md.slug.to_owned()).unwrap_or_else(|| m.name.clone()),
            label: md
                .map(|md| md.short.to_owned())
                .unwrap_or_else(|| m.name.clone()),
            is_ipad: md.is_some(),
            x,
            y,
            w,
            h,
            transform: m.transform,
        });
    }
    let (min_x, min_y, max_x, max_y) = entries.iter().fold(
        (f64::MAX, f64::MAX, f64::MIN, f64::MIN),
        |(ax, ay, bx, by), e| {
            (
                ax.min(e.x),
                ay.min(e.y),
                bx.max(e.x + e.w),
                by.max(e.y + e.h),
            )
        },
    );
    let mut dup = false;
    for (i, e) in entries.iter().enumerate() {
        if e.is_ipad && entries.iter().skip(i + 1).any(|o| o.is_ipad && o.id == e.id) {
            dup = true;
        }
    }
    {
        let mut sh = lock(shared);
        sh.canvas = entries.clone();
        sh.canvas_ok = true;
        sh.modes = modes;
    }
    let items: Vec<CanvasItem> = entries
        .into_iter()
        .map(|e| CanvasItem {
            mid: e.mid(),
            label: e.label.into(),
            is_ipad: e.is_ipad,
            x: e.x as f32,
            y: e.y as f32,
            w: e.w as f32,
            h: e.h as f32,
            transform: e.transform as i32,
        })
        .collect();
    let span_w = ((max_x - min_x).max(1.0)) as f32;
    let span_h = ((max_y - min_y).max(1.0)) as f32;
    let min_x = min_x as f32;
    let min_y = min_y as f32;
    let weak = weak.clone();
    let _ = slint::invoke_from_event_loop(move || {
        let Some(w) = weak.upgrade() else { return };
        w.set_canvas(ModelRc::new(VecModel::from(items)));
        w.set_canvas_min_x(min_x);
        w.set_canvas_min_y(min_y);
        w.set_canvas_span_w(span_w);
        w.set_canvas_span_h(span_h);
        w.set_canvas_ok(true);
        if dup {
            w.set_status_line("⚠ 同型号多台 iPad 的 EDID desc 相同，画布无法区分（已知限制）".into());
        }
    });
}

impl CanvasEntry {
    /// 临时桥接：CanvasItem 的 mid 字段。
    fn mid(&self) -> slint::SharedString {
        self.id.clone().into()
    }
}

/// 松手：重叠校验 → 存配置 → 写 lua → reload → 回读。运行在工作线程。
/// 全程持 layout_mu：校验用的画布、写盘、重载、回读一气呵成，串行化所有布局操作（评审 🔴6）。
fn canvas_moved(shared: Arc<Mutex<Shared>>, weak: Weak<OmarpadWindow>, slug: String, x: f64, y: f64) {
    let layout_mu = lock(&shared).layout_mu.clone();
    let _layout = lock(&layout_mu);
    let Some(entry) = lock(&shared).canvas.iter().find(|e| e.id == slug).cloned() else {
        return;
    };
    if !entry.is_ipad {
        return;
    }
    let new_rect = (x, y, entry.w, entry.h);
    if let Some(other) = overlap_with(&shared, &slug, new_rect) {
        set_status(&weak, format!("⚠ {} 与 {} 重叠，未保存", entry.label, other));
        refresh_canvas(&shared, &weak);
        return;
    }
    let pos = format!("{}x{}", x.round() as i32, y.round() as i32);
    if let Err(e) = modify_config(&shared, |c| {
        if let Some(m) = model::by_slug(&slug) {
            c.entry_or_default(m).position = pos.clone();
        }
    }) {
        set_status(&weak, format!("⚠ 写配置失败：{e:#}"));
        return;
    }
    match persist_layout(&shared, &weak) {
        Ok(()) => set_status_transient(&weak, format!("✓ {} 已移到 {} 并生效", entry.label, pos)),
        Err(e) => set_status(&weak, format!("⚠ 布局生效失败：{e:#}")),
    }
}

/// 旋转所选：0→1→3→2 循环，中心锚定，旋转后尺寸交换再校验重叠。串行（layout_mu）。
fn canvas_rotate(shared: Arc<Mutex<Shared>>, weak: Weak<OmarpadWindow>, slug: String) {
    let layout_mu = lock(&shared).layout_mu.clone();
    let _layout = lock(&layout_mu);
    let Some(entry) = lock(&shared).canvas.iter().find(|e| e.id == slug).cloned() else {
        return;
    };
    if !entry.is_ipad {
        return;
    }
    let nt = hypr::next_transform(entry.transform);
    let was_rot = matches!(entry.transform, 1 | 3);
    let now_rot = matches!(nt, 1 | 3);
    let (nw, nh) = if was_rot == now_rot {
        (entry.w, entry.h)
    } else {
        (entry.h, entry.w)
    };
    let cx = entry.x + entry.w / 2.0;
    let cy = entry.y + entry.h / 2.0;
    let (nx, ny) = (cx - nw / 2.0, cy - nh / 2.0);
    if let Some(other) = overlap_with(&shared, &slug, (nx, ny, nw, nh)) {
        set_status(&weak, format!("⚠ 旋转后与 {} 重叠，未保存", other));
        refresh_canvas(&shared, &weak);
        return;
    }
    let pos = format!("{}x{}", nx.round() as i32, ny.round() as i32);
    if let Err(e) = modify_config(&shared, |c| {
        if let Some(m) = model::by_slug(&slug) {
            let d = c.entry_or_default(m);
            d.transform = nt;
            d.position = pos.clone();
        }
    }) {
        set_status(&weak, format!("⚠ 写配置失败：{e:#}"));
        return;
    }
    match persist_layout(&shared, &weak) {
        Ok(()) => set_status_transient(
            &weak,
            format!("✓ {} 已旋转（transform {nt}）并生效", entry.label),
        ),
        Err(e) => set_status(&weak, format!("⚠ 布局生效失败：{e:#}")),
    }
}

/// 分辨率两档：原生 = preferred（mode 置 None），半分辨率 = EDID 窄档。串行（layout_mu）。
fn mode_set(shared: Arc<Mutex<Shared>>, weak: Weak<OmarpadWindow>, slug: String, half: bool) {
    let layout_mu = lock(&shared).layout_mu.clone();
    let _layout = lock(&layout_mu);
    let (native, half_mode) = {
        let sh = lock(&shared);
        let Some(m) = sh.modes.get(&slug).cloned() else {
            set_status(&weak, "⚠ 没读到这台的模式档位".into());
            return;
        };
        m
    };
    let chosen = if half {
        half_mode.clone()
    } else {
        native.clone()
    };
    if let Err(e) = modify_config(&shared, |c| {
        if let Some(m) = model::by_slug(&slug) {
            c.entry_or_default(m).mode = if half { Some(half_mode.clone()) } else { None };
        }
    }) {
        set_status(&weak, format!("⚠ 写配置失败：{e:#}"));
        return;
    }
    match persist_layout(&shared, &weak) {
        Ok(()) => set_status_transient(&weak, format!("✓ 已切到 {chosen} 并生效")),
        Err(e) => set_status(&weak, format!("⚠ 布局生效失败：{e:#}")),
    }
}

/// 与其它画布块的重叠检查，返回撞到的块名。
fn overlap_with(shared: &Arc<Mutex<Shared>>, slug: &str, rect: (f64, f64, f64, f64)) -> Option<String> {
    lock(shared)
        .canvas
        .iter()
        .filter(|e| e.id != slug)
        .find(|e| hypr::rects_overlap(rect, (e.x, e.y, e.w, e.h)))
        .map(|e| e.label.clone())
}

/// 写 lua → hyprctl reload → 等 600 ms → 回读画布。
/// 这版 hyprctl 没有 currentMode 字段，模式切换的效果读不回来 —— 如实标注。
fn persist_layout(shared: &Arc<Mutex<Shared>>, weak: &Weak<OmarpadWindow>) -> anyhow::Result<()> {
    let mu = lock(shared).config_mu.clone();
    let _g = lock(&mu);
    let cfg = config::load()?;
    hypr::write_lua(&cfg)?;
    drop(_g);
    hypr::reload_hyprland()?;
    std::thread::sleep(Duration::from_millis(600));
    refresh_canvas(shared, weak);
    Ok(())
}

fn set_status(weak: &Weak<OmarpadWindow>, text: String) {
    let weak = weak.clone();
    let _ = slint::invoke_from_event_loop(move || {
        if let Some(w) = weak.upgrade() {
            w.set_status_line(text.into());
        }
    });
}

/// 成功类消息：5 秒后自动淡出（失败类用 set_status 保留）。
/// 清除前比对文本：期间出现了更新的消息（如投放失败）就不动它（评审 🔴9）。
fn set_status_transient(weak: &Weak<OmarpadWindow>, text: String) {
    set_status(weak, text.clone());
    let weak2 = weak.clone();
    let _ = slint::invoke_from_event_loop(move || {
        let w3 = weak2.clone();
        slint::Timer::single_shot(Duration::from_secs(5), move || {
            if let Some(w) = w3.upgrade() {
                if w.get_status_line() == text.as_str() {
                    w.set_status_line("".into());
                }
            }
        });
    });
}

fn empty_card() -> DeviceCard {
    DeviceCard {
        slug: "".into(),
        name: "".into(),
        state: "".into(),
        port: "".into(),
        detail: "".into(),
        state_color: brush(FAIL),
        progress: "".into(),
        action_label: "".into(),
        brightness: -1.0,
        has_brightness: false,
        mode_native: "".into(),
        mode_half: "".into(),
        mode_half_on: false,
        has_mode: false,
    }
}

fn build_cards(v: &ScanView, shared: &Arc<Mutex<Shared>>) -> Vec<DeviceCard> {
    let sh = lock(shared);
    let cfg = config::load().unwrap_or_default();
    let mut out: Vec<DeviceCard> = Vec::new();

    for d in &v.dfu {
        let mut c = empty_card();
        c.port = d.port.clone().into();
        match model::from_dfu_serial(&d.serial) {
            Some(m) => {
                c.slug = m.slug.into();
                c.name = m.name.into();
                c.detail = format!("CPID/BDID 已识别（{}）", m.slug).into();
                if let Some(f) = sh.flash.as_ref().filter(|f| f.port == d.port) {
                    c.state = "自动连接中".into();
                    c.state_color = brush(PENDING);
                    c.progress = f.step.clone().into();
                    c.detail = "投放进行中，期间扫描与其它设备不受影响".into();
                } else if let Some(fa) = sh.failures.get(&d.port) {
                    c.state = "自动连接失败".into();
                    c.state_color = brush(FAIL);
                    c.progress = fa.reason.clone().into();
                    c.detail = "同一台不会自动重试；重试将重跑全部预检".into();
                    if fa.retryable {
                        c.action_label = "重试".into();
                    }
                } else {
                    c.state = if sh.auto_connect { "DFU 待投放" } else { "DFU（自动已关）" }.into();
                    c.state_color = brush(PENDING);
                }
            }
            None => {
                c.name = "未知设备".into();
                c.state = "拒绝投放".into();
                c.detail = format!("serial: {}", d.serial).into();
                c.state_color = brush(FAIL);
            }
        }
        out.push(c);
    }

    for d in &v.gud {
        let mut c = empty_card();
        c.port = d.port.clone().into();
        match model::from_product(&d.product) {
            Some(m) => {
                c.slug = m.slug.into();
                c.name = m.name.into();
                c.state = "在线".into();
                c.state_color = brush(OK);
                c.detail = format!(
                    "维护口 {}",
                    usb::maintenance_tty(&d.port)
                        .map(|p| p.display().to_string())
                        .unwrap_or_else(|| "未找到".into())
                )
                .into();
                if let Some(conn) = sh.connectors.get(m.slug) {
                    // 5 s 内的松手回显优先（sysfs 下一拍才对齐）
                    let pct = sh
                        .bright_echo
                        .get(m.slug)
                        .filter(|(t, _)| t.elapsed() < Duration::from_secs(5))
                        .map(|(_, p)| *p)
                        .or_else(|| conn.brightness_pct());
                    if let Some(p) = pct {
                        c.brightness = p as f32;
                        c.has_brightness = true;
                    }
                }
                if let Some((native, half)) = sh.modes.get(m.slug) {
                    c.mode_native = native.clone().into();
                    c.mode_half = half.clone().into();
                    c.has_mode = true;
                    c.mode_half_on = cfg.get(m).mode.is_some();
                }
            }
            None => {
                c.name = "未知设备".into();
                c.state = "产品串不认识".into();
                c.detail = format!("product: {:?}", d.product).into();
                c.state_color = brush(FAIL);
            }
        }
        out.push(c);
    }
    for d in &v.uart {
        let mut c = empty_card();
        c.name = "引导失败".into();
        c.state = "m1n1 uartproxy".into();
        c.port = d.port.clone().into();
        c.detail = "m1n1 起来了但内核没有引导（DTB 线索见 gud/dtb/README.md）".into();
        c.state_color = brush(FAIL);
        out.push(c);
    }
    for d in &v.acm {
        let mut c = empty_card();
        c.name = d.product.clone().into();
        c.state = "仅维护口".into();
        c.port = d.port.clone().into();
        c.detail = "GUD 失败的退路（2×ACM）".into();
        c.state_color = brush(FALLBACK);
        out.push(c);
    }
    out.sort_by(|a, b| a.port.cmp(&b.port));
    out
}

fn spawn_flash(
    shared: Arc<Mutex<Shared>>,
    weak: Weak<OmarpadWindow>,
    root: PathBuf,
    port: String,
    m: &'static model::Model,
    wait_secs: u64,
) {
    std::thread::spawn(move || {
        let set_step = |t: String| {
            if let Some(f) = lock(&shared).flash.as_mut() {
                f.step = t.clone();
            }
            set_status(&weak, t);
        };
        let mut on_event = |ev: &FlashEvent| match ev {
            FlashEvent::Step(1) => set_step("[1/3] checkm8 + PongoOS…".into()),
            FlashEvent::Session(s) => set_step(format!("[1/3] session {s}")),
            FlashEvent::Step(2) => set_step("[2/3] 上传 Linux 并启动…".into()),
            FlashEvent::UploadLine(l) => set_step(format!("[2/3] {l}")),
            FlashEvent::Step(3) => set_step("[3/3] 等它上线…".into()),
            FlashEvent::Online { port } => {
                set_step(format!("[3/3] 已在 {port} 上线，等 Hyprland 接管…"))
            }
            _ => {}
        };

        // 决策与执行绑定：执行前复核 DFU 端口与机型仍是决策时那台（评审 🔴2）
        match flash::flash_with_progress(
            &root,
            Duration::from_secs(wait_secs),
            Some((port.as_str(), m)),
            &mut on_event,
        ) {
            Ok(out) => {
                let mut final_take = hypr::Takeover::UsbOnly;
                for _ in 0..40 {
                    match hypr::takeover(&out.port) {
                        t @ hypr::Takeover::DesktopActive(_) => {
                            final_take = t;
                            break;
                        }
                        t => final_take = t,
                    }
                    std::thread::sleep(Duration::from_millis(500));
                }
                let msg = match &final_take {
                    hypr::Takeover::DesktopActive(conn) => {
                        format!("✓ {} 桌面生效（{}）", out.model.name, conn)
                    }
                    hypr::Takeover::ConnectorReady(conn) => format!(
                        "⚠ {} 连接器 {} 已连接但未被接管 —— 检查 monitors.lua 是否加载 ipadctl 布局",
                        out.model.name, conn
                    ),
                    hypr::Takeover::UsbOnly => {
                        format!("⚠ {} 仅 USB 上线，未找到 DRM 连接器", out.model.name)
                    }
                };
                lock(&shared).flash = None;
                set_status_transient(&weak, msg);
            }
            Err(e) => {
                let reason = format!("{e:#}");
                let first = reason.lines().next().unwrap_or("未知错误").to_owned();
                let mut sh = lock(&shared);
                sh.flash = None;
                sh.failures.insert(
                    port,
                    FailedAttempt {
                        reason,
                        retryable: true,
                    },
                );
                drop(sh);
                set_status(&weak, format!("自动连接失败：{first}"));
            }
        }
        let _ = m;
    });
}

/// `--measure`：内存测量驱动。**自动投放在此模式下被 scan_loop 禁用。**
fn start_measure(weak: Weak<OmarpadWindow>) {
    std::thread::spawn(move || {
        let rss_kb = || -> u64 {
            std::fs::read_to_string("/proc/self/status")
                .ok()
                .and_then(|t| {
                    t.lines()
                        .find(|l| l.starts_with("VmRSS"))
                        .and_then(|l| l.split_whitespace().nth(1).and_then(|v| v.parse().ok()))
                })
                .unwrap_or(0)
        };
        let sample = |tag: &str| println!("MEM\t{tag}\t{} kB", rss_kb());
        let set_visible = |v: bool| {
            let weak = weak.clone();
            let _ = slint::invoke_from_event_loop(move || {
                if let Some(w) = weak.upgrade() {
                    if v {
                        let _ = w.show();
                    } else {
                        let _ = w.hide();
                    }
                }
            });
        };

        std::thread::sleep(Duration::from_secs(1));
        sample("01-cold-tray-only");
        set_visible(true);
        std::thread::sleep(Duration::from_secs(2));
        sample("02-window-open");
        set_visible(false);
        std::thread::sleep(Duration::from_secs(2));
        sample("03-window-hidden");
        for _ in 0..3 {
            set_visible(true);
            std::thread::sleep(Duration::from_millis(600));
            set_visible(false);
            std::thread::sleep(Duration::from_millis(600));
        }
        std::thread::sleep(Duration::from_secs(2));
        sample("04-after-3-reopen-hidden");
        println!("MEM\tdone");
        let _ = slint::quit_event_loop();
    });
}
