//! ipadctl core 库：CLI（本包 bin）与 GUI（crates/gui 的 omapad 包）共用。
//!
//! 设备发现（usb/kmsg/model）、DFU 投放（flash）、设备侧维护与充电（acm/device）、
//! 配置与 Hyprland 布局（config/hypr/paths）、触摸校准（touch）。模块保持原平铺结构，
//! 拆分历史见 DESIGN.md §3。

pub mod acm;
pub mod config;
pub mod device;
pub mod flash;
pub mod hypr;
pub mod kmsg;
pub mod model;
pub mod paths;
pub mod touch;
pub mod usb;
