# build-freerdp-mac.sh 构建逻辑与依赖版本冲突修复方案

> 关联脚本：[scripts/build-freerdp-mac.sh](../scripts/build-freerdp-mac.sh)
> 适用环境：macOS (Apple Silicon / arm64)，FreeRDP 3.28.0，部署目标 **macOS 13.0+**（13-26 已验证）

## 1. 脚本职责

构建 VDI 客户端所需的 FreeRDP 3.28.0 运行库：

- **只编译库**，不编译 FreeRDP 自带的客户端二进制 / 服务端 / 代理（`WITH_CLIENT=OFF`、`WITH_SERVER=OFF`、`WITH_PROXY=OFF`）
- 产出三大动态库（安装到 `freerdp-3.28.0/install-mac/lib/`）：
  - `libfreerdp3.3.dylib` — 核心 RDP 协议
  - `libfreerdp-client3.3.dylib` — 客户端公共代码 + **全部客户端通道**（静态链接，见第 4 节）
  - `libwinpr3.3.dylib` — Windows 兼容抽象层
- 上层工程通过 `CMAKE_PREFIX_PATH` 指向 `install-mac` 找到 FreeRDP/WinPR 的 CMake 包，运行时以 `@rpath` 加载这些 dylib（改 dylib 后无需重编上层 app）

## 2. 构建流程

```
① 定位目录
   SCRIPT_DIR / FREERDP_DIR / BUILD_DIR / INSTALL_DIR / DEPS_DIR

② 依赖校验
   BREW_PREFIX = $(brew --prefix)
   OPENSSL_ROOT = <brew>/opt/openssl@3   （Homebrew keg-only，必须存在否则退出）

③ CMake 配置（Ninja + Release）   ← 依赖定位的关键，见第 3 节
   -DCMAKE_PREFIX_PATH="$DEPS_DIR;$BREW_PREFIX"   ← .deps-mac13 自编译库优先
   -DOPENSSL_ROOT_DIR="$OPENSSL_ROOT"             ← 显式指定 OpenSSL@3

④ 编译  cmake --build "$BUILD_DIR" --parallel
⑤ 安装  cmake --install "$BUILD_DIR"  →  install-mac/
```

### 关键 CMake 选项说明

| 选项 | 值 | 作用 |
|------|-----|------|
| `CMAKE_PREFIX_PATH` | `$DEPS_DIR;$BREW_PREFIX` | 依赖查找路径，**自编译精简库优先于 Homebrew** |
| `OPENSSL_ROOT_DIR` | brew OpenSSL@3 | TLS 加密库（keg-only，需显式指定） |
| `CMAKE_OSX_DEPLOYMENT_TARGET` | `13.0` | 最低部署系统 macOS 13（同主工程 CMakeLists 写死，见第 5 节） |
| `WITH_FFMPEG / WITH_SWSCALE / WITH_DSP_FFMPEG` | ON | H.264 软解 + 颜色转换 + 音频 DSP |
| `WITH_MACAUDIO` | ON | 音频后端用 CoreAudio（rdpsnd/audin -> sys:mac） |
| `CHANNEL_RDPECAM_CLIENT` | ON | 启用摄像头重定向通道（默认 OFF） |
| `WITH_CLIENT/WITH_SERVER/WITH_PROXY` | OFF | 关闭客户端二进制/服务端/代理 |
| `WITH_CLIENT_MAC / WITH_CLIENT_SDL` | OFF | 不编译 FreeRDP 自带 UI 客户端 |
| `WITH_X11/WITH_WAYLAND/...` | OFF | 精简平台后端，仅保留 macOS 所需 |

## 3. 依赖版本冲突修复方案（本次修改核心）

### 3.1 现象

2026-08-05 全量重建 + 打包后，`scripts/check-bundle.sh` 自检 **FAIL**：

```
UNRESOLVED: .../qf-client.app/Contents/Frameworks/libfreerdp3.3.dylib -> @rpath/libswscale.9.dylib
UNRESOLVED: .../libfreerdp3.3.dylib -> @rpath/libavcodec.62.dylib
UNRESOLVED: .../libfreerdp3.3.dylib -> @rpath/libavutil.60.dylib
...（共 7 个 FFmpeg 依赖无法解析）
```

### 3.2 根因分析

两套 FFmpeg 版本不一致，且 FreeRDP 链接了错误的那套：

| 来源 | 路径 | 版本 | 谁在用 |
|------|------|------|--------|
| **Homebrew FFmpeg** | `/opt/homebrew/opt/ffmpeg/lib` | `libavcodec.62 / libavutil.60 / libswscale.9 / ...` | 被 FreeRDP 链接（错误） |
| **自编译精简 FFmpeg** | `.deps-mac13/lib` | `libavcodec.61 / libavutil.59 / libswscale.8 / ...`（minos 13.0，无 Homebrew 传递依赖） | 被 `package-dmg.sh` 打包进 bundle（正确） |

原因链：

1. 原脚本 `CMAKE_PREFIX_PATH` 只设置为 `$BREW_PREFIX`，`FindFFmpeg.cmake` 因此命中 **Homebrew** 的 FFmpeg
2. FreeRDP dylib 记录了对 Homebrew FFmpeg（`/opt/homebrew/opt/ffmpeg/lib/libavcodec.62.dylib` 等）的绝对路径依赖
3. `package-dmg.sh` 只收集 `.deps-mac13` 的 FFmpeg（61 版本）进 bundle，`install_name_tool` 把 FreeRDP 的引用改成 `@rpath/libswscale.9.dylib` 等
4. bundle 里只有 `libswscale.8.dylib` → `@rpath/libswscale.9.dylib` 找不到 → 运行即崩溃

### 3.3 修复方案（代码变更）

`scripts/build-freerdp-mac.sh` 增加 `.deps-mac13` 依赖目录，并放到 `CMAKE_PREFIX_PATH` **最前面**（CMake 按列表顺序优先查找）：

```diff
 BREW_PREFIX="$(brew --prefix)"
 OPENSSL_ROOT="$BREW_PREFIX/opt/openssl@3"
+# 自编译精简 FFmpeg（无 Homebrew 传递依赖，打包友好），优先于 Homebrew FFmpeg
+DEPS_DIR="$(cd "$SCRIPT_DIR/../.deps-mac13" && pwd)"

 cmake -S "$FREERDP_DIR" -B "$BUILD_DIR" -G Ninja \
   ...
-  -DCMAKE_PREFIX_PATH="$BREW_PREFIX" \
+  -DCMAKE_PREFIX_PATH="$DEPS_DIR;$BREW_PREFIX" \
```

核心思路：**让 FreeRDP 链接的依赖版本与 `package-dmg.sh` 收集进 bundle 的版本保持一致**。由于 bundle 依赖自编译精简 FFmpeg（干净、无传递依赖、部署友好），FreeRDP 侧必须同样使用 `.deps-mac13` 的 FFmpeg。

> 注意：Homebrew FFmpeg 还依赖 zlib/bzip2/libiconv 等大量传递库，直接打进 bundle 会导致依赖面爆炸，因此**反向方案（让打包脚本改用 Homebrew FFmpeg）不可取**，修复必须落在构建侧。

### 3.4 验证方法

修复后需**删除 `build-mac/` 重新配置**（CMake 的 `FFMPEG_INCLUDE_DIRS/FFMPEG_LIBRARIES` 等 find 结果缓存不会因 `CMAKE_PREFIX_PATH` 变化自动失效，残留路径会导致白跑）：

```bash
rm -rf freerdp-3.28.0/build-mac
bash scripts/build-freerdp-mac.sh
```

验证 FreeRDP 已正确链接精简版 FFmpeg（应显示 61/59/8/5 版本、`.deps-mac13` 绝对路径）：

```bash
otool -L freerdp-3.28.0/install-mac/lib/libfreerdp3.3.28.0.dylib | grep -iE "av|sw"
# 期望: .../.deps-mac13/lib/libswscale.8.dylib
#       .../.deps-mac13/lib/libavcodec.61.dylib ...
```

或检查 CMake 缓存：

```bash
grep -E "^(FFMPEG_INCLUDE_DIRS|FFMPEG_LIBRARIES)" freerdp-3.28.0/build-mac/CMakeCache.txt
# 期望路径均在 /Users/kk/Downloads/vdi-client-mac-main/.deps-mac13/ 下
```

完整链路验证（重编主工程 → 打包 → 自检）：

```bash
cmake --build qfreerdp-vdi-client/build
bash scripts/package-dmg.sh
bash scripts/check-bundle.sh   # 期望输出: PASS: 所有依赖均可在 bundle 内解析
```

## 4. 其他注意事项

- **`install-mac/lib/freerdp3/` 目录不存在属正常**：FreeRDP 3.x 的客户端通道（cliprdr/rdpgfx/rdpecam/drdynvc/rdpdr 等）是**静态子系统**，编译进 `libfreerdp-client3.dylib`，运行时通过 addin 表直接调用，不需要独立插件 dylib。验证通道已编入：

  ```bash
  nm -m freerdp-3.28.0/install-mac/lib/libfreerdp-client3.3.28.0.dylib | grep -iE "ecam|avf"
  # 应见 CamAvfSampleDelegate / CLIENT_RDPECAM_SUBSYSTEM_TABLE 等符号
  ```

- **改库后同步 install**：修改 FreeRDP 源码重编后，若 `cmake --install` 判定 install-mac 中 dylib "Up-to-date" 跳过复制，需核对 `stat` 两个路径的 mtime，必要时手动 `cp` + `install_name_tool` 修正 rpath + 重新 ad-hoc 签名。
- **干净重建时删缓存**：`freerdp-3.28.0/build-mac/`、`freerdp-3.28.0/install-mac/`、`qfreerdp-vdi-client/build/`、`build/`（打包产物，含 `package/` staging 与 DMG）均可安全删除；`.deps-mac13/` 是预编译输入依赖，**不可删**。从零到最终 DMG 的完整可重复构建命令见第 7 节（2026-08-05 已验证通过）。

## 5. macOS 部署目标（13.0）

为避免在高版本系统上编译出 `LC_BUILD_VERSION minos = 26.x` 的产物导致旧系统无法运行，部署目标在**构建侧**固定：

| 位置 | 配置 |
|------|------|
| `scripts/build-freerdp-mac.sh` | `-DCMAKE_OSX_DEPLOYMENT_TARGET=13.0` |
| `qfreerdp-vdi-client/CMakeLists.txt` | `set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0")`（写死，任何 configure 都生效） |

验证（bundle 内全部二进制 minos 均应 ≤ 13.0）：

```bash
vtool -show-build <任意主程序或 dylib> | awk '/minos/{print $2; exit}'
```

`.deps-mac13` 预编译库本身即为 minos 13.0；Qt 6.11.1 官方库亦支持 13.0+；代码未使用 26 特有 API → **最终产物支持 macOS 13-26**（arm64）。

## 6. FreeRDP 源码级修改记录（麦克风重定向，2026-08-05）

本仓库在 `freerdp-3.28.0/` 源码上直接修改了 3 处，解决 audin（麦克风）重定向无声问题（这些改动随 FreeRDP 一起编译，重建脚本已自动包含）：

| 文件 | 修改 | 解决 |
|------|------|------|
| `channels/audin/client/mac/audin_mac.m` | `WAVE_FORMAT_PCM` 的 ASBD flags 增加 `kAudioFormatFlagIsPacked` | 缺少 packed 标志导致 `AudioQueueNewInput` 可能失败 |
| `libfreerdp/codec/dsp_ffmpeg.c` | PCM16 映射从 `PCM_U16LE` 改为 `PCM_S16LE` | FFmpeg 无 U16LE 编码器，`swr_convert` 失败 |
| `channels/audin/client/audin_main.c` | 服务器采样率/位深与客户端能力一致时走**直通路径**（跳过 `dsp_context_reset`） | 根治 `dsp_context_reset FAILED`（dsp_context 为 NULL）|

配套的 FreeRDP 摄像头自研后端（AVFoundation `avf/` + VideoToolbox 硬编）与上述修复均随 dylib 分发，无需额外文件。

## 7. 完整可重复构建流程（已验证）

```bash
# ① 清理全部构建产物（.deps-mac13 保留）
rm -rf build freerdp-3.28.0/build-mac freerdp-3.28.0/install-mac qfreerdp-vdi-client/build

# ② FreeRDP（含第 6 节源码修改 + minos 13.0）
bash scripts/build-freerdp-mac.sh

# ③ 主工程（qf-client 内嵌 VDIClient.app）
cmake -S qfreerdp-vdi-client -B qfreerdp-vdi-client/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_PREFIX_PATH="/Users/kk/.local/share/qt-official/6.11.1/macos;$(pwd)/.deps-mac13"
cmake --build qfreerdp-vdi-client/build --parallel 8

# ④ 打包 DMG + 自检
bash scripts/package-dmg.sh
bash scripts/check-bundle.sh   # 期望 PASS
```
