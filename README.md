# BlueBridgeSimulator —— CT117E-M4 全软件模拟器（Keil µVision 集成）

不用真实硬件、不用 ST-Link：在 Keil µVision 里 **编译 → 启动调试 → 当前 build 自动烧进虚拟板 → 停在 `main()`**，
然后照常用断点、单步、Watch；虚拟板上的 LCD / LED / 按键 / 电位器 / 脉冲源 / PWM / 串口都在实时运行。

![模拟器界面](BlueBridgeSimulator/docs/images/simulator-window.png)

- 目标板：蓝桥杯 CT117E-M4（STM32G431RBT6，ARM Cortex-M4F @ 80 MHz）
- 完整功能与使用说明：[BlueBridgeSimulator/README.md](BlueBridgeSimulator/README.md)
- 下载安装包（v1.0.0）：[Releases](https://github.com/scott5204/BlueBridgeSimulator/releases/latest)

## 仓库目录

| 目录 | 内容 |
|---|---|
| `BlueBridgeSimulator/` | 模拟器本体 + Keil AGDI 驱动 + 安装/卸载脚本（主要项目，说明见其 README） |
| `unicorn-2.1.4/` | 打补丁后的 Unicorn 引擎源码（GPLv2） |
| `docs/` | 设计文档（含 Debug IPC 协议） |

## 许可协议

GPL-2.0（见 [LICENSE](LICENSE)）—— 模拟器链接了 GPLv2 的 Unicorn 引擎，整体采用兼容的 copyleft 许可。