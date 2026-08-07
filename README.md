# VDIClient (macOS arm64)

macOS 桌面 **VDI 远程桌面客户端**（虚拟桌面基础设施客户端，即"云电脑"客户端）：登录 VDI 服务器、管理虚拟机、连接远程桌面，基于自编译 FreeRDP 3.28.0 + Qt 6。

支持 **macOS 13 - 26（arm64）**，部署目标固定 13.0。

## 功能特性

| 模块 | 说明 |
|------|------|
| RDP 连接 | GFX + AVC444，FFmpeg 软解 H.264，解码已线程化；QRhiTexture + Metal 渲染 |
| 登录 / VM 管理 | Qt Widgets 界面：Token 登录、VM 列表/电源管理、快照、心跳保活、多语言（中/英/日/繁） |
| 剪贴板 | 双向文本 + 文件传输（CF_HDROP / FileGroupDescriptorW） |
| 磁盘重定向 | macOS libusb 无法透传 USB，改为把 /Volumes 卷映射为 RDPDR 盘符 |
| 摄像头重定向 | 自研 AVFoundation 后端 + VideoToolbox 硬编 H.264，2 秒空闲自动暂停（自动关灯） |
| 麦克风重定向 | 官方 audin `mac/` 后端（CoreAudio），已修复 3 处 FreeRDP 源码问题 |

## 架构

两个进程协作：

```
VDIClient.app (Qt Widgets 登录/VM 界面)
  └─ QProcess 启动内嵌 qf-client.app  ──>  RDP 渲染核心 (QML + FreeRDP)
        （qf-client 为 UIElement，无 Dock 图标）
```

- `VDIClient.app`：登录、Token、VM 列表、心跳（15s）、从服务器获取连接命令
- `qf-client.app`：实际 RDP 会话（渲染/输入/剪贴板/磁盘/摄像头/麦克风）

## 目录结构

| 目录 | 说明 |
|------|------|
| `qfreerdp-vdi-client/` | 主工程（`qf-client/` + `vdi-client/`），一次构建产出两个 app 并合并 |
| `freerdp-3.28.0/` | FreeRDP 源码 + `build-mac/`（构建）+ `install-mac/`（安装产物） |
| `.deps-mac13/` | 预编译第三方依赖：精简 FFmpeg、OpenSSL、json-c、libusb、openh264、spdlog/fmt |
| `scripts/` | `build-freerdp-mac.sh`（FreeRDP 构建）、`package-dmg.sh`（DMG 打包）、`check-bundle.sh`（依赖自检） |
| `docs/` | 构建与修复方案文档 |
| `build/` | 打包产物 `VDIClient-1.5.0.dmg` |

## 构建与打包

依赖：Xcode 命令行工具、CMake、Ninja、Homebrew OpenSSL@3、Qt 6.11.1（arm64）、`.deps-mac13`。

```bash
# ① FreeRDP（含源码级修改，见 docs/build-freerdp-mac.md 第 6 节）
bash scripts/build-freerdp-mac.sh

# ② 主工程
cmake -S qfreerdp-vdi-client -B qfreerdp-vdi-client/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_PREFIX_PATH="/Users/kk/.local/share/qt-official/6.11.1/macos;$(pwd)/.deps-mac13"
cmake --build qfreerdp-vdi-client/build --parallel 8

# ③ 打包 DMG + 自检
bash scripts/package-dmg.sh
bash scripts/check-bundle.sh
```

产物：`build/VDIClient-1.5.0.dmg`（单应用 DMG，ad-hoc 签名未公证）。完整可重复构建流程（从零到 DMG）已验证通过。

## 文档

- [docs/build-freerdp-mac.md](docs/build-freerdp-mac.md) — FreeRDP 构建逻辑、依赖版本冲突修复、部署目标、FreeRDP 源码修改记录
- [qt-qml-rdp-client-goal.md](qt-qml-rdp-client-goal.md) — Windows 版客户端目标文档（保留作参考，本项目为 macOS 版）
