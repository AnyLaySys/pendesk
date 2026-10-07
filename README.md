# PenDesk

PenDesk 将词典笔连接到 Windows 桌面，提供远程桌面、输入、声音、录音、相机与文件传输。电脑端还会在 Windows 11 上注册 `PenDesk (Windows 虚拟摄像头)`；该设备只在电脑端 PenDesk 运行时出现。

## 系统要求

- Windows x64、PowerShell、Rust stable（含 MSVC C++ 生成工具与 Windows SDK）。
- Windows 版 [Tailscale](https://tailscale.com/download) 已安装并登录。程序通过它取得 Tailnet IPv4。
- `adb` 位于 Windows 的 `PATH`，词典笔已通过 ADB 连接。
- WSL2 Ubuntu，能使用 `sudo`。构建脚本会在 WSL 中交叉编译词典笔 ARM64 程序。
- 词典笔系统须提供 `/usr/bin/miniapp_cli`、`/userdata` 与可用的 ADB shell。安装时会创建 `/userdata/.disable_app_whitelist_clean`。
- 内置虚拟摄像头需要 Windows 11 Build 22000 或更高版本。虚拟麦克风不提供；Windows 没有等价的用户态虚拟音频端点 API。

## 首次配置 WSL

`build.ps1` 每次构建都会执行以下两项，它们不会卸载或替换 amd64 软件包：

```bash
sudo dpkg --add-architecture arm64
sudo apt install -y libc6-dev-arm64-cross libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev
```

全新 WSL 还需要一次性准备编译器、`pkg-config` 与 ARM64 GStreamer 开发库。构建脚本刻意不执行 `apt update`，因此首次手动准备时运行：

```bash
sudo dpkg --add-architecture arm64
sudo apt update
sudo apt install -y build-essential gcc-aarch64-linux-gnu pkg-config libc6-dev-arm64-cross libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libgstreamer1.0-dev:arm64 libgstreamer-plugins-base1.0-dev:arm64
```

ARM64 GStreamer 开发库会带来较多依赖，这是为生成词典笔相机采集程序所必需的。项目位于 Windows 磁盘时，WSL 中的路径为 `/mnt/c/...`，不会复制到 WSL 家目录。

## 构建

在项目根目录执行：

```powershell
.\build.ps1
```

产物位于 `build\out`：

- `pendesk.exe`：Windows 电脑端与安装工具。
- `pendesk_camera.dll`：Windows 虚拟摄像头组件，必须和 `pendesk.exe` 保持同目录。
- `pendesk.amr`：词典笔 MiniApp 安装包。

首次构建会下载 QuickJS 与词典笔 ARM64 Tailscale，因此需要网络连接。

## 安装与配置

先确认 ADB 能看到目标词典笔：

```powershell
adb devices
```

如同时连接多个设备，所有 PenDesk 命令都应指定词典笔序列号：

```powershell
.\build\out\pendesk.exe install --serial <词典笔序列号>
```

安装后，配置电脑端所在 Tailnet。省略 `--host` 时会使用当前 Windows Tailscale 的 IPv4：

```powershell
.\build\out\pendesk.exe configure --serial <词典笔序列号>
```

如需指定另一台已运行 PenDesk 的电脑：

```powershell
.\build\out\pendesk.exe configure --host <Tailnet IPv4> --serial <词典笔序列号>
```

若词典笔尚未登录 Tailscale，可附加预授权密钥：

```powershell
.\build\out\pendesk.exe configure --auth-key <Tailscale-auth-key> --serial <词典笔序列号>
```

配置本机后，电脑端会自动启动。也可以手动控制：

```powershell
.\build\out\pendesk.exe run
.\build\out\pendesk.exe stop
.\build\out\pendesk.exe restart
```

## 常见问题

- `无ADB设备`：检查 `adb devices`，并用 `--serial` 指定词典笔，避免误选普通 Android 手机。
- `Tailscale未安装` 或 `Tailscale未登录`：在 Windows 安装并登录 Tailscale 后重新执行 `configure`。
- Windows 相机找不到 PenDesk：确认 Windows 11 版本符合要求、`pendesk_camera.dll` 与 `pendesk.exe` 同目录，并保持 `pendesk.exe run` 运行。
- 词典笔熄屏再亮屏后没有画面：请安装包含最新 `pendesk.amr` 的版本；播放器会在恢复时自动重建。