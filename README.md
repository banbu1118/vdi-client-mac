# VDIClient - macOS VDI 客户端

基于 Qt6 构建的 macOS 原生 VDI（虚拟桌面基础设施）客户端，通过 FreeRDP（sdl3-freerdp）协议连接到远程桌面。

## 项目结构

```
vdi-client-mac/
├── CMakeLists.txt              # CMake 构建配置（含 DMG 打包）
├── VDIClient-Info.plist.in     # 主应用 Info.plist 模板
├── freerdp-Info.plist          # sdl3-freerdp 的 Info.plist（含 TCC 权限声明）
├── desktop-1024x1024.png       # 应用图标源文件（1024x1024 PNG）
├── src/
│   ├── main.cpp                # 入口文件
│   ├── loginwindow.h           # 主窗口头文件
│   └── loginwindow.cpp         # 主窗口实现（业务逻辑、UI、网络请求、RDP 进程管理）
├── resources/
│   ├── resources.qrc           # Qt 资源文件
│   └── logo.png                # 应用 Logo
└── build/                      # 构建产物目录
    └── VDIClient-<version>-macOS.dmg  # 打包好的 DMG 安装包
```

## 功能

- **用户登录**：支持服务器地址、用户名、密码登录
- **自动登录/记住密码**：支持保存登录信息，自动登录
- **多语言支持**：英语、简体中文、繁体中文、日语
- **虚拟机管理**：查看虚拟机列表、开机、关机、重启、还原（快照）
- **RDP 远程连接**：通过 sdl3-freeRDP 连接远程桌面
- **物理分辨率适配**：自动获取屏幕物理分辨率并传入 RDP 参数
- **全局事件过滤**：RDP 连接期间拦截键盘事件，发送到远程桌面
- **SSL 配置**：支持自签名证书（开发环境 VerifyNone）

## 技术栈

| 组件 | 技术 |
|------|------|
| 编程语言 | C++17 |
| GUI 框架 | Qt6（Core, Widgets, Network） |
| 构建系统 | CMake ≥ 3.16 |
| RDP 客户端 | [sdl3-freerdp](https://github.com/FreeRDP/FreeRDP) (v3.27.1, arm64) |
| 网络通信 | Qt Network（HTTPS/JSON API） |
| 打包工具 | macdeployqt6 + hdiutil（DMG） |
| 代码签名 | ad-hoc codesign |

## 构建环境要求

| 依赖 | 版本 | 安装方式 |
|------|------|----------|
| macOS | ≥ 14.0 (Sonoma) | — |
| Xcode | ≥ 15.x | `xcode-select --install` 或 App Store |
| CMake | ≥ 3.16 | `brew install cmake` |
| Qt6 | ≥ 6.x | `brew install qt` |
| sdl3-freerdp | ≥ 3.27.1 | 源码编译，位于 `../sdl3-freerdp` |

## 构建命令

### 1. 配置

```bash
cd vdi-client-mac
cmake -B build -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0
```

### 2. 编译

```bash
cmake --build build
```

编译完成后，`build/VDIClient.app` 即为可运行的 .app bundle。

### 3. 运行调试

```bash
open build/VDIClient.app
# 或从终端直接运行
./build/VDIClient.app/Contents/MacOS/VDIClient
```

## 打包环境要求

| 工具 | 用途 | 安装方式 |
|------|------|----------|
| macdeployqt6 | 部署 Qt 框架依赖 | `brew install qt` 自带 |
| hdiutil | 创建 DMG 镜像 | macOS 内置 |
| codesign | ad-hoc 代码签名 | macOS 内置（Xcode） |

## 打包命令

```bash
# 编译 + 打包 DMG（一步完成）
cmake --build build --target package_dmg
```

打包完成后，DMG 文件位于 `build/VDIClient-<version>-macOS.dmg`。

### 打包流程（四步）

1. **复制** — 将 VDIClient.app 复制到临时暂存目录
2. **部署 Qt 框架** — macdeployqt6 自动复制所需的 Qt 动态库
3. **代码签名** — ad-hoc codesign 整个 bundle（含 sdl3-freerdp）
4. **创建 DMG** — hdiutil 生成压缩 UDZO 格式的 .dmg 安装包

### 手动打包（当 `package_dmg` 因环境限制失败时）

```bash
# 确保 build 目录已编译
cmake --build build

# 手动执行打包步骤
DMG_STAGING="build/dmg-staging"
DMG_APP="${DMG_STAGING}/VDIClient.app"

rm -rf "${DMG_STAGING}"
mkdir -p "${DMG_STAGING}"

# 1. 复制 .app
ditto --noextattr "build/VDIClient.app" "${DMG_APP}"

# 2. 部署 Qt 框架
macdeployqt6 "${DMG_APP}" -always-overwrite -verbose=1

# 3. 代码签名
codesign --force --sign - --deep "${DMG_APP}"

# 4. 创建 DMG
ln -sfn /Applications "${DMG_STAGING}/Applications"
hdiutil create -ov -fs HFS+ -srcfolder "${DMG_STAGING}" \
    -volname "VDIClient" \
    -format UDZO -imagekey zlib-level=9 \
    "build/VDIClient-1.5.0-macOS.dmg"
rm -rf "${DMG_STAGING}"
```

## 注意事项

### macOS 最低版本

项目设置 `CMAKE_OSX_DEPLOYMENT_TARGET=14.0`（macOS Sonoma），如需支持更低版本，修改 CMakeLists.txt 中的该变量并重新编译。

### TCC 隐私权限

首次连接 RDP 时，macOS 会弹出隐私权限请求：
- **屏幕录制** — 显示远程桌面内容（必须允许）
- **辅助功能** — 发送键盘鼠标输入（必须允许）
- **输入监控** — 捕获键盘输入（必须允许）

如果拒绝，sdl3-freerdp 进程将被 macOS 强制终止（exit code 6）。

### 项目依赖关系

sdl3-freerdp 项目需要和 vdi-client-mac 同级目录：

```
vdi-opendesk/
├── sdl3-freerdp/          # FreeRDP 构建产物（.app bundle）
├── vdi-client-mac/        # 本 VDI 客户端项目
│   └── CMakeLists.txt     # 构建时会从 ../sdl3-freerdp 复制依赖
└── ...
```
