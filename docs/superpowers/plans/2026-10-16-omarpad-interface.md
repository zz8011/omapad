# Omarpad Interface Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans task by task. This workspace has no subagent dispatch tool.

**Goal:** Deliver the existing Omarpad Slint control panel with Omarchy theme adaptation, refined typography and motion, and a functional charging page.

**Architecture:** Bring a source-only snapshot of the already built `ipadctl` application into this isolated worktree. Keep the existing Rust core, device detection, screen canvas, and two-tab model. Add a small pure palette adapter to the GUI, bind it to Slint's global Theme, and add charging state/actions through the existing ACM API on worker threads.

**Tech Stack:** Rust, Slint 1.18, ksni, Omarchy `omarchy theme color --all`, local fontconfig.

**Spec:** `ipadctl/DESIGN.md` (existing user-approved control model) and `docs/INTERFACE-DESIGN.md` (updated to the actual two-tab application and its evidence boundaries).

## Global constraints

- Work in this isolated `zz8011/界面` worktree. The main checkout's `ipadctl/` is untracked user work; copy source files only and leave its target/config/repair artifacts untouched.
- Do not edit Omarchy theme files or the user's Hyprland configuration to implement skinning. Read current theme state only.
- Do not run DFU, alter brightness/charging, or reload Hyprland during visual verification. Start the GUI in `--measure` or an equivalent no-flash mode.
- Do not claim a hardware change took effect until readback supports it. J207 watchdog log evidence and A7 register readback are distinct.
- Do not silently replace an existing resident Omarpad process or install autostart/system files.

## Review focus

- Theme switch while the current theme directory is being replaced: keep the last complete palette and update all tokens in one UI turn.
- Arbitrary user theme with low secondary-text or accent contrast: correct derived UI colors without modifying source theme.
- GUI cannot reach Omarchy command: keep a legible fallback palette and report only a theme warning, never terminate the control panel.
- Charging ACM is absent or times out: show an unavailable/stale state; keep scan and window responsive.
- Charging write returns target-file success but no later readback: say only “目标已提交” until the appropriate evidence arrives.

## Task 1: Import and baseline

**Files:** Copy `ipadctl/Cargo.toml`, `Cargo.lock`, `src/`, `crates/gui/`, `DESIGN.md`, `.gitignore` from the main checkout into the empty `ipadctl/` in this worktree. Exclude `target/`, `.mimosa/`, `.scratch/` and binaries.

- [x] Confirm source inventory and preserve the main checkout unchanged.
- [x] Run `cargo test --workspace --locked` in the copied tree; baseline 36 passed.
- [x] Commit the source import as one reviewable change (`649a692`).

## Task 2: Omarchy palette and typography

**Files:** Add `ipadctl/crates/gui/src/theme.rs`; modify `crates/gui/src/main.rs`, `crates/gui/ui/app.slint`, and both design docs.

**Interfaces:** `theme::Palette::from_resolved(&str) -> Result<Palette>` parses semantic colors; `theme::current() -> Result<(String, Palette)>` reads the local Omarchy palette; `theme::apply(&OmarpadWindow, &Palette)` updates the Slint Theme global on the UI thread.

- [x] Add focused tests for dark Labra, light Flexoki, malformed/incomplete input, and contrast adjustment; observed failure before implementation.
- [x] Implement the palette adapter and last-good polling; apply palette and card colors together on the UI event loop.
- [x] Replace fixed UI colors with Theme properties; get monospace from fontconfig and use the system body font.
- [x] Add short hover/selection/toggle motion with reduced-motion path.
- [x] Run tests and render Labra screen and charging pages; inspected no clipped charging cards.
- [x] Stage theme and visual change with the charging implementation for the final feature commit.

## Task 3: Complete charging page

**Files:** Modify `crates/gui/ui/app.slint`, `crates/gui/src/main.rs`; add pure status/option helpers in `crates/gui/src/charge.rs` if needed.

**Interfaces:** Existing `Acm::open` and `device::{set_charge_limit, charge_limit_effective, battery_current}` run only on bounded worker threads. UI receives short state strings and valid per-model choices.

- [x] Add tests for per-model limit choices and evidence wording; observed failure before implementation.
- [x] Build cards for online iPads with target, device readback, current, age and error text.
- [x] Run ACM read/write on worker threads with per-port exclusion and a two-job cap. Confirm J207 only from a log newer than device uptime sampled after submission.
- [x] Run focused and whole-workspace tests; render Labra charging tab in no-flash mode and inspect readings.
- [x] Check Flexoki Light window with an offline palette preview. Actual system theme switching remains untested.
- [x] Update implementation status in docs and stage charging page for the final feature commit.

## Task 4: Delivery check

**Files:** `README.md` or `ipadctl/README.md` for a concise build/run note.

- [x] Run targeted `rustfmt --check`, `cargo test --workspace --locked` (45 passed), `cargo clippy --workspace --locked --all-targets -- -D warnings` (clean) and `git diff --check` (clean). Full workspace `cargo fmt --check` finds formatting drift imported with older core source, unrelated to this UI change.
- [x] Verify the no-flash GUI visibly opens on current Labra and reports the theme name; exercise Flexoki Light parsing and contrast from an offline fixture without changing the system theme.
- [x] Record live display evidence and untested hardware writes in the design record. Resident/system configuration unchanged.
