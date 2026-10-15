//! 已上线设备的**设备侧**状态：充电限流、电量计。
//!
//! 亮度不在这里 —— 它必须走**宿主**的 `card*-USB-*-backlight`。
//! 直接写设备 sysfs 会让 gudd 内部的 `bl_pct` 变陈旧，下一次 DPMS 开关就被覆盖回去。

use crate::acm::Acm;
use crate::model::Model;
use anyhow::{Context, Result, bail};
use std::time::Duration;

/// Last raw J207 watchdog write record, including its kernel timestamp.
/// The GUI compares this with the pre-submit record; a value alone could
/// describe an old write and must not confirm a new limit request.
pub fn watchdog_last_write(acm: &mut Acm) -> Result<Option<String>> {
    let (out, rc) = acm.run(
        "dmesg | grep 'd2355: write 0x044e' | tail -1",
        Duration::from_secs(15),
    )?;
    if rc != 0 {
        return Ok(None);
    }
    Ok(out
        .lines()
        .rev()
        .find(|line| line.contains("d2355: write 0x044e"))
        .map(|s| s.trim().to_owned()))
}

/// 读回**实际生效**的充电限流。
///
/// 不去读 `/run/*-limit` —— 那个文件**不存在是正常的**：两个 watchdog 都只是
/// "读不到就用内置默认"，自己从不创建它，所以文件缺失不代表没在充电。
/// 真正生效的值要问硬件：
///
/// - **A7**（D2089）：`rawr 0 0x3c 04 c0 :1` 读 0x4C0，结果落在
///   `/sys/kernel/i2cprobe/out`，形如 `  read[1]: bf`
/// - **J207**（D2355）：读回只进 dmesg（没有 out 文件），且 `r` 命令的语法没摸清
///   （实测 `r 0x44e 0x1` 被模块以 EINVAL 拒收）。但 watchdog 每 30 s 写一次，
///   dmesg 里的 `d2355: write 0x044e len=1 ok: 64` 就是最直接的生效证据。
pub fn charge_limit_effective(acm: &mut Acm, m: &Model) -> Result<String> {
    if m.slug == "j207" {
        let (out, rc) = acm.run(
            "dmesg | grep 'd2355: write 0x044e' | tail -1",
            Duration::from_secs(15),
        )?;
        if rc != 0 || out.trim().is_empty() {
            bail!("dmesg 里没有 0x44e 的写入记录 —— 看门狗可能没在跑");
        }
        let v = out
            .rsplit("ok: ")
            .next()
            .and_then(|s| s.lines().next())
            .map(str::trim)
            .unwrap_or("?");
        let ma = u32::from_str_radix(v, 16).map(|x| x * 10);
        return Ok(match ma {
            Ok(ma) => format!("0x{v}（{ma} mA）"),
            Err(_) => format!("0x{v}"),
        });
    }

    let (out, rc) = acm.run(
        "printf 'rawr 0 0x3c 04 c0 :1\\n' > /sys/kernel/i2cprobe/cmd; sleep 0.3; cat /sys/kernel/i2cprobe/out",
        Duration::from_secs(15),
    )?;
    if rc != 0 {
        bail!("读 0x4C0 失败（设备端退出码 {rc}）");
    }
    // 输出形如：
    //   raw bus 0 addr 0x3c write[2]: 04 c0
    //     read[1]: bf
    // 后面还可能跟着设备端 shell 的提示符（`~ #`，出现时机不定），
    // 所以只取紧跟标记的**第一行**，不做整体 trim。
    let v = out
        .rsplit("read[1]:")
        .next()
        .and_then(|s| s.lines().next())
        .map(str::trim)
        .unwrap_or("");
    if v.len() == 2 && v.chars().all(|c| c.is_ascii_hexdigit()) {
        Ok(format!("0x{v}"))
    } else {
        bail!("回读格式不认识：{:?}", out.trim())
    }
}

/// 设置充电限流。写的是 watchdog 的**目标文件**，让 watchdog 去写寄存器 ——
/// 避免和它每 30 s 的写入打架。最迟 30 s 生效。
pub fn set_charge_limit(acm: &mut Acm, m: &Model, hex: &str) -> Result<()> {
    let v = hex.trim_start_matches("0x");
    if v.is_empty() || !v.chars().all(|c| c.is_ascii_hexdigit()) || v.len() > 2 {
        bail!("限流值应当是 1~2 位十六进制，收到 {hex:?}");
    }
    let (_, rc) = acm.run(
        &format!("printf %s {v} > {}", m.charge_limit_file),
        Duration::from_secs(10),
    )?;
    if rc != 0 {
        bail!("写 {} 失败（设备端退出码 {rc}）", m.charge_limit_file);
    }
    Ok(())
}

/// 读电量计的平均电流（mA）。**正数 = 在充电**，这是判据。
///
/// 返回 `Ok(None)` 表示这台**没有**电量计（J207）；读失败走 `Err`，不要混为一谈。
///
/// ⚠️ grep 必须带**尾随空格**：寄存器值里会出现 `0x1465` 这种含 `0x14` 的数字，
/// 不加空格会误匹配到 0x10 那行（实测踩过）。
pub fn battery_current(acm: &mut Acm, m: &Model) -> Result<Option<i32>> {
    if m.slug == "j207" {
        return Ok(None);
    }
    let (out, rc) = acm.run(
        "/sbin/battery 3 2 2>&1 | grep '0x14 '",
        Duration::from_secs(60),
    )?;
    if rc != 0 {
        bail!("设备端退出码 {rc}：{}", out.trim());
    }
    // 三次采样各一行，形如：
    //   `  0x14 平均电流(mA) = 0x0116 (   278)  [5/5]`
    // 取第一行即可。
    let line = out.lines().next().unwrap_or_default().to_owned();
    let ma = last_paren_int(&line).with_context(|| format!("电流格式不认识：{line:?}"))?;
    Ok(Some(ma))
}

/// 取行里**最后一个能解析成整数的括号组**。
///
/// 不能简单地按第一个 `(` 切 —— 单位名本身带括号（`平均电流(mA)`），
/// 第一个括号组是 `mA`，不是数值。
fn last_paren_int(line: &str) -> Option<i32> {
    line.match_indices('(')
        .filter_map(|(i, _)| {
            let rest = &line[i + 1..];
            let end = rest.find(')')?;
            rest[..end].trim().parse::<i32>().ok()
        })
        .next_back()
}

/// 一次性把设备侧状态取齐，供 `status` 显示。每一项都已格式化成可打印的字符串。
pub struct DeviceState {
    pub charge_limit: String,
    pub battery: String,
}

pub fn read_state(acm: &mut Acm, m: &Model) -> DeviceState {
    // 单项失败不影响其它项 —— 状态查询不该因为一个脚本缺失就整体失败
    let charge_limit =
        charge_limit_effective(acm, m).unwrap_or_else(|e| format!("读取失败：{e:#}"));
    let battery = match battery_current(acm, m) {
        Ok(Some(ma)) => format!(
            "{ma:>6} mA  {}",
            if ma > 0 {
                "充电中"
            } else {
                "⚠️ 放电中"
            }
        ),
        Ok(None) => "这台没有电量计脚本".to_owned(),
        Err(e) => format!("读取失败：{e:#}"),
    };
    DeviceState {
        charge_limit,
        battery,
    }
}

/// 打开某个端口对应的维护口。
pub fn open_acm(port: &str) -> Result<Acm> {
    let tty = crate::usb::maintenance_tty(port)
        .with_context(|| format!("端口 {port} 没有找到维护口（接口 :1.0 的 ttyACM）"))?;
    Acm::open(&tty, port)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn 取最后一个括号里的整数() {
        // 单位名本身带括号 —— 这是踩过的坑
        assert_eq!(
            last_paren_int("  0x14 平均电流(mA) = 0x0319 (   793)  [4/5]"),
            Some(793)
        );
        assert_eq!(
            last_paren_int("  0x14 平均电流(mA) = 0xfc82 (  -894)  [3/5]"),
            Some(-894)
        );
        assert_eq!(
            last_paren_int("  0x02 AtRate = 0x001e (    30)  [5/5]"),
            Some(30)
        );
        assert_eq!(last_paren_int("没有括号"), None);
        assert_eq!(last_paren_int("(mA) 只有单位没有数值"), None);
    }
}
