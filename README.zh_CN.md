# AudioPlaybackConnector
[English](https://github.com/ysc3839/AudioPlaybackConnector/blob/master/README.md) | **简体中文**

Windows 10 2004+ 及 Windows 11 蓝牙音频接收 (A2DP Sink) 连接工具。

微软在 Windows 10 2004 加入了蓝牙 A2DP Sink 支持，但需要第三方工具来管理连接。
本工具提供了一个轻量、现代且开源的通知区域托盘程序，用于快速连接与管理蓝牙音频。

### 主要功能
* **托盘快捷操作：** 左键点击托盘图标快速连接或断开已配对的蓝牙设备。
* **独立蓝牙音量控制：** 提供专属音量滑块调节手机音频流音量，不影响系统主音量。
* **锁定手机音量按键：** 防止手机音量物理按键更改电脑系统主音量。
* **绝对音量解绑优化：** 首次启动时提示优化音量设置（禁用绝对音量），并支持随时在菜单中恢复。
* **开机自启与自动重连：** 支持随系统启动并自动连接上次使用的蓝牙设备。

# 预览
![预览](https://cdn.jsdelivr.net/gh/ysc3839/AudioPlaybackConnector@master/AudioPlaybackConnector.gif)

# 使用方法
* 从 [releases](https://github.com/ysc3839/AudioPlaybackConnector/releases) 下载并运行 AudioPlaybackConnector。
* 在系统蓝牙设置中配对蓝牙设备（可右键托盘图标选择“蓝牙设置”）。
* **左键点击** 托盘图标选择并连接设备。
* **右键点击** 托盘图标打开音量调节滑块及相关设置。
* **音量解绑优化：** 首次运行时可选择应用音量优化，防止手机按键影响电脑主音量（需要管理员权限并重启电脑生效），后续也可在托盘菜单中选择恢复。
