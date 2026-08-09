# Qt6 + QML FreeRDP 客户端目标（Windows 版）

> ## 文档说明（2026-08-09 更新）
> 本文档记录 **Windows (MSVC 2022) 版** 客户端的目标与状态，对应项目（`qfreerdp-windows/`、`vdi-client-windows-main/`）**不在当前仓库中**，以下内容保留作 Windows 版参考。
> 当前仓库 `vdi-client-mac-main` 为 **macOS arm64 版**（FreeRDP 3.28.0 + Qt 6 QML/QSG/QRhi/Metal + Qt Widgets），项目结构、功能与构建见 [README.md](README.md)。
> macOS 版关键差异：
> - 渲染：`QRhiTexture` + Metal 后端（CPU staging buffer 上传），无 D3D11；
> - 摄像头：自研 AVFoundation（`avf/`）后端（官方仅 wmf/v4l/android）。采集侧**直连 VideoToolbox 硬编**（`VTCompressionSession` 直接消费 CVPixelBuffer，零拷贝；色彩属性随 buffer 附件输出，SPS VUI 正确，解决部分设备紫绿偏色/横纹问题）；`activeFormat` 按「尺寸+像素格式+帧率」三重精确匹配；空闲 2 秒自动暂停（自动关灯，再次请求透明恢复）；**已在实机验证正常**；麦克风：官方 audin `mac/` 后端（已修复 3 处 FreeRDP 源码问题，见 [docs/build-freerdp-mac.md](docs/build-freerdp-mac.md#6-freerdp-源码级修改记录麦克风重定向2026-08-05)）；
> - 原始 USB 透传（urbdrc/libusb）在 macOS 不可行，已转 RDPDR 磁盘重定向；
> - 项目已合并为 `qfreerdp-vdi-client/` 单项目：qf-client 内嵌进 `VDIClient.app`（单应用产物），qf-client 设 `LSUIElement=true` 无 Dock 图标；
> - 打包：`scripts/package-dmg.sh` 产出 DMG（`hdiutil makehybrid + convert`，沙箱兼容），ad-hoc 签名未公证；
> - 部署目标固定 **macOS 13.0**（主程序 + 全部 dylib minos=13.0，支持 13-26）；`check-bundle.sh` 自检 PASS；从零到 DMG 的完整可重复构建已验证通过（见 build 文档第 7 节）；
> - **当前状态（2026-08-09）**：`build/VDIClient-1.5.0.dmg` 安装后**运行正常**，**摄像头重定向实测正常**（含此前在另一台设备复现的紫绿偏色+横纹+双画面问题验证）；其余功能（剪贴板/磁盘/麦克风）待回归。

本文档记录基于 libfreerdp 构建的 VDI 远程桌面客户端的工作计划与实际状态。当前三个项目均为 **Windows (MSVC 2022)** 环境。

> 目录：`C:\Users\Administrator\Desktop\VDIClient\`
> - `qfreerdp-windows/` — qf-client (QML + FreeRDP)
> - `vdi-client-windows-main/` — VDIClient (Qt Widgets 登录/VM 管理)
> - `freerdp-3.28.0/` — FreeRDP 源码及编译产物
>
> 构建配置参考：FreeRDP 3.28.0 Release 模式（MSVC 2022 + vcpkg 依赖），CPU 转存帧缓冲 + **D3D11 原生纹理**上传（`ID3D11DeviceContext::UpdateSubresource`），无 DMA-BUF/VAAPI；音频输出使用 FreeRDP 库内建 WASAPI 后端，麦克风采集使用 WinMM 后端；摄像头使用 rdpecam（WMF 后端）；Windows 客户端（wfreerdp.exe）默认编译但 qf-client 不使用。

## 架构概览

整套 VDI 客户端分为两个独立进程：

```
VDIClient (QWidgets)              qf-client (QML + FreeRDP)
┌──────────────────────┐          ┌─────────────────────────┐
│ LoginWindow          │ QProcess │ QGuiApplication + QML   │
│ - 登录/Token 管理    │────────→ │ - RdpViewItem 渲染      │
│ - VM 列表/状态监控   │  启动    │ - FreeRDP 连接线程      │
│ - 启动 qf-client     │          │ - 剪贴板/USB/音频通道   │
│ - 心跳保活 (15s)     │          │ - 动态分辨率 (disp DVC) │
└──────────────────────┘          └─────────────────────────┘
```

| 项目 | 技术栈 | 入口 | 构建产物 |
|------|--------|------|----------|
| VDIClient | Qt6 Widgets + Network | `main.cpp` → `LoginWindow` | `build/Release/VDIClient.exe` |
| qf-client | Qt6 QML/Quick + FreeRDP 3.28 | `main()` → `mini-qf-client.cc` + `main.qml` | `build/qf-client.exe` |

VDIClient 通过 `QProcess` 启动 qf-client：请求服务器获取连接命令（原为 wfreerdp.exe 命令行），移除程序名、替换 `template.rdp` 路径为 `%LOCALAPPDATA%` 模板，再以 `bin\qf-client.exe <args>` 方式启动，工作目录为 `bin/`。登录接口的 `client_type` 为 `win_client`。

---

## 编译配置摘要

### FreeRDP 3.28.0 当前编译配置（Windows）

**基础环境**：MSVC 2022（`vcvars64.bat` 环境），Release，Ninja 生成器，`BUILD_SHARED_LIBS=ON`，vcpkg toolchain（`C:\Users\Administrator\Desktop\workspace\vcpkg\scripts\buildsystems\vcpkg.cmake`），ccache 已启用。

| 分类 | 开启 (ON) | 关闭 (OFF) |
|------|-----------|------------|
| **编解码** | FFmpeg H.264 (avcodec-62), DSP FFmpeg, SWScale, **OpenH264** (openh264-7.dll), NSCodec (内置) | JPEG, GFX AV1, AOM, Opus, SoXR, LAME, AAC 系列, GSM, **VAAPI**, Media Foundation |
| **音频** | **WASAPI**（播放，`WITH_WASAPI=ON`）, **WinMM**（`WITH_WINMM=ON`，麦克风 audin 使用） | PulseAudio, ALSA, OSS |
| **通道** | cliprdr, rdpsnd, rdpdr, drdynvc, rdpGFX, disp, rdpecam, urbdrc, audin, RAIL, rdpei (多触点), ainput, video, encomsp, echo, location, geometry, drive, smartcard, printer, remdesk, telemetry | TSMF (多媒体), SSH Agent, RDP2TCP, RDPEAR, RDPEWA, GFXREDIR |
| **客户端通道** | drive, printer, smartcard (总开关 ON) | rdpecam_client_stub(无), rdpemsc_client, telemetry_client |
| **客户端** | client_common, client_channels, **client_windows（wfreerdp.exe）**, **winpr-tools（winpr-hash/makecert）** | X11, Wayland, SDL 客户端, 可执行二进制（除 wfreerdp） |
| **其他** | OpenSSL, **NATIVE_SSPI**（Windows 原生 NLA/CredSSP）, WINDOWS_CERT_STORE, SIMD, SSE2, AVX2, SMARTCARD_PCSC, SMARTCARD_EMULATE, VERBOSE_WINPR_ASSERT, PROGRESS_BAR, TIMEZONE_COMPILED, SERVER_INTERFACE（接口，`WITH_SERVER=OFF`） | Kerberos, CUPS, cJSON, uriparser, PKCS11, LibreSSL, MbedTLS, UNICODE_BUILTIN（Windows 用系统 Unicode） |

> **VAAPI 关闭** (`WITH_VAAPI=OFF`)：Linux 时期因部分 GPU 驱动不兼容（偶发 SIGSEGV）关闭，Windows 版本身不使用 VAAPI，GFX H.264 全部走 FFmpeg 软件解码。
>
> **PRINTER_CLIENT 编译为 ON**：Windows 版通道总开关默认全开，但 qf-client 实际不加载打印机重定向。
>
> **WITH_MEDIA_FOUNDATION=OFF**：FreeRDP 编译时未启用 WMF，rdpecam 摄像头通道仍会注册（`device:*`），摄像头采集由 rdpecam 通道机制处理。

### VDIClient (Qt Widgets)

| 分类 | 说明 |
|------|------|
| 框架 | Qt 6.11.1 Core + Widgets + Network |
| C++ 标准 | C++17 |
| 编译 | MSVC（Visual Studio 17 2022 生成器，-A x64） |
| 部署 | `build_and_package.bat`（复制 qf-client → CMake → windeployqt → VC 运行库 → Inno Setup 打包） |
| SSL | QSslSocket + Windows Schannel 后端（`tls/qschannelbackend.dll`，windeployqt 部署），`VerifyNone` |

### qf-client (QML + FreeRDP)

| 分类 | 说明 |
|------|------|
| 框架 | Qt 6.11.1 Core + Gui + **GuiPrivate** + Qml + Quick |
| C++ 标准 | C++20 (qf-client 源码) / C17 (ref-tmp cliprdr 等 C 源码) |
| 编译器 | MSVC（cl.exe，Ninja） |
| 日志 | spdlog + fmt（控制台，`SPDLOG_LEVEL` 环境变量控制级别） |
| FreeRDP | 自编译 3.28.0 (MSVC)，通过 `CMAKE_PREFIX_PATH` 引用到 `freerdp-3.28.0/install/` |
| 渲染 | **D3D11 原生纹理** + `QSGSimpleTextureNode`：CPU staging buffer → `ID3D11DeviceContext::UpdateSubresource` 上传 → `QRhiTexture::createFrom()` 包装 → Qt Scene Graph (D3D11) 渲染 |

---

## 功能对照表

| 功能模块 | mstsc (Windows 原生) | 官方 FreeRDP | 你的 Qt + FreeRDP | 差距 | 功能实现原理 |
| --- | --- | --- | --- | --- | --- |
| RDP 基础连接 | ✅ | ✅ | ✅ | libfreerdp 库完整集成 | `freerdp_new`+`freerdp_context_new`+`freerdp_connect`；PreConnect/PostConnect 回调配置参数；失败自动重试 3 次（间隔 500ms）；三态状态机支持重连 |
| TLS/NLA/CredSSP 认证 | ✅ | ✅ 完整支持 | ⚠️ NLA 可用，证书跳过验证 | 缺少完整证书管理 | FreeRDP 内置 NLA (CredSSP)，`WITH_NATIVE_SSPI=ON` 使用 Windows 原生 SSPI；`IgnoreCertificate=TRUE` 跳过证书校验 |
| Microsoft Entra SSO | ✅ | ✅ | ❌ | 未实现；`WITH_AAD=OFF` | — |
| VDIClient 登录/VM 管理 | ❌ (mstsc 无独立登录器) | ❌ | ✅ **QWidgets 登录界面 + VM 列表管理** | 自研 VDI 管理客户端 | `LoginWindow` 管理：服务器健康检查、Token 认证、虚拟机列表/状态查询、开机/关机/重启/还原、心跳保活、多语言 |
| 用户名密码登录 | ✅ | ✅ | ✅ ✅ **双重** | qf-client 和 VDIClient 各有一套凭据管理 | qf-client: `FreeRDP_Username`/`FreeRDP_Password`；VDIClient: REST API `/api/v1/auth/login` 获取 Token，记住密码 / 自动登录 |
| Drive 重定向 | ✅ | ✅ `/drive` | ✅ | 由 CLI/.rdp 文件配置，重连时通过 `g_saved_drive_args` 恢复 | `freerdp_client_add_device_channel(settings, 3, {"drive", name, path})`；`FreeRDP_DeviceRedirection=TRUE` 通过 rdpdr 通道实现 |
| USB 重定向 | ❌ (mstsc 原生不支持) | ✅ `/usb` | ✅ | URBDRC 通道 + libusb 枚举，由 CLI `/usb:` 或 .rdp 文件 `usbdevicestoredirect` 显式启用；**默认禁用**；USB 设备需 UsbDk 驱动（安装包自动静默安装） | 标志持久化 + 悬浮栏设备选择（`id:vid:pid`）+ 重连时恢复；热插拔监听 `LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED/LEFT` |
| 摄像头重定向 | ✅ (Win10+) | ✅ `/camera` | ✅ | **硬编码启用**，默认重定向所有摄像头设备 (`device:*`)，无运行时 UI 控制 | `freerdp_client_add_dynamic_channel(settings, 2, {RDPECAM_DVC_CHANNEL_NAME, "device:*"})`；WMF 后端 |
| 多显示器 | ✅ `/multimon`（最多 16 屏） | ✅ `/multimon` | ⚠️ 基础单屏分辨率变化 | 缺少多 Monitor Layout 支持 | disp DVC `SendMonitorLayout` 发送单个 primary 布局 |
| 选择显示器 | ✅ | ✅ | ❌ | 未实现 | — |
| 多屏切换 | ✅ | ✅ | ❌ | 未实现 | — |
| 动态分辨率 | ❌（mstsc 不支持） | ✅ | ✅ **300ms 消抖 + 4px 对齐 + 全帧纹理更新** | 窗口调整后 RDP 分辨率自动同步 | disp DVC `SendMonitorLayout` → GFX_RESET；300ms QTimer 消抖；4px 对齐请求；渲染线程 `UpdateSubresource` 全帧更新 |
| 字体平滑 (ClearType) | ✅ | ✅ | ✅ | 显式启用 | `FreeRDP_AllowFontSmoothing=TRUE`；PerformanceFlags 发送 PERF_ENABLE_FONT_SMOOTHING 位 |
| RD Gateway (TSG) | ✅ | ✅ `/g:` | ✅ | PAA token 认证，网关隧道通过 TSG 协议建立 | `GatewayHostname`/`GatewayPort`/`GatewayAccessToken`/`GatewayCredentialsSource=5` |
| Restricted Admin 模式 | ✅ | ✅ `/restricted-admin` | ❌ | 未实现 | — |
| Remote Credential Guard | ✅ | ✅（3.9+） | ❌ | 未实现 | — |
| Pass-the-Hash 登录 | ❌ | ✅ `/pth:hash` | ❌ | 未实现 | — |
| TLS 版本/密码配置 | ✅ | ✅ | ❌ | 未实现 | — |
| 证书管理 UI | ✅ | ✅ `/cert:tofu\|name\|fingerprint` | ⚠️ 仅跳过验证（IgnoreCertificate=TRUE） | 缺少 UI 化证书管理 | 当前仅支持 `/cert:ignore` |
| RemoteFX (GFX H.264) | ✅（Win7~10） | ✅ | ✅ GFX pipeline 硬编码开启（H.264/AVC444） | 使用 FFmpeg 软件解码 | `SupportGraphicsPipeline=TRUE`, `GfxH264=TRUE`, `GfxAVC444=TRUE`, `GfxAVC444v2=TRUE`；`WITH_GFX_H264=ON` 编译；`WITH_OPENH264=ON` |
| AVC444/H.264 解码 | ✅（Win10+） | ✅ | ✅ AVC420+AVC444+AVC444v2 全开 | CPU 软件解码 | `FreeRDP_GfxH264=TRUE` 启用 AVC420；`FreeRDP_GfxAVC444=TRUE` 启用 AVC444；`FreeRDP_GfxAVC444v2=TRUE` 启用 AVC444v2 |
| GFX AV1 编解码 | ✅（Win11+） | ✅ `/gfx:av1` | ❌ | 未实现；`WITH_GFX_AV1=OFF` | — |
| 窗口缩放（Smart Sizing） | ✅ | ✅ | ✅ | 一致（等比缩放） | Qt 场景图自动缩放 D3D11 纹理四边形以填充窗口 |
| 自适应缩放 | ✅ | ✅ | ✅ | 一致 | 脏矩形按服务器分辨率等比映射到窗口坐标 |
| 全屏模式 | ✅（`/f` 参数） | 完整支持 | ✅ | 默认全屏 + 切换按钮退出/进入 | QML `Window.FullScreen` 初始状态；`toggleDisplayMode()` |
| 窗口最小尺寸 | ✅ | ❌ | ✅ | minimumWidth=650 / minimumHeight=550 | QML `Window` 元素设置最小尺寸约束 |
| 无边框全屏 | ❌ | ✅ | ❌ | 已切回系统原生标题栏 | 使用系统原生窗口装饰 |
| 悬浮工具栏（连接栏） | ✅（顶部固定连接栏） | ✅ | ✅ | 顶部居中热区 + 3s 自动隐藏 + 图钉固定 + CAD/USB/全屏/最小化按钮 | QML `MouseArea` 检测 → 显示工具栏 → 离开启动 3s 隐藏定时器 |
| 多窗口模式 | ✅（多个 mstsc） | ✅ | ❌ | 未实现 | — |
| RemoteApp | ✅ | ✅ | ❌ | 未实现；`CHANNEL_RAIL=ON` 已编译但未使用 | — |
| RemoteApp 无缝窗口 | ✅ | ✅ | ❌ | 未实现 | — |
| 远程协助 / 会话影子 | ✅ | ✅ | ❌ | 未实现 | — |
| 鼠标输入 | ✅ | ✅ | ✅ | 坐标缩放映射到 RDP 分辨率 | `mouseEventScaleSend()` → `freerdp_input_send_mouse_event()` |
| 相对鼠标模式 | ✅ | ✅ | ❌ | 未实现 | — |
| 鼠标捕获 | ✅ | ✅ | ❌ | 未实现 | — |
| 鼠标侧键 (XButton1/2) | ✅ | ✅ | ❌ | 未实现 | — |
| RDP Pointer（光标通道） | ✅ | ✅ | ✅ | 完整实现：Pointer 六回调 + XOR/AND 解码 + unordered_map 缓存 + SYSPTR_NULL 自动隐藏 + 3px 移动阈值恢复 + 2000ms 防抖 | `graphics_register_pointer` 六回调；`freerdp_image_copy_from_pointer_data()` 解码 → QCursor |
| 键盘扫描码 | ✅ | ✅ | ✅ | 一致 | `qf::to_freerdp_key_code()` 查找表 → `freerdp_input_send_keyboard_event_ex()` |
| Unicode 输入 | ✅ | ✅ | ✅ | 扫描码回退到 Unicode | 键码映射失败时回退到 `freerdp_input_send_unicode_keyboard_event()` |
| 输入法 IME | ✅ | ⚠️ | ❌ | 中文输入可能有问题 | 未实现 IME 专用通道 |
| 触屏 / 多点触控 | ✅ | ✅ | ❌ | 未实现；`CHANNEL_RDPEI=ON` 已编译但未使用 | — |
| 触笔（Pen）输入 | ✅ | ✅ | ❌ | 未实现 | — |
| 粘贴文本 | ✅ | ✅ | ✅ **由 `/clipboard` 参数控制，默认禁用** | 一致 | cliprdr 通道；由 CLI 参数 `/clipboard` 控制（默认禁用） |
| 剪贴板图片 | ✅（PNG/DIB/DIBV5） | ✅ | ✅ **由 `/clipboard` 参数控制，默认禁用** | 一致 | cliprdr 通道：PNG/DIB/DIBV5 → `QImage::fromData()`/`imageFromDib()` → `QClipboard::setImage()` |
| 剪贴板文件 | ✅ | ✅ | ✅ **由 `/clipboard` 参数控制，默认禁用** | 一致 | `FileGroupDescriptorW` 解析 → 独立工作线程 64KB 分块下载；由 `/clipboard` 控制；`cliprdr_file_context_new` + `clipboard_entry` worker |
| 麦克风输入（音频输入） | ✅ | ✅ | ✅ | audin DVC + **WinMM 后端**（`sys:winmm`）；检测到麦克风设备才启用（`waveInGetNumDevs()`） | `freerdp_client_add_dynamic_channel(settings, 2, {AUDIN_CHANNEL_NAME, "sys:winmm"})`；`FreeRDP_AudioCapture` 按设备存在性设置 |
| RDPSND 声音输出 | ✅ | ✅ | ✅ | FreeRDP 库内建 **WASAPI** 后端（`WITH_WASAPI=ON` 编译进 freerdp-client3.dll） | rdpsnd 走库内 Windows 后端；PCM 播放 |
| 动态虚拟通道 DVC | ✅ | ✅ | ✅ | rdpsnd/GFX/disp/cliprdr/urbdrc/ecam/audin DVC 均已使用 | 通过 `freerdp_client_add_dynamic_channel` 注册；`ChannelConnected` 回调保存上下文 |
| 多媒体重定向 (TSMF) | ⚠️ | ✅ | ❌ | 未实现；`CHANNEL_TSMF=OFF` | — |
| 图形通道 | GDI/RDP | GDI/OpenGL | ✅ **CPU 转存帧缓冲 + D3D11 原生纹理 + QSGSimpleTextureNode** | GFX 解码到 GDI primary buffer → `copyFrameData()` 拷贝脏矩形到 staging buffer → 渲染线程 `beforeRendering` 中 `UpdateSubresource` 全帧上传 → `QSGSimpleTextureNode` 渲染 | 纹理只创建一次（`CreateTexture2D(B8G8R8A8_UNORM, DEFAULT, BIND_SHADER_RESOURCE)` + `QRhiTexture::createFrom()`），每帧复用 |
| RemoteFX Codec | ✅（Win7~10） | ✅ | ✅ GFX H.264 硬编码开启 | 使用 FFmpeg H.264 软件解码 | `WITH_GFX_H264=ON` 编译 |
| Bitmap Cache | ✅ | ✅ | ✅ FreeRDP 默认开启 | BitmapCacheV3 已启用 | 默认值 |
| 网络自动优化 | ✅ | ✅ | ✅ | NetworkAutoDetect=TRUE | 带宽估算 |
| 数据压缩 | ✅ | ✅ `/compression` | ✅ | CompressionEnabled=TRUE | MPPC/MPPC-8k 无损压缩 |
| Bandwidth Auto Detect | ✅ | ✅ | ✅ | NetworkAutoDetect=TRUE | 标准带宽自适应 |
| UDP Transport | ✅（RDP 10 UDP 优先） | ✅ | ✅ | SupportMultitransport=TRUE | UDP 优先，失败回退 TCP |
| RDPEUDP2 | ✅ | ✅ | ✅ | SupportMultitransport=TRUE 时自动协商 | — |
| TCP fallback | ✅ | ✅ | ✅ | 默认 | TCP；`TcpConnectTimeout=15s` |
| Smartcard Login | ✅ | ✅ | ❌ | 未实现；`CHANNEL_SMARTCARD=ON` + `SMARTCARD_PCSC=ON` 已编译 | — |
| Kerberos SSO | ✅ | ✅ | ❌ | 不可用；`WITH_KRB5=OFF` | — |
| Windows Hello | ✅（Win10+） | ⚠️ | ❌ | 未实现 | — |
| GPO 策略兼容 | ✅ | ✅ | 依赖协议 | 未验证 | — |
| 多会话管理 | ❌ 客户端职责 | ❌ 客户端职责 | ✅ **VDIClient VM 列表管理** | 完整的 VDI 虚拟机列表管理 | VDIClient 通过 REST API 管理多个 VM 的开机/关机/重启/恢复/连接 |
| 断线重连 | ✅（自动重连） | ⚠️ | ✅ | 三态状态机 + 3 次重试 + 重连后清理上下文 | `WaitForMultipleObjects` 100ms 轮询 → `freerdp_shall_disconnect_context`/`g_reconnectRequested`（USB 切换触发）→ `freerdp_disconnect`+`freerdp_connect` 最多 3 次 |
| 自动恢复 Session | ⚠️ | ⚠️ | ✅ | `freerdp_disconnect` → `freerdp_connect` → my_pre_connect 清理旧设备/通道 → my_post_connect 重新初始化 GDI/指针回调 | 重连时 channel contexts 已释放：`my_pre_connect()` 中 `freerdp_device_collection_free()`+`freerdp_dynamic_channel_collection_free()` 清理旧设备；`g_dispContext/g_gfxContext/g_clipboard_client_context` 置空防止残指针回调 |
| 联网认证—Token 过期处理 | ✅ | ✅ | ✅ **VDIClient 处理** | Token 过期自动返回登录页、终止 RDP 进程 | VDIClient `isTokenExpired()` 检测 HTTP 401 → `handleTokenExpired()`: 清 Token、终止 qf-client、切回登录页 |
| VDI—快照管理 | ❌ (mstsc 无此功能) | ❌ | ✅ | 通过 REST API 查询/恢复 Milestone 快照 | VDIClient `fetchVmSnapshot()` → `onVmSnapshotReply()` 显示/隐藏还原按钮 |
| VDI—心跳保活 | ❌ | ❌ | ✅ | 每 15 秒发送心跳维持会话 | VDIClient `startHeartbeat()` → QTimer 15s → `/api/v1/users/heartbeat` POST |
| VDI—修改密码 | ❌ | ❌ | ✅ | 通过服务器 API 修改密码 | VDIClient `onChangePasswordClicked()` → 密码对话框 → `PUT /api/v1/users/password` |
| VDI—记住密码 / 自动登录 | ❌ | ❌ | ✅ | QSettings 持久化 + 启动自动登录 | VDIClient `saveSettings()`/`loadSettings()` 使用 QSettings |
| VDI—多语言 | ❌ | ❌ | ✅ | 中/英/日/繁四语言 | VDIClient `initTranslations()` 内嵌字典 + `updateLanguage()` 即时切换 |
| /admin 管理连接 | ✅ | ✅ `/admin` | ❌ | 未实现 | — |
| 网络连接类型选择 | ✅ | ✅ `/network:...` | ❌ | 未实现 | — |
| 代理支持 | ✅ | ✅ `/proxy:...` | ❌ | 未实现 | — |
| Hyper-V 控制台 | ✅ | ✅ `/vmconnect` | ❌ | 未实现 | — |
| RDP2TCP 隧道 | ❌ | ✅ `/rdp2tcp` | ❌ | 未实现；`CHANNEL_RDP2TCP=OFF` | — |
| 录音/回放 | ❌ | ✅ `/dump` | ❌ | 未实现 | — |
| 键盘布局/语言配置 | ✅ | ✅ `/kbd:...` | ❌ | 未实现 | — |
| Alternate Shell | ✅ | ✅ `/shell:...` | ❌ | 未实现 | — |
| 公共模式（Public Mode） | ✅ `/public` | ✅ | ❌ | 未实现 | — |
| 日志系统 | ✅ | CLI 日志 | ✅ **spdlog/fmt + VDIClient 日志** | qf-client: spdlog 控制台（`SPDLOG_LEVEL` 可调）；VDIClient: `qDebug()/qInfo()` 输出 | 两份日志独立输出 |
| 参数配置文件 / CLI | ✅（.rdp 文件） | cmd 参数 | ✅ | `freerdp_client_settings_parse_command_line()` 解析 | 在 `my_pre_connect()` 中执行一次（初始连接）；`g_cli_argc/g_cli_argv` 全局变量保存；重连时跳过解析但恢复 `/drive:` `/usb:` 配置 |
| 交替 Shell / 启动程序 | ✅ | ✅ | ❌ | 未实现 | — |
| Public Mode (公共模式) | ✅ | ✅ | ❌ | 未实现 | — |
| KDC Proxy | ✅ | ✅ `/kdcproxy` | ❌ | 未实现 | — |
| 插件体系 | ❌ | channels | ❌ | 未实现 | — |
| 证书管理 | ✅ | 完整 | ⚠️ 仅跳过验证 | 需要 UI 化 | `FreeRDP_IgnoreCertificate=TRUE` |
| 多语言 | ✅ | 部分 | ✅ **VDIClient 支持 4 语言** | qf-client 使用了 `qsTr()` 但未配置多语言翻译文件 | QML `qsTr()` 占位；VDIClient 独立 QWidgets 多语言系统 |

---

## 性能优化记录

### 已实施优化

| 优化 | 问题 | 解决方式 | 效果 |
|------|------|---------|------|
| **DMA-BUF → CPU Staging Buffer 迁移**（Linux 时期） | DMA-BUF + EGL/GBM 路径偶发 SIGSEGV (exit 11)，部分 GPU 驱动兼容性差 | 改用 `std::vector<uint8_t>` 作为 CPU 侧 staging buffer。移除了 EGL Image / GBM BO / DMA-BUF FD 等复杂依赖 | ✅ 消除 EGL/GBM 相关崩溃 |
| **D3D11 原生纹理方案**（Windows） | QImage 拷贝 + 软渲染性能差；`QNativeInterface::QSGD3D11Texture::fromNative()` 在 Qt 6.11 渲染线程崩溃（访问违例） | 改用 `ID3D11Device` 自建纹理：`CreateTexture2D(B8G8R8A8_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE)` → `QRhiTexture::createFrom()` → 每帧 `UpdateSubresource()` 直接上传，在 `beforeRendering`（渲染线程）完成 | ✅ 稳定，无 QImage 拷贝 |
| **createFrom 崩溃修复** | `QRhiTexture::createFrom()` 创建 SRV 时崩溃 | 纹理缺少 `D3D11_BIND_SHADER_RESOURCE`，补上后稳定；纹理与设备 `AddRef/Release` 管理，防 QRhi 释放后设备失效 | ✅ 崩溃消除 |
| **纹理过滤 Nearest** | 远程桌面文字发虚 | `SetFiltering(Nearest)` | ✅ 文字锐利 |
| **全帧上传替代脏矩形** | 使用 dirty rect 上传时 stride 不匹配导致纹理错位 | 改用全帧上传，避免跨 stride 复杂度 | ✅ 纹理正确 |
| **glViewport 显式设置**（Linux 时期遗留） | 窗口缩放后 Qt 场景图缓存旧视口，新区域黑屏 | Windows 版由 Qt Scene Graph 管理视口，无此问题 | — |
| **剪贴板改为 CLI 控制** | 硬编码启用，无法通过 CLI 关闭 | CLI 解析前先设 `FreeRDP_RedirectClipboard=FALSE`，由 `/clipboard` 决定 | ✅ 默认禁用，`/clipboard` 启用 |
| **重连时清理通道上下文** | `freerdp_disconnect` 释放 channel contexts 后指针悬空 | `my_pre_connect()` 中清理旧设备集合；`g_dispContext/g_gfxContext/g_clipboard_client_context` 置空 | ✅ 消除重连时残指针回调 |
| **VDIClient 启动路径泛化** | 硬编码 `bin/` 查找路径 | 基于 `applicationDirPath()` 查找 `bin/` 目录（当前目录 → 逐级向上） | ✅ 支持不同部署目录结构 |
| **初始分辨率使用窗口实际尺寸** | QML 窗口未 mapped 时 item size 很小 | 优先使用 CLI 分辨率，其次 item 尺寸 × devicePixelRatio，最后回退 1024x768 | ✅ 初始 RDP session 分辨率合理 |
| **分辨率 4px 对齐** | RDP 服务器将请求分辨率对齐到 4 的倍数，不匹配导致黑边 | 请求前 `(w+3)&~3u` 预对齐 | ✅ 请求分辨率与服务器返回一致 |

### 架构变更记录

| 时间 | 变更 | 原因 |
|------|------|------|
| Linux 初始 | DMA-BUF 零拷贝 + EGL Image + GBM | 追求性能最优 |
| Linux k1 | DMA-BUF + VAAPI 硬件解码 | 降低 CPU 负载 |
| Linux k2 | 完整功能版 | 窗口缩放 glViewport 修复 |
| Linux k3 | 移除 VAAPI | 部分 GPU 驱动不兼容 |
| Linux k4 | 移除 DMA-BUF | EGL/GBM 偶发崩溃 |
| Linux k5 | 窗口花屏优化 | 纹理上传错位修复 |
| Linux k6 | 音频后端重构 | rdpsnd/audin 改为 FreeRDP 库自带后端 |
| Linux k7 | 渲染透明度修复 | 片段着色器强制 `fragColor.a=1.0` + staging buffer Alpha 初始化 |
| **Windows w1** | **迁移至 Windows (MSVC 2022)** | 目标平台切换为 Windows |
| **Windows w2** | **D3D11 原生纹理方案** | `QNativeInterface::QSGD3D11Texture::fromNative()` 渲染线程崩溃，改用 `CreateTexture2D` + `QRhiTexture::createFrom()` |
| **Windows w3** | **createFrom 参数修复** | 缺 `D3D11_BIND_SHADER_RESOURCE` 崩溃，补齐后稳定 |
| 当前 | CPU staging buffer + D3D11 原生纹理 + 全帧更新 | 稳定优先 |

### 已知瓶颈

| 瓶颈 | 说明 | 上限 |
|------|------|------|
| 服务端 H.264 编码速率 | 服务端发送 GFX 瓦片的速率 | ~50-60fps |
| invokeMethod 跨线程开销 | `QMetaObject::invokeMethod(QueuedConnection)` 投递延迟 | ~7ms/帧 |
| 场景图 vsync 同步 | Qt 场景图以显示器刷新率为节拍 | 60fps |
| **CPU 帧拷贝** | `copyFrameData()` memcpy 脏矩形 + `UpdateSubresource` 全帧上传 | 较零拷贝方案多 5-15% CPU 开销 |
| **软件解码** | GFX H.264 用 FFmpeg 软件解码 | 解码耗时 ~33ms（高分辨率时） |

---

## 编译配置详情

### FreeRDP 3.28.0（Windows，等效 `build-freerdp.ps1`）

完整 CMake 配置见 `freerdp-3.28.0/build/CMakeCache.txt`。核心编译命令：

```powershell
# 1) 进入 MSVC x64 环境
& "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64

# 2) 配置 + 编译 + 安装
cd C:\Users\Administrator\Desktop\VDIClient\freerdp-3.28.0\build
cmake .. -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_INSTALL_PREFIX="$PWD\install" `
  "-DCMAKE_TOOLCHAIN_FILE=C:\Users\Administrator\Desktop\workspace\vcpkg\scripts\buildsystems\vcpkg.cmake" `
  -DWITH_WASAPI=ON -DWITH_FFMPEG=ON -DWITH_SWSCALE=ON -DWITH_OPENH264=ON `
  -DWITH_OPENSSL=ON -DWITH_CHANNELS=ON -DWITH_CLIENT_COMMON=ON `
  -DWITH_DSP_FFMPEG=ON -DWITH_SSE2=ON `
  -DCHANNEL_URBDRC=ON -DCHANNEL_RDPECAM_CLIENT=ON -DCHANNEL_GEOMETRY=ON `
  -DWITH_PULSE=OFF -DWITH_ALSA=OFF -DWITH_CUPS=OFF -DWITH_PCSC=OFF `
  -DWITH_VAAPI=OFF -DWITH_GSM=OFF -DWITH_SERVER=OFF -DWITH_PROXY=OFF `
  -DWITH_CLIENT_SDL=OFF -DWITH_MANPAGES=OFF
cmake --build . --config Release --parallel
cmake --install . --config Release
```

### qf-client（Windows，等效 `build-qf-client.ps1`）

```powershell
cd C:\Users\Administrator\Desktop\VDIClient\qfreerdp-windows\build
cmake .. -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  "-DCMAKE_PREFIX_PATH=C:/Qt/6.11.1/msvc2022_64;C:/Users/Administrator/Desktop/VDIClient/freerdp-3.28.0/install" `
  "-DCMAKE_TOOLCHAIN_FILE=C:/Users/Administrator/Desktop/workspace/vcpkg/scripts/buildsystems/vcpkg.cmake"
cmake --build . --config Release --parallel
```

> 注：qf-client 的 `CMakeLists.txt` 使用 `list(PREPEND CMAKE_PREFIX_PATH .../freerdp-3.28.0/install)` 自动定位自编译 FreeRDP；C++20/C17 混编；MSVC 下 `rc.exe` 编译 `resources.rc` 嵌入图标；`/FORCE:MULTIPLE` 解决本地 ref-tmp cliprdr 与 freerdp-client3.dll 的重复符号。

### VDIClient（Windows，`build_and_package.bat` 一体化）

```powershell
cd C:\Users\Administrator\Desktop\VDIClient\vdi-client-windows-main
.\build_and_package.bat
```

脚本流程：清理 build/ → 从 `..\qfreerdp-windows\build\` 复制 qf-client.exe + DLL + qml/plugins 到 `bin/`，复制 UsbDk MSI 到 `drivers/` → `cmake -G "Visual Studio 17 2022" -A x64` 配置 → 编译 → `windeployqt --release --no-translations` 部署到 `build\Release\` → 复制 VC++ 运行库 → `ISCC.exe installer.iss` 生成 `installer_output\VDIClient-Setup.exe`。

## 参考
- [FreeRDP GitHub](https://github.com/FreeRDP/FreeRDP)
- [Microsoft Compare Remote Desktop clients](https://learn.microsoft.com/en-us/previous-versions/remote-desktop-client/compare-remote-desktop-clients?pivots=remote-pc)
