//! 画布拖动和旋转后的显示器吸附。坐标均为 Hyprland 逻辑像素。

#[derive(Clone, Copy, Debug)]
pub struct Rect {
    pub x: f64,
    pub y: f64,
    pub w: f64,
    pub h: f64,
}

impl Rect {
    pub const fn new(x: f64, y: f64, w: f64, h: f64) -> Self {
        Self { x, y, w, h }
    }

    fn at(self, x: f64, y: f64) -> Self {
        Self { x, y, ..self }
    }

    fn overlaps(self, other: Self) -> bool {
        self.x < other.x + other.w
            && self.x + self.w > other.x
            && self.y < other.y + other.h
            && self.y + self.h > other.y
    }
}

#[derive(Clone, Copy)]
struct AxisOption {
    value: f64,
    snapped: bool,
}

/// 返回松手/旋转后的位置。靠近时沿两个轴贴边或对齐；旋转中心锚定撞到
/// 显示器时，允许移到最近的可用边。结果始终要再经调用方做重叠校验。
pub fn snap_position(rect: Rect, others: &[Rect], tolerance: f64, rotate: bool) -> (f64, f64) {
    let force = rotate && others.iter().any(|o| rect.overlaps(*o));
    let mut xs = vec![AxisOption {
        value: rect.x,
        snapped: false,
    }];
    let mut ys = vec![AxisOption {
        value: rect.y,
        snapped: false,
    }];
    for o in others {
        let x_positions = [
            o.x - rect.w,
            o.x + o.w,
            o.x,
            o.x + o.w - rect.w,
            o.x + (o.w - rect.w) / 2.0,
        ];
        let y_positions = [
            o.y - rect.h,
            o.y + o.h,
            o.y,
            o.y + o.h - rect.h,
            o.y + (o.h - rect.h) / 2.0,
        ];
        for (i, value) in x_positions.into_iter().enumerate() {
            if (value - rect.x).abs() <= tolerance || (force && i < 2) {
                xs.push(AxisOption {
                    value,
                    snapped: true,
                });
            }
        }
        for (i, value) in y_positions.into_iter().enumerate() {
            if (value - rect.y).abs() <= tolerance || (force && i < 2) {
                ys.push(AxisOption {
                    value,
                    snapped: true,
                });
            }
        }
    }

    let mut best = None::<((f64, f64), usize, f64)>;
    for x in xs {
        for y in &ys {
            if !x.snapped && !y.snapped {
                continue;
            }
            let (px, py) = (x.value.round(), y.value.round());
            if others.iter().any(|o| rect.at(px, py).overlaps(*o)) {
                continue;
            }
            let count = usize::from(x.snapped) + usize::from(y.snapped);
            let distance = (px - rect.x).hypot(py - rect.y);
            let preferred = match best {
                None => true,
                Some((_, old_count, old_distance)) if force => {
                    distance < old_distance || (distance == old_distance && count > old_count)
                }
                Some((_, old_count, old_distance)) => {
                    count > old_count || (count == old_count && distance < old_distance)
                }
            };
            if preferred {
                best = Some(((px, py), count, distance));
            }
        }
    }
    best.map(|(pos, _, _)| pos).unwrap_or((rect.x, rect.y))
}

#[cfg(test)]
mod tests {
    use super::*;

    const HOST: Rect = Rect::new(0.0, 0.0, 1920.0, 1080.0);

    #[test]
    fn drag_near_host_edge_snaps_to_edge_and_top() {
        let (x, y) = snap_position(Rect::new(1928.0, 6.0, 600.0, 800.0), &[HOST], 12.0, false);
        assert_eq!((x, y), (1920.0, 0.0));
    }

    #[test]
    fn drag_far_from_edge_keeps_position() {
        let pos = snap_position(Rect::new(2000.0, 100.0, 600.0, 800.0), &[HOST], 12.0, false);
        assert_eq!(pos, (2000.0, 100.0));
    }

    #[test]
    fn rotate_out_of_overlap_finds_nearest_valid_edge() {
        let (x, y) = snap_position(Rect::new(1800.0, 200.0, 800.0, 600.0), &[HOST], 12.0, true);
        assert_eq!((x, y), (1920.0, 200.0));
    }

    #[test]
    fn snap_does_not_move_into_another_monitor() {
        let blocker = Rect::new(1920.0, 0.0, 100.0, 1080.0);
        let (x, y) = snap_position(
            Rect::new(2028.0, 0.0, 600.0, 800.0),
            &[HOST, blocker],
            12.0,
            false,
        );
        assert_eq!((x, y), (2020.0, 0.0));
    }
}
