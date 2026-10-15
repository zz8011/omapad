//! Charging-page choices and evidence wording. Hardware I/O stays in ipadctl::device.

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Choice {
    pub label: &'static str,
    pub hex: &'static str,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Proof {
    TargetSubmitted,
    WatchdogWrite,
    RegisterReadback,
}

const A7: [Choice; 2] = [
    Choice {
        label: "约 206 mA",
        hex: "7f",
    },
    Choice {
        label: "约 650–750 mA",
        hex: "bf",
    },
];
const J207: [Choice; 3] = [
    Choice {
        label: "500 mA",
        hex: "32",
    },
    Choice {
        label: "1000 mA",
        hex: "64",
    },
    Choice {
        label: "1500 mA",
        hex: "96",
    },
];

pub fn choices(slug: &str) -> &'static [Choice] {
    match slug {
        "a1599" | "j86" => &A7,
        "j207" => &J207,
        _ => &[],
    }
}

pub fn allowed(slug: &str, hex: &str) -> bool {
    choices(slug)
        .iter()
        .any(|c| c.hex.eq_ignore_ascii_case(hex))
}

pub fn watchdog_value(line: &str) -> Option<String> {
    let (_, value) = line.rsplit_once("d2355: write 0x044e len=1 ok: ")?;
    let value = value.trim();
    if value.len() == 2 && value.bytes().all(|b| b.is_ascii_hexdigit()) {
        Some(value.to_ascii_lowercase())
    } else {
        None
    }
}

fn kernel_seconds(line: &str) -> Option<f64> {
    let (_, rest) = line.split_once('[')?;
    let (stamp, _) = rest.split_once(']')?;
    stamp.trim().parse::<f64>().ok()
}

pub fn device_uptime(output: &str) -> Option<f64> {
    output.lines().find_map(|line| {
        line.split_whitespace()
            .next()?
            .parse::<f64>()
            .ok()
            .filter(|v| v.is_finite())
    })
}

pub fn proof(
    slug: &str,
    target: &str,
    submitted_uptime: Option<f64>,
    after_log: Option<&str>,
    register: Option<&str>,
) -> Proof {
    if slug == "j207" {
        if let (Some(since), Some(after)) = (submitted_uptime, after_log)
            && kernel_seconds(after).is_some_and(|t| t > since)
            && watchdog_value(after).is_some_and(|v| v.eq_ignore_ascii_case(target))
        {
            return Proof::WatchdogWrite;
        }
    } else if matches!(slug, "a1599" | "j86")
        && register.is_some_and(|v| v.trim_start_matches("0x").eq_ignore_ascii_case(target))
    {
        return Proof::RegisterReadback;
    }
    Proof::TargetSubmitted
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn model_limits_exclude_unoffered_values() {
        assert_eq!(
            choices("j207").iter().map(|c| c.hex).collect::<Vec<_>>(),
            ["32", "64", "96"]
        );
        assert_eq!(
            choices("a1599").iter().map(|c| c.hex).collect::<Vec<_>>(),
            ["7f", "bf"]
        );
        assert_eq!(
            choices("j86").iter().map(|c| c.hex).collect::<Vec<_>>(),
            ["7f", "bf"]
        );
        assert!(allowed("j207", "96"));
        assert!(!allowed("j207", "ff"));
        assert!(!allowed("unknown", "bf"));
    }

    #[test]
    fn watchdog_value_requires_exact_write_marker() {
        assert_eq!(
            watchdog_value("[ 123.456] d2355: write 0x044e len=1 ok: 64"),
            Some("64".into())
        );
        assert_eq!(
            watchdog_value("[ 123.456] d2355: write 0x044e failed: 64"),
            None
        );
        assert_eq!(
            watchdog_value("[ 123.456] d2355: write 0x044e len=1 ok: 640"),
            None
        );
    }

    #[test]
    fn j207_needs_new_matching_log_after_submit() {
        let before = "[ 100.0] d2355: write 0x044e len=1 ok: 32";
        let after = "[ 131.0] d2355: write 0x044e len=1 ok: 64";
        assert_eq!(
            proof("j207", "64", Some(120.0), Some(before), None),
            Proof::TargetSubmitted
        );
        assert_eq!(
            proof("j207", "64", Some(120.0), Some(after), None),
            Proof::WatchdogWrite
        );
        assert_eq!(
            proof("j207", "32", Some(120.0), Some(after), None),
            Proof::TargetSubmitted
        );
        assert_eq!(
            proof("j207", "64", None, Some(after), None),
            Proof::TargetSubmitted
        );
        assert_eq!(
            proof("j207", "64", Some(132.0), Some(after), None),
            Proof::TargetSubmitted
        );
        assert_eq!(
            proof(
                "j207",
                "64",
                Some(120.0),
                Some("log without timestamp"),
                None
            ),
            Proof::TargetSubmitted
        );
        assert_eq!(device_uptime("123.45 456.78\n"), Some(123.45));
    }

    #[test]
    fn a7_requires_matching_register_readback() {
        assert_eq!(
            proof("a1599", "bf", None, None, Some("0xbf")),
            Proof::RegisterReadback
        );
        assert_eq!(
            proof("j86", "bf", None, None, Some("0x7f")),
            Proof::TargetSubmitted
        );
        assert_eq!(
            proof("a1599", "bf", None, None, None),
            Proof::TargetSubmitted
        );
    }
}
