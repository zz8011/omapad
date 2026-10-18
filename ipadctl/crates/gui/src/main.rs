//! omarpad —— P1b：画布排列（拖拽/旋转/重叠校验/重载回读）+ monitors.lua
//! 一键接入横幅 + 亮度滑条（实时生效、松手持久化、热插拔套默认）+ 分辨率两档。
//!
//! 线程纪律（DESIGN.md §3）：主线程独占 Slint；扫描线程只做快操作 + 决策；
//! 投放/布局生效（hyprctl reload + 回读）在工作线程跑。配置读改写经
//! `config_mu` 串行化，且**不与 Shared 锁嵌套**（防死锁）。
//!
//! `--measure` 模式禁用自动投放（内存测量绝不刷机器）。

slint::include_modules!();

mod charge;
mod layout;
mod theme;

#[cfg(test)]
mod tray_icon_tests {
    use super::*;

    #[test]
    fn tray_uses_omarpad_artwork_at_small_sizes() {
        let (tx, _rx) = std::sync::mpsc::channel();
        let tray = Tray { tx };
        assert!(ksni::Tray::icon_name(&tray).is_empty());
        let icons = ksni::Tray::icon_pixmap(&tray);
        assert_eq!(
            icons.iter().map(|i| (i.width, i.height)).collect::<Vec<_>>(),
            [(32, 32), (64, 64)]
        );
        for icon in icons {
            assert_eq!(icon.data.len(), (icon.width * icon.height * 4) as usize);
            assert!(icon.data.as_chunks::<4>().0.contains(&[255, 16, 26, 49]));
        }
    }
}

#[cfg(test)]
mod device_model_tests {
    use super::*;
    use slint::Model;

    #[test]
    fn refresh_preserves_repeater_for_same_device() {
        let current = ModelRc::new(VecModel::from(vec![("j207", 100)]));
        let updated = preserve_rows(current.clone(), vec![("j207", 50)], |a, b| a.0 == b.0);
        assert_eq!(current, updated);
        assert_eq!(current.row_data(0), Some(("j207", 50)));
    }

    #[test]
    fn hotplug_replaces_repeater_when_identity_changes() {
        let current = ModelRc::new(VecModel::from(vec![("j207", 100)]));
        let updated = preserve_rows(current.clone(), vec![("a1599", 50)], |a, b| a.0 == b.0);
        assert_ne!(current, updated);
        assert_eq!(current.row_data(0), Some(("j207", 100)));
    }
}

use flash::AutoAction;
use ipadctl::flash::{self, FlashEvent};
use ipadctl::hypr::Connector;
use ipadctl::{config, device, hypr, model, paths, usb};
use slint::{Brush, Color, ComponentHandle, Model, ModelRc, VecModel, Weak};
use std::collections::{HashMap, HashSet};
use std::path::PathBuf;
use std::sync::{Arc, LazyLock, Mutex, MutexGuard};
use std::time::{Duration, Instant};

fn color(rgb: theme::Rgb) -> Color {
    Color::from_rgb_u8(rgb.0, rgb.1, rgb.2)
}

fn brush(rgb: theme::Rgb) -> Brush {
    Brush::SolidColor(color(rgb))
}

#[derive(Clone, PartialEq, Eq)]
struct StyleSnapshot {
    name: String,
    palette: theme::Palette,
    mono: String,
    reduce_motion: bool,
}

fn current_style() -> anyhow::Result<StyleSnapshot> {
    let (name, palette) = theme::current()?;
    Ok(StyleSnapshot {
        name,
        palette,
        mono: theme::mono_font().unwrap_or_else(|| "Noto Sans Mono CJK SC".to_owned()),
        reduce_motion: theme::reduce_motion(),
    })
}

fn apply_style(window: &OmapadWindow, style: &StyleSnapshot) {
    let t = window.global::<Theme>();
    let p = &style.palette;
    t.set_bg_window(color(p.window));
    t.set_bg_inset(color(p.inset));
    t.set_bg_card(color(p.card));
    t.set_bg_card_hi(color(p.card_hi));
    t.set_bg_callout(color(p.callout));
    t.set_bg_accent(color(p.accent_soft));
    t.set_border(color(p.border));
    t.set_border_hi(color(p.border_hi));
    t.set_divider(color(p.divider));
    t.set_accent(color(p.accent));
    t.set_accent_text(color(p.accent_text));
    t.set_olive(color(p.ok));
    t.set_text(color(p.text));
    t.set_text_2(color(p.text_secondary));
    t.set_text_3(color(p.text_muted));
    t.set_mono(style.mono.clone().into());
    t.set_reduce_motion(style.reduce_motion);
    window.set_theme_name(style.name.clone().into());
}

/// 保留相同设备的 Slint repeater 实例。扫描每秒更新一次数据；若重建
/// ModelRc，TouchArea 的按下状态会丢失，亮度滑条会在拖动中途回弹。
fn preserve_rows<T: Clone + PartialEq + 'static>(
    current: ModelRc<T>,
    next: Vec<T>,
    same_identity: impl Fn(&T, &T) -> bool,
) -> ModelRc<T> {
    if current.row_count() != next.len()
        || next.iter().enumerate().any(|(i, item)| {
            !current
                .row_data(i)
                .is_some_and(|old| same_identity(&old, item))
        })
    {
        return ModelRc::new(VecModel::from(next));
    }
    for (i, item) in next.into_iter().enumerate() {
        if current.row_data(i).is_some_and(|old| old != item) {
            current.set_row_data(i, item);
        }
    }
    current
}

fn watch_style(weak: Weak<OmapadWindow>, shared: Arc<Mutex<Shared>>, mut last: StyleSnapshot) {
    loop {
        std::thread::sleep(Duration::from_secs(3));
        let Ok(next) = current_style() else { continue };
        if next == last {
            continue;
        }
        let generation = {
            let mut sh = lock(&shared);
            sh.palette = next.palette.clone();
            sh.style_generation = sh.style_generation.wrapping_add(1);
            sh.style_generation
        };
        let v = scan_all();
        let devices = build_cards(&v, &shared);
        let charge_open = lock(&shared).device_tab_open;
        let charges = charge_open.then(|| build_charge_cards(&v, &shared));
        let ui_style = next.clone();
        let weak2 = weak.clone();
        let shared2 = shared.clone();
        if slint::invoke_from_event_loop(move || {
            if lock(&shared2).style_generation != generation {
                return;
            }
            if let Some(w) = weak2.upgrade() {
                apply_style(&w, &ui_style);
                let current = w.get_devices();
                let updated = preserve_rows(current.clone(), devices, |a, b| a.slug == b.slug);
                if updated != current {
                    w.set_devices(updated);
                }
                if let Some(charges) = charges {
                    w.set_charge_empty(charges.is_empty());
                    w.set_charges(ModelRc::new(VecModel::from(charges)));
                }
            }
        })
        .is_err()
        {
            return;
        }
        last = next;
    }
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

struct ChargeSubmission {
    target: String,
    device_uptime: Option<f64>,
    submitted_at: Instant,
}

struct ChargeSample {
    limit: Result<String, String>,
    battery: Option<Result<Option<i32>, String>>,
    log: Option<String>,
}

#[derive(Default)]
struct ChargeReading {
    limit: Option<Result<String, String>>,
    battery: Option<Result<Option<i32>, String>>,
    j207_log: Option<String>,
    sampled_at: Option<Instant>,
    submitted: Option<ChargeSubmission>,
    verify_after: Option<Instant>,
    action_error: Option<String>,
}

#[derive(Default)]
struct Shared {
    palette: theme::Palette,
    style_generation: u64,
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
    device_tab_open: bool,
    charge_readings: HashMap<String, ChargeReading>,
    charge_busy: HashSet<String>,
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
        "omapad".into()
    }
    fn title(&self) -> String {
        "Omapad".into()
    }
    fn icon_name(&self) -> String {
        String::new()
    }
    fn icon_pixmap(&self) -> Vec<ksni::Icon> {
        // 原版品牌应用图标；SNI 要求网络字节序 ARGB，而 PNG 解码为 RGBA。
        static ICONS: LazyLock<Vec<ksni::Icon>> = LazyLock::new(|| {
            [
                include_bytes!("../assets/omapad-app-icon-32.png").as_slice(),
                include_bytes!("../assets/omapad-app-icon-64.png").as_slice(),
            ]
            .into_iter()
            .map(|bytes| {
                let img = image::load_from_memory_with_format(bytes, image::ImageFormat::Png)
                    .expect("bundled Omapad icon must be valid PNG")
                    .into_rgba8();
                let (width, height) = img.dimensions();
                let mut data = img.into_vec();
                for pixel in data.as_chunks_mut::<4>().0 {
                    pixel.rotate_right(1);
                }
                ksni::Icon {
                    width: width as i32,
                    height: height as i32,
                    data,
                }
            })
            .collect()
        });
        ICONS.clone()
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
fn modify_config(
    shared: &Arc<Mutex<Shared>>,
    f: impl FnOnce(&mut config::Config),
) -> anyhow::Result<()> {
    let mu = lock(shared).config_mu.clone();
    let _g = lock(&mu);
    let mut cfg = config::load()?;
    f(&mut cfg);
    config::save(&cfg)
}

fn main() -> anyhow::Result<()> {
    let charge_preview = std::env::args().any(|a| a == "--preview-charge");
    let layout_preview = std::env::args().any(|a| a == "--preview-layout");
    let measure = charge_preview || layout_preview || std::env::args().any(|a| a == "--measure");
    let window = OmapadWindow::new()?;
    window.set_preview_mode(measure);
    if charge_preview {
        window.set_tab(1);
    }
    let mut initial_style = current_style().unwrap_or_else(|e| {
        eprintln!("Omarchy 主题读取失败，使用界面回退色板：{e:#}");
        StyleSnapshot {
            name: "系统回退".to_owned(),
            palette: theme::Palette::fallback(),
            mono: theme::mono_font().unwrap_or_else(|| "Noto Sans Mono CJK SC".to_owned()),
            reduce_motion: theme::reduce_motion(),
        }
    });
    let preview_palette = charge_preview
        .then(|| std::env::var_os("OMARPAD_PREVIEW_PALETTE"))
        .flatten();
    if let Some(path) = &preview_palette {
        let raw = std::fs::read_to_string(path)?;
        initial_style.palette = theme::Palette::from_resolved(&raw)?;
        initial_style.name = "离线主题预览".to_owned();
    }
    apply_style(&window, &initial_style);

    // 配置读取失败时**关闭**自动投放并说明原因 —— 绝不能静默回退到默认开启（评审 🔴1）
    let (settings, wait_secs) = match config::load() {
        Ok(cfg) => (cfg.settings.clone(), cfg.settings.flash_wait_secs),
        Err(e) => {
            let msg = format!("⚠ 配置读取失败，自动投放已停用：{e:#}");
            if !measure {
                window.set_status_line(msg.into());
            }
            (
                config::Settings {
                    auto_connect: false,
                    flash_wait_secs: 120,
                },
                120,
            )
        }
    };
    let shared: Arc<Mutex<Shared>> = Arc::new(Mutex::new(Shared {
        auto_connect: settings.auto_connect && !measure,
        device_tab_open: charge_preview,
        palette: initial_style.palette.clone(),
        ..Default::default()
    }));
    window.set_auto_connect(settings.auto_connect && !measure);

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
    use ksni::blocking::TrayMethods;
    let tray_ready = match (Tray { tx }).spawn() {
        Ok(handle) => {
            let _handle = handle;
            window
                .window()
                .on_close_requested(|| slint::CloseRequestResponse::HideWindow);
            true
        }
        Err(e) => {
            eprintln!("托盘启动失败（{e}）—— 本会话关窗将直接退出");
            window.window().on_close_requested(|| {
                let _ = slint::quit_event_loop();
                slint::CloseRequestResponse::HideWindow
            });
            false
        }
    };
    {
        let weak = window.as_weak();
        window.on_close_settings(move || {
            if tray_ready {
                if let Some(w) = weak.upgrade() {
                    let _ = w.hide();
                }
            } else {
                let _ = slint::quit_event_loop();
            }
        });
    }

    // ---- P1a 回调 ----
    {
        let shared = shared.clone();
        let weak = window.as_weak();
        window.on_toggle_auto(move |v: bool| {
            if measure {
                return;
            }
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
            let is_ipad = lock(&shared)
                .canvas
                .iter()
                .any(|e| e.id == id.as_str() && e.is_ipad);
            if let Some(w) = weak.upgrade() {
                w.set_selected_id(id.clone());
                w.set_selected_is_ipad(is_ipad);
            }
        });
    }
    {
        let shared = shared.clone();
        let weak = window.as_weak();
        window.on_canvas_moved(move |id: slint::SharedString, x: f32, y: f32, scale: f32| {
            let shared2 = shared.clone();
            let weak2 = weak.clone();
            std::thread::spawn(move || {
                canvas_moved(
                    shared2,
                    weak2,
                    id.as_str().to_owned(),
                    x as f64,
                    y as f64,
                    scale as f64,
                )
            });
        });
    }
    {
        let shared = shared.clone();
        let weak = window.as_weak();
        window.on_canvas_rotate(move |id: slint::SharedString, scale: f32| {
            let shared2 = shared.clone();
            let weak2 = weak.clone();
            std::thread::spawn(move || {
                canvas_rotate(shared2, weak2, id.as_str().to_owned(), scale as f64)
            });
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
                sh.brightness_last
                    .insert(slug.as_str().to_owned(), (now, pct));
            }
        });
    }
    {
        let shared = shared.clone();
        window.on_brightness_commit(move |slug: slint::SharedString, v: f32| {
            let pct = v.clamp(0.0, 100.0) as u8;
            // 终值兜底写一次（拖动节流可能错过最后一次），并记回显
            if let Some(conn) = lock(&shared).connectors.get(slug.as_str()).cloned()
                && let Err(e) = conn.set_brightness(pct)
            {
                eprintln!("写背光失败：{e:#}");
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
    {
        let shared = shared.clone();
        let weak = window.as_weak();
        window.on_touch_toggle(move |slug: slint::SharedString, enabled: bool| {
            let shared2 = shared.clone();
            let weak2 = weak.clone();
            let s = slug.as_str().to_owned();
            std::thread::spawn(move || touch_toggle(shared2, weak2, s, enabled));
        });
    }
    {
        window.on_touch_calibrate(move |slug: slint::SharedString| {
            let s = slug.as_str().to_owned();
            eprintln!("触摸校准请求：{s}");
            // TODO: 实现校准窗口
        });
    }
    {
        let shared = shared.clone();
        window.on_tab_changed(move |device_page_open| {
            lock(&shared).device_tab_open = device_page_open;
        });
    }
    {
        let shared = shared.clone();
        let weak = window.as_weak();
        window.on_charge_refresh(move |slug| {
            let v = scan_all();
            if let Some(d) = unique_charge_device(&v, slug.as_str()) {
                spawn_charge_read(&shared, &weak, d.port.clone(), true);
            }
        });
    }
    {
        let shared = shared.clone();
        let weak = window.as_weak();
        window.on_charge_set(move |slug, hex| {
            let slug = slug.as_str();
            let hex = hex.as_str();
            if !charge::allowed(slug, hex) {
                return;
            }
            let v = scan_all();
            if let Some(d) = unique_charge_device(&v, slug) {
                spawn_charge_set(
                    &shared,
                    &weak,
                    d.port.clone(),
                    slug.to_owned(),
                    hex.to_owned(),
                );
            }
        });
    }

    // 托盘命令 → 主线程
    let weak = window.as_weak();
    std::thread::spawn(move || handle_tray(rx, weak));

    // 扫描线程：快操作 + 决策 + 定期画布刷新
    let weak = window.as_weak();
    let scan_shared = shared.clone();
    std::thread::spawn(move || scan_loop(weak, scan_shared, root, wait_secs, measure));

    if preview_palette.is_none() {
        let weak = window.as_weak();
        let style_shared = shared.clone();
        std::thread::spawn(move || watch_style(weak, style_shared, initial_style));
    }

    if measure && !charge_preview && !layout_preview {
        start_measure(window.as_weak());
    } else {
        window.show()?;
    }

    slint::run_event_loop_until_quit()?;
    Ok(())
}

fn handle_tray(rx: std::sync::mpsc::Receiver<TrayCommand>, weak: Weak<OmapadWindow>) {
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

/// Controls use a model slug, but the ACM session is always bound to one current USB port.
/// A duplicate model has no unambiguous target in the current UI, so neither card can write.
fn unique_charge_device<'a>(v: &'a ScanView, slug: &str) -> Option<&'a usb::UsbDevice> {
    let mut matching = v
        .gud
        .iter()
        .filter(|d| model::from_product(&d.product).is_some_and(|m| m.slug == slug));
    let first = matching.next()?;
    matching.next().is_none().then_some(first)
}

fn spawn_charge_read(
    shared: &Arc<Mutex<Shared>>,
    weak: &Weak<OmapadWindow>,
    port: String,
    force: bool,
) {
    {
        let mut sh = lock(shared);
        if sh.charge_busy.contains(&port) || sh.charge_busy.len() >= 2 {
            return;
        }
        let due = sh.charge_readings.get(&port).is_none_or(|r| {
            force
                || r.verify_after.is_some_and(|t| Instant::now() >= t)
                || r.sampled_at
                    .is_none_or(|t| t.elapsed() >= Duration::from_secs(60))
        });
        if !due {
            return;
        }
        sh.charge_busy.insert(port.clone());
    }
    let shared = shared.clone();
    let weak = weak.clone();
    std::thread::spawn(move || {
        let live = scan_all().gud.into_iter().find(|d| d.port == port);
        let m = live.as_ref().and_then(|d| model::from_product(&d.product));
        let result = (|| -> anyhow::Result<ChargeSample> {
            let m = m.ok_or_else(|| anyhow::anyhow!("设备已经离线"))?;
            let tty = usb::maintenance_tty(&port).ok_or_else(|| anyhow::anyhow!("维护口未就绪"))?;
            let mut acm = ipadctl::acm::Acm::open(&tty, &port)?;
            if m.slug == "j207" {
                let log = device::watchdog_last_write(&mut acm)?;
                let limit = log
                    .as_deref()
                    .and_then(charge::watchdog_value)
                    .map(|v| Ok(format!("0x{v} · 最近看门狗日志")))
                    .unwrap_or_else(|| Err("没有可识别的看门狗写入日志".into()));
                Ok(ChargeSample {
                    limit,
                    battery: Some(Ok(None)),
                    log,
                })
            } else {
                let limit =
                    device::charge_limit_effective(&mut acm, m).map_err(|e| format!("{e:#}"));
                let battery = device::battery_current(&mut acm, m).map_err(|e| format!("{e:#}"));
                Ok(ChargeSample {
                    limit,
                    battery: Some(battery),
                    log: None,
                })
            }
        })();
        let mut sh = lock(&shared);
        sh.charge_busy.remove(&port);
        // A disconnected port is removed by the scanner. Never resurrect its stale sample.
        if !scan_all().gud.iter().any(|d| d.port == port) {
            return;
        }
        let r = sh.charge_readings.entry(port).or_default();
        match result {
            Ok(sample) => {
                r.limit = Some(sample.limit);
                r.battery = sample.battery;
                r.j207_log = sample.log;
            }
            Err(e) => {
                r.limit = Some(Err(format!("{e:#}")));
                r.battery = None;
                r.j207_log = None;
            }
        }
        r.sampled_at = Some(Instant::now());
        r.verify_after = None;
        drop(sh);
        let _ = slint::invoke_from_event_loop(move || {
            let _ = weak.upgrade();
        });
    });
}

fn spawn_charge_set(
    shared: &Arc<Mutex<Shared>>,
    weak: &Weak<OmapadWindow>,
    port: String,
    slug: String,
    hex: String,
) {
    {
        let mut sh = lock(shared);
        if sh.charge_busy.contains(&port) || sh.charge_busy.len() >= 2 {
            return;
        }
        sh.charge_busy.insert(port.clone());
        sh.charge_readings
            .entry(port.clone())
            .or_default()
            .action_error = None;
    }
    let shared = shared.clone();
    let weak = weak.clone();
    std::thread::spawn(move || {
        let result = (|| -> anyhow::Result<Option<f64>> {
            let v = scan_all();
            let d = unique_charge_device(&v, &slug)
                .filter(|d| d.port == port)
                .ok_or_else(|| anyhow::anyhow!("设备已断开或同型号设备无法区分"))?;
            let m = model::from_product(&d.product).expect("unique model");
            let tty = usb::maintenance_tty(&port).ok_or_else(|| anyhow::anyhow!("维护口未就绪"))?;
            let mut acm = ipadctl::acm::Acm::open(&tty, &port)?;
            device::set_charge_limit(&mut acm, m, &hex)?;
            // Read the device's uptime *after* the target write. A matching
            // kernel log must be newer than this marker to confirm our action.
            let uptime = if slug == "j207" {
                acm.run("cat /proc/uptime", Duration::from_secs(10))
                    .ok()
                    .and_then(|(out, rc)| (rc == 0).then(|| charge::device_uptime(&out)))
                    .flatten()
            } else {
                None
            };
            Ok(uptime)
        })();
        let succeeded = result.is_ok();
        let mut sh = lock(&shared);
        sh.charge_busy.remove(&port);
        let r = sh.charge_readings.entry(port.clone()).or_default();
        match result {
            Ok(device_uptime) => {
                let now = Instant::now();
                r.submitted = Some(ChargeSubmission {
                    target: hex.clone(),
                    device_uptime,
                    submitted_at: now,
                });
                r.verify_after = Some(now + Duration::from_secs(31));
                r.action_error = None;
            }
            Err(e) => r.action_error = Some(format!("限流目标写入失败：{e:#}")),
        }
        drop(sh);
        if succeeded
            && let Err(e) = modify_config(&shared, |c| {
                if let Some(m) = model::by_slug(&slug) {
                    c.entry_or_default(m).charge_limit = hex;
                }
            })
        {
            lock(&shared)
                .charge_readings
                .entry(port)
                .or_default()
                .action_error = Some(format!("设备已接收目标，但保存默认值失败：{e:#}"));
        }
        let _ = slint::invoke_from_event_loop(move || {
            let _ = weak.upgrade();
        });
    });
}

fn refresh_charge_jobs(shared: &Arc<Mutex<Shared>>, weak: &Weak<OmapadWindow>, v: &ScanView) {
    {
        let mut sh = lock(shared);
        let live: HashSet<&str> = v.gud.iter().map(|d| d.port.as_str()).collect();
        sh.charge_readings
            .retain(|port, _| live.contains(port.as_str()));
        if !sh.device_tab_open {
            return;
        }
    }
    for d in &v.gud {
        if model::from_product(&d.product).is_some() {
            spawn_charge_read(shared, weak, d.port.clone(), false);
        }
    }
}

fn build_charge_cards(v: &ScanView, shared: &Arc<Mutex<Shared>>) -> Vec<ChargeCard> {
    let sh = lock(shared);
    let cfg = config::load();
    let mut cards = Vec::new();
    for d in &v.gud {
        let Some(m) = model::from_product(&d.product) else {
            continue;
        };
        let r = sh.charge_readings.get(&d.port);
        let busy = sh.charge_busy.contains(&d.port);
        let unique = unique_charge_device(v, m.slug).is_some();
        let tty = usb::maintenance_tty(&d.port).is_some();
        let target = cfg.as_ref().ok().map(|c| c.get(m).charge_limit);
        let submitted = r.and_then(|r| r.submitted.as_ref());
        let proof = submitted.map(|s| {
            if r.and_then(|r| r.sampled_at)
                .is_some_and(|t| t > s.submitted_at)
            {
                charge::proof(
                    m.slug,
                    &s.target,
                    s.device_uptime,
                    r.and_then(|r| r.j207_log.as_deref()),
                    r.and_then(|r| r.limit.as_ref())
                        .and_then(|v| v.as_ref().ok())
                        .map(String::as_str),
                )
            } else {
                charge::Proof::TargetSubmitted
            }
        });
        let (state, state_color, proof_line) = match proof {
            Some(charge::Proof::WatchdogWrite) => (
                "看门狗已写入",
                sh.palette.ok,
                "本次提交后出现新的匹配写入日志；不是寄存器回读".to_owned(),
            ),
            Some(charge::Proof::RegisterReadback) => (
                "寄存器已回读",
                sh.palette.ok,
                "本次提交后读回了匹配的 0x4C0 值".to_owned(),
            ),
            Some(charge::Proof::TargetSubmitted) => (
                "目标已提交",
                sh.palette.pending,
                "等待设备侧证据；目标值不代表已经生效".to_owned(),
            ),
            None if r.and_then(|r| r.sampled_at).is_some() => {
                if r.and_then(|r| r.limit.as_ref()).is_some_and(Result::is_err) {
                    (
                        "读取失败",
                        sh.palette.fail,
                        "设备侧状态暂不可用；可稍后刷新".to_owned(),
                    )
                } else {
                    (
                        "已采样",
                        sh.palette.ok,
                        "这是当前读数；尚未提交新目标".to_owned(),
                    )
                }
            }
            None => (
                "待读取",
                sh.palette.fallback,
                "选择档位后将分别显示目标和设备侧证据".to_owned(),
            ),
        };
        let action_error = r.and_then(|r| r.action_error.as_deref());
        let state = if action_error.is_some() {
            "需留意"
        } else if busy {
            "读取中"
        } else {
            state
        };
        let state_color = if action_error.is_some() {
            sh.palette.fail
        } else {
            state_color
        };
        let limit_line = match r.and_then(|r| r.limit.as_ref()) {
            Some(Ok(v)) => format!(
                "设备侧读数：{v} · 配置目标：{}",
                target.as_deref().unwrap_or("未知")
            ),
            Some(Err(e)) => format!("设备侧读取失败：{e}"),
            None => format!(
                "设备侧尚未读取 · 配置目标：{}",
                target.as_deref().unwrap_or("未知")
            ),
        };
        let battery_line = match r.and_then(|r| r.battery.as_ref()) {
            Some(Ok(Some(ma))) => format!(
                "平均电流：{ma:+} mA{}",
                if *ma > 0 { " · 充电中" } else { "" }
            ),
            Some(Ok(None)) => "此机型没有电量计脚本".to_owned(),
            Some(Err(e)) => format!("电流读取失败：{e}"),
            None => "电流尚未读取".to_owned(),
        };
        let proof_line = if let Some(e) = action_error {
            e.to_owned()
        } else if !unique {
            "同型号多台在线，无法唯一定位限流目标".to_owned()
        } else if !tty {
            "维护口未就绪，暂时无法改档".to_owned()
        } else {
            proof_line
        };
        let sampled_line = r
            .and_then(|r| r.sampled_at)
            .map(|t| format!("{} 秒前采样", t.elapsed().as_secs()))
            .unwrap_or_else(|| "等待首次采样".into());
        let choices = charge::choices(m.slug);
        let ch = |i: usize| choices.get(i).copied();
        let label = |i| ch(i).map(|c| c.label).unwrap_or("").into();
        let hex = |i| ch(i).map(|c| c.hex).unwrap_or("").into();
        let active = |i| ch(i).is_some_and(|c| target.as_deref() == Some(c.hex));
        cards.push(ChargeCard {
            slug: m.slug.into(),
            name: m.name.into(),
            port: d.port.clone().into(),
            state: state.into(),
            state_color: brush(state_color),
            limit_line: limit_line.into(),
            battery_line: battery_line.into(),
            proof_line: proof_line.into(),
            sampled_line: sampled_line.into(),
            option_a: label(0),
            option_a_hex: hex(0),
            option_b: label(1),
            option_b_hex: hex(1),
            option_c: label(2),
            option_c_hex: hex(2),
            active_a: active(0),
            active_b: active(1),
            active_c: active(2),
            can_set: unique && tty && !busy,
        });
    }
    cards
}

fn scan_loop(
    weak: Weak<OmapadWindow>,
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
        if !measure && let Some(root) = &root {
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
                            let still_same = v.dfu.iter().any(|x| {
                                x.port == port
                                    && model::from_dfu_serial(&x.serial)
                                        .is_some_and(|y| y.slug == m.slug)
                            });
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
                            if let Some(w) = weak.upgrade()
                                && w.get_status_line() == REFUSE_MULTI_TEXT
                            {
                                w.set_status_line("".into());
                            }
                        });
                    }
                }
            }
        }

        // ---- 刷新卡片 ----
        refresh_charge_jobs(&shared, &weak, &v);
        let generation = lock(&shared).style_generation;
        let items = build_cards(&v, &shared);
        let charge_open = lock(&shared).device_tab_open;
        let charges = charge_open.then(|| build_charge_cards(&v, &shared));
        if lock(&shared).style_generation != generation {
            continue;
        }
        let weak = weak.clone();
        let shared2 = shared.clone();
        if slint::invoke_from_event_loop(move || {
            if lock(&shared2).style_generation != generation {
                return;
            }
            let Some(w) = weak.upgrade() else { return };
            let current = w.get_devices();
            let updated = preserve_rows(current.clone(), items, |a, b| a.slug == b.slug);
            if updated != current {
                w.set_devices(updated);
            }
            if let Some(charges) = charges {
                w.set_charge_empty(charges.is_empty());
                w.set_charges(ModelRc::new(VecModel::from(charges)));
            }
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
    let mut revive = Vec::new();
    for d in &v.gud {
        let Some(m) = model::from_product(&d.product) else {
            continue;
        };
        let Some(c) = conns.iter().find(|c| c.usb_port == d.port) else {
            continue;
        };
        sh.connectors.insert(m.slug.to_owned(), c.clone());
        if baseline {
            // 首轮之后的新端口（含同端口重插）才套默认：亮度即时写；限流属 P1c
            if sh.seen_gud.insert(d.port.clone()) {
                let b = cfg.get(m).brightness;
                if let Err(e) = c.set_brightness(b) {
                    eprintln!("热插拔套默认亮度失败（{}）：{e:#}", m.slug);
                }
                revive.push(c.name.clone());
            }
        } else {
            // 首轮基线：只记录，不写
            sh.seen_gud.insert(d.port.clone());
        }
    }
    drop(sh);
    if !revive.is_empty() {
        // 要等 Hyprland 接管热插拔，放工作线程，不拖住扫描循环
        let shared = shared.clone();
        std::thread::spawn(move || revive_if_ghost_named(&shared, &revive));
    }
}

/// 新上线的 iPad 没出现在 Hyprland 启用列表里 → reload 一次。
///
/// ipadctl.lua 会按名字禁用残留空壳（见 hypr.rs `LUA_GHOST_BLOCK`），这条规则留到
/// 下次 reload。若内核把这个名字又分给了新接入的真屏，它会被压成禁用；reload 从
/// 文件重建规则，真屏重新吃 desc: 规则。reload 前先重新生成 lua：盘上可能是
/// 不带清理段的旧版文件，原样重放会把空壳一起点亮。
/// 每秒查一次、最多 5 次，给 Hyprland 接管热插拔的时间；全部启用就提前结束。
fn revive_if_ghost_named(shared: &Arc<Mutex<Shared>>, names: &[String]) {
    let mut missing: Vec<&String> = names.iter().collect();
    for _ in 0..5 {
        std::thread::sleep(Duration::from_secs(1));
        match hypr::hyprctl_monitors() {
            Ok(mons) => missing.retain(|n| !mons.iter().any(|m| &m.name == *n)),
            Err(e) => eprintln!("检查新屏是否启用：{e:#}"),
        }
        if missing.is_empty() {
            return;
        }
    }
    // 锁顺序同 touch_toggle / persist_layout：layout_mu → config_mu
    let layout_mu = lock(shared).layout_mu.clone();
    let _layout = lock(&layout_mu);
    let mu = lock(shared).config_mu.clone();
    let res = {
        let _g = lock(&mu);
        config::load().and_then(|cfg| hypr::write_lua(&cfg).map(|_| ()))
    };
    if let Err(e) = res.and_then(|()| hypr::reload_hyprland()) {
        eprintln!("{missing:?} 上线但未启用，重新生成并 reload 失败：{e:#}");
    }
}

/// 从 hyprctl 重建画布数据并推给 UI。hyprctl 不可用则隐藏画布。
fn refresh_canvas(shared: &Arc<Mutex<Shared>>, weak: &Weak<OmapadWindow>) {
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
            id: md
                .map(|md| md.slug.to_owned())
                .unwrap_or_else(|| m.name.clone()),
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
        if e.is_ipad
            && entries
                .iter()
                .skip(i + 1)
                .any(|o| o.is_ipad && o.id == e.id)
        {
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
            w.set_status_line(
                "⚠ 同型号多台 iPad 的 EDID desc 相同，画布无法区分（已知限制）".into(),
            );
        }
    });
}

impl CanvasEntry {
    /// 临时桥接：CanvasItem 的 mid 字段。
    fn mid(&self) -> slint::SharedString {
        self.id.clone().into()
    }
}

/// 松手：吸附与重叠校验 → 存配置 → 写 lua → reload → 回读。运行在工作线程。
/// 全程持 layout_mu：校验用的画布、写盘、重载、回读一气呵成，串行化所有布局操作（评审 🔴6）。
fn canvas_moved(
    shared: Arc<Mutex<Shared>>,
    weak: Weak<OmapadWindow>,
    slug: String,
    x: f64,
    y: f64,
    scale: f64,
) {
    let layout_mu = lock(&shared).layout_mu.clone();
    let _layout = lock(&layout_mu);
    let Some(entry) = lock(&shared).canvas.iter().find(|e| e.id == slug).cloned() else {
        return;
    };
    if !entry.is_ipad {
        return;
    }
    let others = canvas_other_rects(&shared, &slug);
    let (x, y) = layout::snap_position(
        layout::Rect::new(x, y, entry.w, entry.h),
        &others,
        snap_tolerance(scale),
        false,
    );
    let new_rect = (x.round(), y.round(), entry.w, entry.h);
    if let Some(other) = overlap_with(&shared, &slug, new_rect) {
        set_status(
            &weak,
            format!("⚠ {} 与 {} 重叠，未保存", entry.label, other),
        );
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

/// 旋转所选：0→1→3→2 循环，中心锚定，贴边吸附后校验重叠。串行（layout_mu）。
fn canvas_rotate(shared: Arc<Mutex<Shared>>, weak: Weak<OmapadWindow>, slug: String, scale: f64) {
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
    let (nx, ny) = layout::snap_position(
        layout::Rect::new(cx - nw / 2.0, cy - nh / 2.0, nw, nh),
        &canvas_other_rects(&shared, &slug),
        snap_tolerance(scale),
        true,
    );
    if let Some(other) = overlap_with(&shared, &slug, (nx.round(), ny.round(), nw, nh)) {
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

fn snap_tolerance(scale: f64) -> f64 {
    if scale.is_finite() && scale > 0.0 {
        12.0 / scale
    } else {
        0.0
    }
}

fn canvas_other_rects(shared: &Arc<Mutex<Shared>>, slug: &str) -> Vec<layout::Rect> {
    lock(shared)
        .canvas
        .iter()
        .filter(|e| e.id != slug)
        .map(|e| layout::Rect::new(e.x, e.y, e.w, e.h))
        .collect()
}

/// 分辨率两档：原生 = preferred（mode 置 None），半分辨率 = EDID 窄档。串行（layout_mu）。
fn mode_set(shared: Arc<Mutex<Shared>>, weak: Weak<OmapadWindow>, slug: String, half: bool) {
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

fn touch_toggle(shared: Arc<Mutex<Shared>>, weak: Weak<OmapadWindow>, slug: String, enabled: bool) {
    let layout_mu = lock(&shared).layout_mu.clone();
    let _layout = lock(&layout_mu);

    if let Err(e) = modify_config(&shared, |c| {
        if let Some(m) = model::by_slug(&slug) {
            c.entry_or_default(m).touch_enabled = enabled;
        }
    }) {
        set_status(&weak, format!("⚠ 写配置失败：{e:#}"));
        return;
    }

    match persist_layout(&shared, &weak) {
        Ok(()) => {
            let text = if enabled {
                "✓ 触摸已启用并生效"
            } else {
                "✓ 触摸已禁用并生效"
            };
            set_status_transient(&weak, text.into());
        }
        Err(e) => set_status(&weak, format!("⚠ 布局生效失败：{e:#}")),
    }
}

/// 与其它画布块的重叠检查，返回撞到的块名。
fn overlap_with(
    shared: &Arc<Mutex<Shared>>,
    slug: &str,
    rect: (f64, f64, f64, f64),
) -> Option<String> {
    lock(shared)
        .canvas
        .iter()
        .filter(|e| e.id != slug)
        .find(|e| hypr::rects_overlap(rect, (e.x, e.y, e.w, e.h)))
        .map(|e| e.label.clone())
}

/// 写 lua → hyprctl reload → 等 600 ms → 回读画布。
/// 这版 hyprctl 没有 currentMode 字段，模式切换的效果读不回来 —— 如实标注。
fn persist_layout(shared: &Arc<Mutex<Shared>>, weak: &Weak<OmapadWindow>) -> anyhow::Result<()> {
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

fn set_status(weak: &Weak<OmapadWindow>, text: String) {
    let weak = weak.clone();
    let _ = slint::invoke_from_event_loop(move || {
        if let Some(w) = weak.upgrade() {
            w.set_status_line(text.into());
        }
    });
}

/// 成功类消息：5 秒后自动淡出（失败类用 set_status 保留）。
/// 清除前比对文本：期间出现了更新的消息（如投放失败）就不动它（评审 🔴9）。
fn set_status_transient(weak: &Weak<OmapadWindow>, text: String) {
    set_status(weak, text.clone());
    let weak2 = weak.clone();
    let _ = slint::invoke_from_event_loop(move || {
        let w3 = weak2.clone();
        slint::Timer::single_shot(Duration::from_secs(5), move || {
            if let Some(w) = w3.upgrade()
                && w.get_status_line() == text.as_str()
            {
                w.set_status_line("".into());
            }
        });
    });
}

fn empty_card(p: &theme::Palette) -> DeviceCard {
    DeviceCard {
        slug: "".into(),
        name: "".into(),
        state: "".into(),
        port: "".into(),
        detail: "".into(),
        state_color: brush(p.fail),
        progress: "".into(),
        action_label: "".into(),
        brightness: -1.0,
        has_brightness: false,
        mode_native: "".into(),
        mode_half: "".into(),
        mode_half_on: false,
        has_mode: false,
        has_touch: false,
        touch_enabled: false,
    }
}

fn build_cards(v: &ScanView, shared: &Arc<Mutex<Shared>>) -> Vec<DeviceCard> {
    let sh = lock(shared);
    let p = &sh.palette;
    let cfg = config::load().unwrap_or_default();
    let mut out: Vec<DeviceCard> = Vec::new();

    for d in &v.dfu {
        let mut c = empty_card(p);
        c.port = d.port.clone().into();
        match model::from_dfu_serial(&d.serial) {
            Some(m) => {
                c.slug = m.slug.into();
                c.name = m.name.into();
                c.detail = format!("CPID/BDID 已识别（{}）", m.slug).into();
                if let Some(f) = sh.flash.as_ref().filter(|f| f.port == d.port) {
                    c.state = "自动连接中".into();
                    c.state_color = brush(p.pending);
                    c.progress = f.step.clone().into();
                    c.detail = "投放进行中，期间扫描与其它设备不受影响".into();
                } else if let Some(fa) = sh.failures.get(&d.port) {
                    c.state = "自动连接失败".into();
                    c.state_color = brush(p.fail);
                    c.progress = fa.reason.clone().into();
                    c.detail = "同一台不会自动重试；重试将重跑全部预检".into();
                    if fa.retryable {
                        c.action_label = "重试".into();
                    }
                } else {
                    c.state = if sh.auto_connect {
                        "DFU 待投放"
                    } else {
                        "DFU（自动已关）"
                    }
                    .into();
                    c.state_color = brush(p.pending);
                }
            }
            None => {
                c.name = "未知设备".into();
                c.state = "拒绝投放".into();
                c.detail = format!("serial: {}", d.serial).into();
                c.state_color = brush(p.fail);
            }
        }
        out.push(c);
    }

    for d in &v.gud {
        let mut c = empty_card(p);
        c.port = d.port.clone().into();
        match model::from_product(&d.product) {
            Some(m) => {
                c.slug = m.slug.into();
                c.name = m.name.into();
                c.state = "在线".into();
                c.state_color = brush(p.ok);
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
                // 触摸支持
                if ipadctl::touch::model_supports_touch(m) {
                    c.has_touch = true;
                    c.touch_enabled = cfg.get(m).touch_enabled;
                }
            }
            None => {
                c.name = "未知设备".into();
                c.state = "产品串不认识".into();
                c.detail = format!("product: {:?}", d.product).into();
                c.state_color = brush(p.fail);
            }
        }
        out.push(c);
    }
    for d in &v.uart {
        let mut c = empty_card(p);
        c.name = "引导失败".into();
        c.state = "m1n1 uartproxy".into();
        c.port = d.port.clone().into();
        c.detail = "m1n1 起来了但内核没有引导（DTB 线索见 gud/dtb/README.md）".into();
        c.state_color = brush(p.fail);
        out.push(c);
    }
    for d in &v.acm {
        let mut c = empty_card(p);
        c.name = d.product.clone().into();
        c.state = "仅维护口".into();
        c.port = d.port.clone().into();
        c.detail = "GUD 失败的退路（2×ACM）".into();
        c.state_color = brush(p.fallback);
        out.push(c);
    }
    out.sort_by(|a, b| a.port.cmp(&b.port));
    out
}

fn spawn_flash(
    shared: Arc<Mutex<Shared>>,
    weak: Weak<OmapadWindow>,
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
fn start_measure(weak: Weak<OmapadWindow>) {
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

#[cfg(test)]
mod charge_routing_tests {
    use super::*;

    #[test]
    fn duplicate_model_cannot_be_selected_by_slug() {
        let device = |port: &str| usb::UsbDevice {
            port: port.to_owned(),
            vid: "1d50".into(),
            pid: "614d".into(),
            product: model::by_slug("a1599").unwrap().product.into(),
            serial: String::new(),
        };
        let mut v = ScanView {
            dfu: vec![],
            gud: vec![device("1-1")],
            uart: vec![],
            acm: vec![],
        };
        assert_eq!(
            unique_charge_device(&v, "a1599").map(|d| d.port.as_str()),
            Some("1-1")
        );
        v.gud.push(device("1-2"));
        assert!(unique_charge_device(&v, "a1599").is_none());
        assert!(unique_charge_device(&v, "j207").is_none());
    }
}
