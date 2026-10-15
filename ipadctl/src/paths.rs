//! 用户侧路径。
//!
//! ⚠️ **不能在 sudo 下直接用 `$HOME`** —— 它会是 `/root`，配置和 hypr 文件就被写到
//! root 家目录去了（实测踩过）。本工具有些操作需要 root（刷机），所以必须显式处理：
//! 有 `SUDO_USER` 就按它去 `/etc/passwd` 查真正的家目录。

use anyhow::{bail, Context, Result};
use std::path::PathBuf;

/// 调用者的家目录（穿透 sudo）。
pub fn home_dir() -> Result<PathBuf> {
    if let Some(user) = std::env::var_os("SUDO_USER") {
        let user = user.to_string_lossy().into_owned();
        if user != "root" {
            match home_of(&user) {
                Some(h) => return Ok(h),
                None => bail!("SUDO_USER={user} 在 /etc/passwd 里查不到家目录"),
            }
        }
    }
    let home = std::env::var_os("HOME").context("环境变量 HOME 未设置")?;
    Ok(PathBuf::from(home))
}

/// 从 /etc/passwd 取某个用户的家目录。字段 6（1 起算）。
fn home_of(user: &str) -> Option<PathBuf> {
    let text = std::fs::read_to_string("/etc/passwd").ok()?;
    for line in text.lines() {
        let f: Vec<&str> = line.split(':').collect();
        if f.len() >= 6 && f[0] == user {
            return Some(PathBuf::from(f[5]));
        }
    }
    None
}

pub fn config_path() -> Result<PathBuf> {
    Ok(home_dir()?.join(".config/ipadctl/config.toml"))
}

/// 本工具完全拥有的 Hyprland 布局文件。
pub fn lua_path() -> Result<PathBuf> {
    Ok(home_dir()?.join(".config/hypr/ipadctl.lua"))
}

/// 用户手写的 Hyprland 配置，我们只在里面加一行 `require`。
pub fn monitors_lua_path() -> Result<PathBuf> {
    Ok(home_dir()?.join(".config/hypr/monitors.lua"))
}

/// 从可执行文件往上找含 `gud/launch/` 的目录（供 CLI 与 GUI 共用）。
///
/// 认 `gud/launch/alpine_dfu_wrapper.py` 而不是 `gud/`，避免 `.scratch` 里的同名目录。
/// 注意：从 GUI/CLI 二进制出发通常上溯 3~4 层能到仓库根；装到 PATH 之外的
/// 其他位置时要靠 `IPADCTL_ROOT` 或 `--root` 指定。
pub fn project_root() -> Result<PathBuf> {
    let exe = std::env::current_exe().context("取当前可执行文件路径失败")?;
    for dir in exe.ancestors() {
        if dir.join("gud/launch/alpine_dfu_wrapper.py").is_file() {
            return Ok(dir.to_owned());
        }
    }
    bail!(
        "从 {} 往上找不到仓库根目录（要含 gud/launch/alpine_dfu_wrapper.py）；用 --root 指定",
        exe.display()
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn 非_sudo_时用_HOME() {
        // 测试进程通常没有 SUDO_USER
        if std::env::var_os("SUDO_USER").is_none() {
            assert_eq!(home_dir().unwrap(), PathBuf::from(std::env::var("HOME").unwrap()));
        }
    }

    #[test]
    fn 能从_passwd_里查到本用户的家目录() {
        let me = std::env::var("USER").unwrap_or_default();
        if !me.is_empty() {
            let h = home_of(&me);
            assert!(h.is_some(), "查不到 {me} 的家目录");
            assert!(h.unwrap().is_absolute());
        }
    }

    #[test]
    fn 查不存在的用户返回_none() {
        assert!(home_of("绝对不存在的用户zzz").is_none());
    }
}
