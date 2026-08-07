#!/bin/bash
# ============================================================
# vdi-client-mac DMG 打包脚本
# 用法: ./scripts/package-dmg.sh
# 产出: build/VDIClient-<版本>.dmg
#
# 流程:
#   1. 拷贝 VDIClient.app 到 staging（构建期已内嵌 qf-client.app）
#   2. 收集非 Qt 依赖 (freerdp/ffmpeg/openssl/libusb/spdlog/fmt/json-c)
#     到内嵌 qf-client.app/Contents/Frameworks
#   3. install_name_tool 统一改为 @rpath 引用
#   4. macdeployqt 部署 Qt 框架 + QML 模块
#   5. 校验 qf-client.app 已内嵌
#   6. ad-hoc 签名
#   7. hdiutil 生成 DMG
# ============================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PKG_DIR="$ROOT/build/package"
VDI_APP="$PKG_DIR/VDIClient.app"
# qf-client.app 由构建期 POST_BUILD 嵌入 VDIClient.app/Contents/MacOS/bin/
QF_APP="$VDI_APP/Contents/MacOS/bin/qf-client.app"
FW="$QF_APP/Contents/Frameworks"
MACDEPLOYQT="/Users/kk/.local/share/qt-official/6.11.1/macos/bin/macdeployqt"
DEPS="$ROOT/.deps-mac13"
VERSION="1.5.0"

echo "==> [1/7] 准备 staging 目录"
rm -rf "$PKG_DIR"
mkdir -p "$PKG_DIR"
# 只拷贝 VDIClient.app（用户只看到一个应用，qf-client 已内嵌其中）
cp -R "$ROOT/qfreerdp-vdi-client/build/bin/VDIClient.app" "$PKG_DIR/"
mkdir -p "$FW"

echo "==> [2/7] 收集非 Qt 依赖到 Frameworks"
# FreeRDP
cp -L "$ROOT/freerdp-3.28.0/install-mac/lib/libfreerdp3.3.dylib" "$FW/"
cp -L "$ROOT/freerdp-3.28.0/install-mac/lib/libfreerdp-client3.3.dylib" "$FW/"
cp -L "$ROOT/freerdp-3.28.0/install-mac/lib/libwinpr3.3.dylib" "$FW/"
# FFmpeg 主体（自编译精简版，minos 13.0）
cp -L "$DEPS/lib/libavcodec.61.dylib" "$DEPS/lib/libavdevice.61.dylib" \
      "$DEPS/lib/libavfilter.10.dylib" "$DEPS/lib/libavformat.61.dylib" \
      "$DEPS/lib/libavutil.59.dylib" "$DEPS/lib/libswresample.5.dylib" \
      "$DEPS/lib/libswscale.8.dylib" "$FW/"
# 其他（自编译，minos 13.0）
cp -L "$DEPS/lib/libssl.3.dylib" "$DEPS/lib/libcrypto.3.dylib" "$FW/"
cp -L "$DEPS/lib/libopenh264.8.dylib" "$FW/"
cp -L "$DEPS/lib/libusb-1.0.0.dylib" "$FW/"
cp -L "$DEPS/lib/libjson-c.5.dylib" "$FW/"
# brew 安装的库文件只读（444），macdeployqt strip 时需要写权限
chmod u+w "$FW"/*.dylib

echo "==> [3/7] install_name_tool 统一 @rpath 引用"
# 将非系统/非 Qt 绝对路径引用统一改为 @rpath/<basename>
fix_deps() {
    local file="$1"
    local deps
    deps=$(otool -L "$file" | awk 'NR>2 {print $1}')
    for d in $deps; do
        case "$d" in
            /System/*|/usr/lib/*|@rpath/*|@loader_path/*|@executable_path/*|/opt/homebrew/opt/qtbase/*|/opt/homebrew/opt/qtdeclarative/*)
                ;;
            *)
                install_name_tool -change "$d" "@rpath/$(basename "$d")" "$file"
                ;;
        esac
    done
}

# 删除非 @loader_path/@executable_path/@rpath 开头的 LC_RPATH（避免目标主机上掩蔽缺库）
clean_rpaths() {
    local file="$1"
    local rpaths
    rpaths=$(otool -l "$file" | awk '/cmd LC_RPATH/{getline; getline; print $2}')
    for rp in $rpaths; do
        case "$rp" in
            @loader_path|@executable_path|@rpath|@loader_path/*|@executable_path/*)
                ;;
            *)
                install_name_tool -delete_rpath "$rp" "$file"
                ;;
        esac
    done
}

for f in "$FW"/*.dylib; do
    install_name_tool -id "@rpath/$(basename "$f")" "$f"
    install_name_tool -add_rpath @loader_path "$f" 2>/dev/null || true
    fix_deps "$f"
    clean_rpaths "$f"
done

# qf-client 主程序：加 @executable_path/../Frameworks rpath，Qt 引用留给 macdeployqt
QF_EXE="$QF_APP/Contents/MacOS/qf-client"
clean_rpaths "$QF_EXE"
install_name_tool -add_rpath @executable_path/../Frameworks "$QF_EXE" 2>/dev/null || true
fix_deps "$QF_EXE"

echo "==> [4/7] macdeployqt 部署 Qt 依赖"
"$MACDEPLOYQT" "$QF_APP" -qmldir="$ROOT/qfreerdp-vdi-client/qf-client/src"
"$MACDEPLOYQT" "$VDI_APP"

echo "==> [4.5/7] 修复 macdeployqt 部署的第三方 dylib @rpath 引用"
# macdeployqt 拷入 Frameworks 的第三方库（brotli/webp 等）互相以 @rpath/ 引用，
# 但其 LC_RPATH 是 brew 遗留的 @loader_path/../lib，在 app 内无法解析。
# 所有此类库都在 Frameworks 根下，@loader_path 即可命中，统一改为 @loader_path/xxx。
fix_macdeployqt_libs() {
    local app="$1"
    local f deps d base
    for f in "$app/Contents/Frameworks"/lib*.dylib; do
        [ -e "$f" ] || continue
        deps=$(otool -L "$f" 2>/dev/null | awk 'NR>2 {print $1}')
        for d in $deps; do
            case "$d" in
                @rpath/*)
                    base="${d#@rpath/}"
                    if [ -e "$(dirname "$f")/$base" ]; then
                        install_name_tool -change "$d" "@loader_path/$base" "$f"
                    fi
                    ;;
            esac
        done
    done
}
fix_macdeployqt_libs "$QF_APP"
fix_macdeployqt_libs "$VDI_APP"

echo "==> [4.6/7] 修复 Qt 插件/QML 模块 rpath"
# 官方 Qt 构建的插件与 QML 模块自带相对 rpath（@loader_path/../../lib 等，指向 Qt 安装前缀），
# 部署到 app 后这些路径指向不存在的 Contents/lib 等位置。Qt 框架在 Contents/Frameworks，
# 统一按插件实际位置重算 @loader_path 相对 rpath（用 python3 relpath，深度不一）。
fix_qt_loader_rpath() {
    local app="$1"
    local fw="$app/Contents/Frameworks"
    local f dir rel rp
    for f in $(find "$app/Contents/PlugIns" "$app/Contents/Resources/qml" -name "*.dylib" -type f 2>/dev/null); do
        dir=$(dirname "$f")
        rel=$(python3 -c "import os,sys; print(os.path.relpath(sys.argv[1], sys.argv[2]))" "$fw" "$dir")
        for rp in $(otool -l "$f" | awk '/cmd LC_RPATH/{getline; getline; print $2}'); do
            case "$rp" in
                @loader_path/*) install_name_tool -delete_rpath "$rp" "$f" 2>/dev/null || true ;;
            esac
        done
        install_name_tool -add_rpath "@loader_path/$rel" "$f" || true
    done
}
fix_qt_loader_rpath "$QF_APP"
fix_qt_loader_rpath "$VDI_APP"

echo "==> [5/7] 校验内嵌 qf-client.app"
# qf-client.app 由构建期 POST_BUILD 嵌入 VDIClient.app/Contents/MacOS/bin/，这里只做存在性校验
if [ ! -x "$QF_APP/Contents/MacOS/qf-client" ]; then
    echo "错误: qf-client.app 未嵌入 VDIClient.app（$QF_APP）" >&2
    exit 1
fi
# VDIClient 是 Widgets 应用，不需要虚拟键盘输入法插件（其依赖的 QtVirtualKeyboard 未部署，属幽灵依赖）
rm -rf "$VDI_APP/Contents/PlugIns/platforminputcontexts"
# 两个应用均未使用 QtSql，删除 sqldrivers 插件（libqsqlodbc 等带 Homebrew libiodbc 等绝对路径依赖）
rm -rf "$QF_APP/Contents/PlugIns/sqldrivers" "$VDI_APP/Contents/PlugIns/sqldrivers"

echo "==> [6/7] ad-hoc 签名"
codesign --force --deep --sign - "$QF_APP"
codesign --force --deep --sign - "$VDI_APP"

echo "==> [7/7] 生成 DMG（只打包 VDIClient.app，不包含顶层 qf-client.app 副本）"
# 注: hdiutil create -srcfolder 需要分配 /dev/rdisk 设备（沙箱可能禁止），
#     改用 makehybrid + convert（纯文件操作，沙箱兼容），最终产物同为 UDZO 压缩 DMG。
DMG_STAGE="$ROOT/build/dmg-stage"
ISO="$ROOT/build/dmg-stage.iso"
rm -rf "$DMG_STAGE" "$ISO"
# convert 不覆盖已存在目标，重跑前先删除旧 DMG
rm -f "$ROOT/build/VDIClient-$VERSION.dmg"
mkdir -p "$DMG_STAGE"
cp -R "$VDI_APP" "$DMG_STAGE/"
ln -sf /Applications "$DMG_STAGE/Applications"
hdiutil makehybrid -o "$ISO" "$DMG_STAGE" > /dev/null
hdiutil convert "$ISO" -format UDZO -o "$ROOT/build/VDIClient-$VERSION.dmg"
rm -rf "$DMG_STAGE" "$ISO"
echo "==> 完成: build/VDIClient-$VERSION.dmg"
