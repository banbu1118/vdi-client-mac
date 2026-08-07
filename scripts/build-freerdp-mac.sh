#!/bin/bash
# =============================================================
# 构建 FreeRDP 3.28.0 (macOS arm64)
# - 仅编译 qf-client 需要的库（不编译客户端二进制/服务端/代理）
# - 音频使用 CoreAudio (WITH_MACAUDIO=ON, rdpsnd/audin -> sys:mac)
# - 编解码: FFmpeg (H.264 软件解码) + SWScale + DSP FFmpeg
# - OpenSSL 3 (Homebrew keg-only)
# =============================================================
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FREERDP_DIR="$(cd "$SCRIPT_DIR/../freerdp-3.28.0" && pwd)"
BUILD_DIR="$FREERDP_DIR/build-mac"
INSTALL_DIR="$FREERDP_DIR/install-mac"

BREW_PREFIX="$(brew --prefix)"
OPENSSL_ROOT="$BREW_PREFIX/opt/openssl@3"
# 自编译精简 FFmpeg（无 Homebrew 传递依赖，打包友好），优先于 Homebrew FFmpeg
DEPS_DIR="$(cd "$SCRIPT_DIR/../.deps-mac13" && pwd)"

echo "=== FreeRDP 源码目录: $FREERDP_DIR"
echo "=== 构建目录: $BUILD_DIR"
echo "=== 安装目录: $INSTALL_DIR"

if [ ! -d "$OPENSSL_ROOT" ]; then
  echo "[ERROR] OpenSSL@3 not found at $OPENSSL_ROOT"
  exit 1
fi

cmake -S "$FREERDP_DIR" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
  -DCMAKE_PREFIX_PATH="$DEPS_DIR;$BREW_PREFIX" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
  -DOPENSSL_ROOT_DIR="$OPENSSL_ROOT" \
  -DWITH_OPENSSL=ON \
  -DWITH_FFMPEG=ON \
  -DWITH_SWSCALE=ON \
  -DWITH_DSP_FFMPEG=ON \
  -DWITH_MACAUDIO=ON \
  -DWITH_CHANNELS=ON \
  -DCHANNEL_RDPECAM_CLIENT=ON \
  -DWITH_CLIENT_COMMON=ON \
  -DWITH_CLIENT=OFF \
  -DWITH_SERVER=OFF \
  -DWITH_PROXY=OFF \
  -DWITH_MANPAGES=OFF \
  -DWITH_X11=OFF \
  -DWITH_WAYLAND=OFF \
  -DWITH_CLIENT_SDL=OFF \
  -DWITH_CLIENT_MAC=OFF \
  -DWITH_WASAPI=OFF \
  -DWITH_WINMM=OFF \
  -DWITH_MEDIA_FOUNDATION=OFF \
  -DWITH_PULSE=OFF \
  -DWITH_ALSA=OFF \
  -DWITH_OSS=OFF \
  -DWITH_CUPS=OFF \
  -DWITH_GSM=OFF \
  -DWITH_LAME=OFF \
  -DWITH_FAAD2=OFF \
  -DWITH_FAAC=OFF \
  -DWITH_SOXR=OFF \
  -DWITH_AOM=OFF \
  -DWITH_JPEG=OFF \
  -DWITH_VAAPI=OFF

echo
echo "=== 开始编译 (并行) ==="
cmake --build "$BUILD_DIR" --parallel

echo
echo "=== 安装到 $INSTALL_DIR ==="
cmake --install "$BUILD_DIR"

echo
echo "=== 完成 ==="
ls "$INSTALL_DIR/lib/" | grep -E 'freerdp|winpr' || true
ls "$INSTALL_DIR/lib/freerdp3/" | head -30 || true
