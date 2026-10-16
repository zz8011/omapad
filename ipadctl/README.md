# Omapad 控制面板

宿主 Omarchy / Hyprland 上的 iPad GUD 副屏控制面板。交互和历史实测见 [DESIGN.md](DESIGN.md)，本轮主题与充电页的实现边界见 [界面设计](../docs/INTERFACE-DESIGN.md)。

```bash
cd ipadctl
cargo test --workspace --locked
cargo build -p omapad --release --locked
./target/release/omapad
```

窗口有“屏幕”和“充电”两页；关闭窗口后可从托盘重新打开。默认自动副屏开关来自 `~/.config/ipadctl/config.toml`。正常启动时若开关开启且遇到符合条件的 DFU 设备，程序可能执行已配置的自动投放。

**只看界面、禁用自动投放**：先运行 `cargo build -p omapad --locked`，再用 `./target/debug/omapad --preview-charge` 打开充电页；`--measure` 运行短暂窗口与 RSS 测量。预览模式仍会从在线设备读取充电状态，也保留手动限流按钮；不要把它当作硬件隔离模拟器。只有在 `--preview-charge` 模式下，`OMARPAD_PREVIEW_PALETTE=/path/to/resolved-colors.tsv` 才会加载离线色板，并暂停运行中的主题轮询。该文件格式是 `omarchy theme color --all` 的制表符分隔输出。

界面从 `omarchy theme color --all` 获取当前主题，运行中约每三秒检查一次。充电页的按钮高亮代表配置目标；“目标已提交”“看门狗已写入”“寄存器已回读”分别是不同证据层级。J207 仅有看门狗日志证据，没有寄存器回读。设备端口断开、ACM 失败与无电量计会分别显示。

本工作区只提供构建和运行入口；没有安装二进制、创建自启动项或替换现有驻留进程。
