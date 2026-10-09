# AudioPlaybackConnector
**English** | [简体中文](https://github.com/ysc3839/AudioPlaybackConnector/blob/master/README.zh_CN.md)

Bluetooth audio playback (A2DP Sink) connector for Windows 10 2004+ and Windows 11.

Microsoft added Bluetooth A2DP Sink to Windows 10 2004. However, a third-party app is required to manage connections.
This app provides a lightweight, modern, and open-source system tray utility for connecting and controlling incoming Bluetooth audio.

### Features
* **Tray Integration:** Left-click to quickly connect or disconnect paired Bluetooth devices.
* **Independent Bluetooth Volume:** Dedicated volume slider for incoming phone audio without affecting PC system volume.
* **Lock Phone Volume Buttons:** Prevent phone volume rocker buttons from altering PC master volume.
* **Automatic Absolute Volume Decoupling:** Optionally disable Windows AVRCP Absolute Volume on first setup (with option to revert anytime).
* **Startup & Auto-Reconnect:** Option to run at Windows startup and automatically reconnect to the last used device.

# Preview
![Preview](https://cdn.jsdelivr.net/gh/ysc3839/AudioPlaybackConnector@master/AudioPlaybackConnector.gif)

# Usage
* Download and run AudioPlaybackConnector from [releases](https://github.com/ysc3839/AudioPlaybackConnector/releases).
* Pair your Bluetooth device in Windows Bluetooth settings (right-click the tray icon and select "Bluetooth Settings").
* **Left-click** the tray icon to connect or disconnect your device.
* **Right-click** the tray icon to adjust Bluetooth volume, configure startup settings, and manage volume options.
* **Phone Volume Decoupling:** On first launch, the app offers to optimize volume by disabling Absolute Volume (requires Administrator elevation and a PC restart). You can revert this anytime via the tray menu.
