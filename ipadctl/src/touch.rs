//! iPad 触摸屏校准与配置推送（仅 J207）。
//!
//! 链路：设备端 touchd 读 HBPP 报告 → 解码 → 写 /dev/hidg0 → 宿主 hid-multitouch →
//! libinput → Hyprland。touchd 启动时从 `/run/touchd.args` 读取校准参数 `-c a,b,c,d,e,f`，
//! 运行中无法热更新；修改后需 kill 让 touch-init 重启它。
//!
//! 校准方案：Omapad 全屏显示靶点 → 采集触点坐标 → 拟合修正矩阵 → 写 args 文件 → 重启 touchd。

use crate::acm::Acm;
use crate::model::Model;
use anyhow::{bail, Context, Result};
use std::time::Duration;

/// touchd 内置默认校准矩阵（横屏，充电口朝右）。
/// 来自 `.scratch/touch-j207/hid/touchd.c` 的 `CAL_*` 宏。
pub const TOUCH_CAL_DEFAULT: [f64; 6] = [
    -4.46717e-08,
    -4.69102e-06,
    0.993619,
    -6.24276e-06,
    -1.30029e-08,
    0.984553,
];

/// 仿射变换：`u = a·x + b·y + c, v = d·x + e·y + f`。
/// touchd 的输入是原始坐标 (x,y)，输出是映射后的 HID 报告 (u,v)。
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Affine {
    pub a: f64,
    pub b: f64,
    pub c: f64,
    pub d: f64,
    pub e: f64,
    pub f: f64,
}

impl Affine {
    pub fn from_slice(s: &[f64; 6]) -> Self {
        Self {
            a: s[0],
            b: s[1],
            c: s[2],
            d: s[3],
            e: s[4],
            f: s[5],
        }
    }

    pub fn to_array(&self) -> [f64; 6] {
        [self.a, self.b, self.c, self.d, self.e, self.f]
    }

    /// 解析 touchd 参数格式：`-c a,b,c,d,e,f`（六个浮点数，用逗号分隔）。
    pub fn parse_args(s: &str) -> Result<Self> {
        let s = s.trim().trim_start_matches("-c").trim();
        let parts: Vec<&str> = s.split(',').collect();
        if parts.len() != 6 {
            bail!("校准参数应为 6 个浮点数，收到 {}", parts.len());
        }
        let nums: Result<Vec<f64>> = parts.iter().map(|p| p.trim().parse().context("解析浮点数")).collect();
        let nums = nums?;
        Ok(Self::from_slice(&[nums[0], nums[1], nums[2], nums[3], nums[4], nums[5]]))
    }

    /// 格式化为 touchd 参数：`-c a,b,c,d,e,f`（科学计数法，6 位有效数字）。
    pub fn to_args(&self) -> String {
        format!(
            "-c {:.6e},{:.6e},{:.6e},{:.6e},{:.6e},{:.6e}",
            self.a, self.b, self.c, self.d, self.e, self.f
        )
    }

    /// 应用变换：`(x, y) → (u, v)`。
    pub fn apply(&self, x: f64, y: f64) -> (f64, f64) {
        (
            self.a * x + self.b * y + self.c,
            self.d * x + self.e * y + self.f,
        )
    }

    /// 复合两个变换：先 self，再 other。返回 `other ∘ self`。
    pub fn compose(&self, other: &Affine) -> Affine {
        // (x,y) --self--> (u,v) --other--> (u',v')
        // u = a·x + b·y + c
        // v = d·x + e·y + f
        // u' = A·u + B·v + C = A·(a·x + b·y + c) + B·(d·x + e·y + f) + C
        // v' = D·u + E·v + F = D·(a·x + b·y + c) + E·(d·x + e·y + f) + F
        Affine {
            a: other.a * self.a + other.b * self.d,
            b: other.a * self.b + other.b * self.e,
            c: other.a * self.c + other.b * self.f + other.c,
            d: other.d * self.a + other.e * self.d,
            e: other.d * self.b + other.e * self.e,
            f: other.d * self.c + other.e * self.f + other.f,
        }
    }
}

impl Default for Affine {
    fn default() -> Self {
        Self::from_slice(&TOUCH_CAL_DEFAULT)
    }
}

/// 从观测点对拟合仿射变换（最小二乘）。
/// `points` 是 `[(observed_x, observed_y, target_x, target_y), ...]`。
/// 最少需要 3 个点（6 个方程求解 6 个未知数）。
pub fn fit_affine(points: &[(f64, f64, f64, f64)]) -> Result<Affine> {
    if points.len() < 3 {
        bail!("至少需要 3 个点，收到 {}", points.len());
    }
    // 最小二乘正规方程：解 A·x = b，其中 A 是 [x y 1] 的累积矩阵，b 是目标坐标。
    // 分 u 和 v 两个方向分别求解。
    let n = points.len() as f64;
    let mut sum_x = 0.0;
    let mut sum_y = 0.0;
    let mut sum_xx = 0.0;
    let mut sum_yy = 0.0;
    let mut sum_xy = 0.0;
    let mut sum_u = 0.0;
    let mut sum_v = 0.0;
    let mut sum_xu = 0.0;
    let mut sum_yu = 0.0;
    let mut sum_xv = 0.0;
    let mut sum_yv = 0.0;

    for &(x, y, u, v) in points {
        sum_x += x;
        sum_y += y;
        sum_xx += x * x;
        sum_yy += y * y;
        sum_xy += x * y;
        sum_u += u;
        sum_v += v;
        sum_xu += x * u;
        sum_yu += y * u;
        sum_xv += x * v;
        sum_yv += y * v;
    }

    // 系数矩阵 A（对称）
    let a11 = sum_xx;
    let a12 = sum_xy;
    let a13 = sum_x;
    let a22 = sum_yy;
    let a23 = sum_y;
    let a33 = n;

    // 右侧向量 b_u 和 b_v
    let bu1 = sum_xu;
    let bu2 = sum_yu;
    let bu3 = sum_u;
    let bv1 = sum_xv;
    let bv2 = sum_yv;
    let bv3 = sum_v;

    // Cholesky 分解求解（对称正定矩阵）
    let solve = |b1: f64, b2: f64, b3: f64| -> Result<(f64, f64, f64)> {
        // 增广矩阵高斯消元（简化版）
        let det = a11 * (a22 * a33 - a23 * a23)
            - a12 * (a12 * a33 - a23 * a13)
            + a13 * (a12 * a23 - a22 * a13);
        if det.abs() < 1e-10 {
            bail!("矩阵奇异，无法求解（点可能共线）");
        }
        // Cramer 法则（3×3 直接解）
        let x = (b1 * (a22 * a33 - a23 * a23)
            - a12 * (b2 * a33 - a23 * b3)
            + a13 * (b2 * a23 - a22 * b3))
            / det;
        let y = (a11 * (b2 * a33 - a23 * b3)
            - b1 * (a12 * a33 - a23 * a13)
            + a13 * (a12 * b3 - b2 * a13))
            / det;
        let z = (a11 * (a22 * b3 - b2 * a23)
            - a12 * (a12 * b3 - b2 * a13)
            + b1 * (a12 * a23 - a22 * a13))
            / det;
        Ok((x, y, z))
    };

    let (a, b, c) = solve(bu1, bu2, bu3)?;
    let (d, e, f) = solve(bv1, bv2, bv3)?;
    Ok(Affine { a, b, c, d, e, f })
}

/// 把校准参数推送到设备并重启 touchd。
/// 写 `/run/touchd.args`，然后 `killall touchd` 让 touch-init 重启它。
pub fn push_calibration(acm: &mut Acm, cal: &Affine) -> Result<()> {
    let args = cal.to_args();
    let (_, rc) = acm.run(
        &format!("printf '%s' '{}' > /run/touchd.args", args),
        Duration::from_secs(10),
    )?;
    if rc != 0 {
        bail!("写 /run/touchd.args 失败（设备端退出码 {rc}）");
    }
    // killall 返回非 0 时（进程不存在）不算失败 —— 可能还在 firmware() 阶段
    let _ = acm.run("killall touchd", Duration::from_secs(5));
    Ok(())
}

/// 读回当前生效的校准参数。失败时返回默认值。
pub fn read_calibration(acm: &mut Acm) -> Affine {
    let (out, rc) = acm
        .run("cat /run/touchd.args", Duration::from_secs(5))
        .unwrap_or_default();
    if rc != 0 || out.trim().is_empty() {
        return Affine::default();
    }
    Affine::parse_args(&out).unwrap_or_default()
}

/// 机型是否支持触摸（目前只有 J207）。
pub fn model_supports_touch(m: &Model) -> bool {
    m.slug == "j207"
}

/// 触摸设备的 HID 名称（libinput / Hyprland device 规则用）。
///
/// **这是 libinput 清洗后的名字，不是内核原始名**，别照抄 `/proc/bus/input/devices`。
/// 设备端 setup-gadget 报的产品串是 `Linux iPad Pro 10.5 GUD Display`，经过
/// libinput 规范化（转小写、空格与点号换连字符）后才是这个名字；`hyprctl devices`
/// 显示的就是清洗后的那一份，`hl.device` 也只认它。旁证：同机的
/// `logitech-usb-receiver-mouse` 同样是清洗后的形态（`~/.config/hypr/input.lua`）。
///
/// 判断依据（2026-10-17 实测）：内核侧 `/proc/bus/input/devices` 为
/// `N: Name="Linux iPad Pro 10.5 GUD Display"`，Hyprland 侧为
/// `Touch Device at 58420e209800: linux-ipad-pro-10.5-gud-display`。
pub fn touch_device_name(m: &Model) -> Option<&'static str> {
    match m.slug {
        "j207" => Some("linux-ipad-pro-10.5-gud-display"),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn 默认校准往返() {
        let cal = Affine::default();
        let args = cal.to_args();
        let back = Affine::parse_args(&args).unwrap();
        assert!((cal.a - back.a).abs() < 1e-10);
        assert!((cal.f - back.f).abs() < 1e-10);
    }

    #[test]
    fn 三点拟合恒等变换() {
        // (x,y) → (x,y) 应拟合出单位矩阵
        let points = vec![(0.0, 0.0, 0.0, 0.0), (1.0, 0.0, 1.0, 0.0), (0.0, 1.0, 0.0, 1.0)];
        let cal = fit_affine(&points).unwrap();
        assert!((cal.a - 1.0).abs() < 1e-6);
        assert!(cal.b.abs() < 1e-6);
        assert!(cal.c.abs() < 1e-6);
        assert!(cal.d.abs() < 1e-6);
        assert!((cal.e - 1.0).abs() < 1e-6);
        assert!(cal.f.abs() < 1e-6);
    }

    #[test]
    fn 拟合带偏移和缩放() {
        // (x,y) → (2x+10, 3y+20)
        let points = vec![
            (0.0, 0.0, 10.0, 20.0),
            (1.0, 0.0, 12.0, 20.0),
            (0.0, 1.0, 10.0, 23.0),
            (5.0, 7.0, 20.0, 41.0),
        ];
        let cal = fit_affine(&points).unwrap();
        let (u, v) = cal.apply(5.0, 7.0);
        assert!((u - 20.0).abs() < 1e-6);
        assert!((v - 41.0).abs() < 1e-6);
    }

    #[test]
    fn 共线点拒绝() {
        // 三个点在一条线上
        let points = vec![(0.0, 0.0, 0.0, 0.0), (1.0, 1.0, 1.0, 1.0), (2.0, 2.0, 2.0, 2.0)];
        assert!(fit_affine(&points).is_err());
    }

    #[test]
    fn 变换复合() {
        // 先平移 (x,y) → (x+1, y+2)，再缩放 (u,v) → (2u, 3v)
        let t1 = Affine {
            a: 1.0,
            b: 0.0,
            c: 1.0,
            d: 0.0,
            e: 1.0,
            f: 2.0,
        };
        let t2 = Affine {
            a: 2.0,
            b: 0.0,
            c: 0.0,
            d: 0.0,
            e: 3.0,
            f: 0.0,
        };
        let t = t1.compose(&t2);
        let (u, v) = t.apply(5.0, 7.0);
        assert!((u - 12.0).abs() < 1e-6); // 2*(5+1)
        assert!((v - 27.0).abs() < 1e-6); // 3*(7+2)
    }

    #[test]
    fn j207_支持触摸() {
        let m = crate::model::by_slug("j207").unwrap();
        assert!(model_supports_touch(m));
        assert_eq!(touch_device_name(m), Some("linux-ipad-pro-10.5-gud-display"));
        let m2 = crate::model::by_slug("a1599").unwrap();
        assert!(!model_supports_touch(m2));
    }
}
