//! Omarchy palette adapter. `from_resolved` is pure so custom themes can be
//! checked without changing the user's active desktop theme.

use anyhow::{Context, Result, anyhow, bail};
use std::collections::HashMap;
use std::path::PathBuf;
use std::process::Command;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Rgb(pub u8, pub u8, pub u8);

impl Rgb {
    fn mix(self, other: Self, amount: f64) -> Self {
        let mix =
            |a: u8, b: u8| (f64::from(a) * (1.0 - amount) + f64::from(b) * amount).round() as u8;
        Self(
            mix(self.0, other.0),
            mix(self.1, other.1),
            mix(self.2, other.2),
        )
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Palette {
    pub mode: String,
    pub window: Rgb,
    pub inset: Rgb,
    pub card: Rgb,
    pub card_hi: Rgb,
    pub callout: Rgb,
    pub accent_soft: Rgb,
    pub border: Rgb,
    pub border_hi: Rgb,
    pub divider: Rgb,
    pub text: Rgb,
    pub text_secondary: Rgb,
    pub text_muted: Rgb,
    pub accent: Rgb,
    pub accent_text: Rgb,
    pub ok: Rgb,
    pub pending: Rgb,
    pub fail: Rgb,
    pub fallback: Rgb,
}

impl Palette {
    pub fn from_resolved(raw: &str) -> Result<Self> {
        let entries: HashMap<&str, &str> = raw
            .lines()
            .filter_map(|line| line.split_once('\t'))
            .collect();
        let get = |key: &str| -> Result<Rgb> {
            parse_hex(
                entries
                    .get(key)
                    .ok_or_else(|| anyhow!("missing theme color {key}"))?,
            )
            .with_context(|| format!("invalid theme color {key}"))
        };
        let mode = entries
            .get("mode")
            .ok_or_else(|| anyhow!("missing theme mode"))?;
        if !matches!(*mode, "light" | "dark") {
            bail!("invalid theme mode {mode}");
        }
        let window = get("background")?;
        let raw_text = get("foreground")?;
        let card = entries
            .get("lighter_background")
            .and_then(|s| parse_hex(s).ok())
            .unwrap_or_else(|| window.mix(raw_text, 0.10));
        let text = readable(raw_text, card, 4.5);
        let text_secondary = readable(
            entries
                .get("dark_foreground")
                .and_then(|s| parse_hex(s).ok())
                .unwrap_or(text),
            card,
            4.5,
        );
        let text_muted = readable(
            entries
                .get("muted")
                .and_then(|s| parse_hex(s).ok())
                .unwrap_or(text_secondary),
            card,
            4.5,
        );
        let accent = visible(get("accent")?, card, 3.0);
        let accent_soft = window.mix(accent, 0.18);
        let accent_text = readable(readable(accent, card, 4.5), accent_soft, 4.5);
        Ok(Self {
            mode: (*mode).to_owned(),
            window,
            inset: window.mix(text, 0.04),
            card,
            card_hi: window.mix(text, 0.14),
            callout: window.mix(accent, 0.08),
            accent_soft,
            border: window.mix(text, 0.20),
            border_hi: window.mix(text, 0.32),
            divider: window.mix(text, 0.15),
            text,
            text_secondary,
            text_muted,
            accent,
            accent_text,
            ok: visible(optional(&entries, "green", text_secondary), card, 5.5),
            pending: visible(optional(&entries, "yellow", text_secondary), card, 5.5),
            fail: visible(optional(&entries, "red", accent), card, 5.5),
            fallback: text_secondary,
        })
    }

    pub fn fallback() -> Self {
        Self::from_resolved("mode\tdark\nbackground\t#0D120D\nforeground\t#F2F5F0\nlighter_background\t#121A12\ndark_foreground\t#A7B4A6\naccent\t#FF2E88\nred\t#FF758A\ngreen\t#B8C77A\nyellow\t#F9E2AF\n")
            .expect("built-in fallback palette is valid")
    }
}

impl Default for Palette {
    fn default() -> Self {
        Self::fallback()
    }
}

fn parse_snapshot(name_before: &str, name_after: &str, raw: &str) -> Result<Palette> {
    if name_before.is_empty() || name_before != name_after {
        bail!("Omarchy theme changed during palette read");
    }
    Palette::from_resolved(raw)
}

fn state_dir() -> Result<PathBuf> {
    let base = if let Some(p) = std::env::var_os("XDG_STATE_HOME") {
        PathBuf::from(p)
    } else {
        PathBuf::from(std::env::var_os("HOME").ok_or_else(|| anyhow!("HOME is unset"))?)
            .join(".local/state")
    };
    Ok(base.join("omarchy/current"))
}

/// Read Omarchy's resolved semantic palette, checking for a theme-set race.
pub fn current() -> Result<(String, Palette)> {
    let path = state_dir()?.join("theme.name");
    let before =
        std::fs::read_to_string(&path).with_context(|| format!("read {}", path.display()))?;
    let output = Command::new("omarchy")
        .args(["theme", "color", "--all"])
        .output()
        .context("run omarchy theme color --all")?;
    if !output.status.success() {
        bail!("omarchy theme color --all exited {}", output.status);
    }
    let after =
        std::fs::read_to_string(&path).with_context(|| format!("read {}", path.display()))?;
    let name = after.trim().to_owned();
    let palette = parse_snapshot(before.trim(), &name, std::str::from_utf8(&output.stdout)?)?;
    Ok((name, palette))
}

pub fn mono_font() -> Option<String> {
    let output = Command::new("fc-match")
        .args(["monospace", "-f", "%{family}\n"])
        .output()
        .ok()?;
    if !output.status.success() {
        return None;
    }
    let name = String::from_utf8(output.stdout).ok()?;
    let first = name.lines().next()?.split(',').next()?.trim();
    if first.is_empty() {
        None
    } else {
        Some(first.to_owned())
    }
}

pub fn reduce_motion() -> bool {
    Command::new("gsettings")
        .args(["get", "org.gnome.desktop.interface", "enable-animations"])
        .output()
        .ok()
        .is_some_and(|output| output.status.success() && output.stdout.starts_with(b"false"))
}

fn parse_hex(raw: &str) -> Result<Rgb> {
    let hex = raw
        .strip_prefix('#')
        .ok_or_else(|| anyhow!("expected #RRGGBB"))?;
    if hex.len() != 6 || !hex.bytes().all(|b| b.is_ascii_hexdigit()) {
        bail!("expected #RRGGBB");
    }
    Ok(Rgb(
        u8::from_str_radix(&hex[0..2], 16)?,
        u8::from_str_radix(&hex[2..4], 16)?,
        u8::from_str_radix(&hex[4..6], 16)?,
    ))
}

fn optional(entries: &HashMap<&str, &str>, key: &str, fallback: Rgb) -> Rgb {
    entries
        .get(key)
        .and_then(|s| parse_hex(s).ok())
        .unwrap_or(fallback)
}

fn luminance(c: Rgb) -> f64 {
    let channel = |v: u8| {
        let x = f64::from(v) / 255.0;
        if x <= 0.04045 {
            x / 12.92
        } else {
            ((x + 0.055) / 1.055).powf(2.4)
        }
    };
    0.2126 * channel(c.0) + 0.7152 * channel(c.1) + 0.0722 * channel(c.2)
}

pub fn contrast(a: Rgb, b: Rgb) -> f64 {
    let (low, high) = (
        luminance(a).min(luminance(b)),
        luminance(a).max(luminance(b)),
    );
    (high + 0.05) / (low + 0.05)
}

fn readable(color: Rgb, background: Rgb, minimum: f64) -> Rgb {
    if contrast(color, background) >= minimum {
        return color;
    }
    visible(color, background, minimum)
}

fn visible(color: Rgb, background: Rgb, minimum: f64) -> Rgb {
    if contrast(color, background) >= minimum {
        return color;
    }
    for n in 1..=100 {
        let amount = f64::from(n) / 100.0;
        let toward_black = color.mix(Rgb(0, 0, 0), amount);
        let toward_white = color.mix(Rgb(255, 255, 255), amount);
        let black_ok = contrast(toward_black, background) >= minimum;
        let white_ok = contrast(toward_white, background) >= minimum;
        if black_ok || white_ok {
            return if black_ok && white_ok {
                if contrast(toward_black, background) >= contrast(toward_white, background) {
                    toward_black
                } else {
                    toward_white
                }
            } else if black_ok {
                toward_black
            } else {
                toward_white
            };
        }
    }
    if contrast(Rgb(0, 0, 0), background) >= contrast(Rgb(255, 255, 255), background) {
        Rgb(0, 0, 0)
    } else {
        Rgb(255, 255, 255)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const LABRA: &str = "mode\tdark\nbackground\t#040704\nforeground\t#D3D7B5\nlighter_background\t#1d201d\ndark_foreground\t#9ea188\naccent\t#e33d84\nred\t#ee585d\ngreen\t#747f11\nyellow\t#d3dd2c\n";
    const FLEXOKI_LIGHT: &str = "mode\tlight\nbackground\t#FFFCF0\nforeground\t#100F0F\nlighter_background\t#E6E4D9\ndark_foreground\t#878580\naccent\t#205EA6\nred\t#D14D41\ngreen\t#66800B\nyellow\t#AD8301\n";

    #[test]
    fn labra_keeps_its_background_and_accent() {
        let p = Palette::from_resolved(LABRA).unwrap();
        assert_eq!(p.window, Rgb(0x04, 0x07, 0x04));
        assert_eq!(p.accent, Rgb(0xe3, 0x3d, 0x84));
        assert!(contrast(p.text_secondary, p.card) >= 4.5);
        assert!(contrast(p.ok, p.card) >= 5.5);
        assert!(contrast(p.accent_text, p.accent_soft) >= 4.5);
    }

    #[test]
    fn flexoki_light_corrects_low_contrast_secondary_text() {
        let p = Palette::from_resolved(FLEXOKI_LIGHT).unwrap();
        assert_eq!(p.mode, "light");
        assert_ne!(p.text_secondary, Rgb(0x87, 0x85, 0x80));
        assert!(contrast(p.text_secondary, p.card) >= 4.5);
        assert!(contrast(p.accent, p.card) >= 3.0);
        assert!(contrast(p.accent_text, p.accent_soft) >= 4.5);
    }

    #[test]
    fn missing_or_invalid_required_colors_are_rejected() {
        let missing = LABRA.replace("accent\t#e33d84\n", "");
        assert!(Palette::from_resolved(&missing).is_err());
        let malformed = LABRA.replace("#040704", "rgba(0,0,0,1)");
        assert!(Palette::from_resolved(&malformed).is_err());
        let bad_mode = LABRA.replace("mode\tdark", "mode\tauto");
        assert!(Palette::from_resolved(&bad_mode).is_err());
    }

    #[test]
    fn rejects_palette_read_across_theme_switch() {
        assert!(parse_snapshot("Labra", "Flexoki Light", LABRA).is_err());
        assert!(parse_snapshot("Labra", "Labra", LABRA).is_ok());
    }
}
