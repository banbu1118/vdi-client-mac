#!/bin/bash
# ============================================================
# 打包结果自检脚本
# 用法: ./scripts/check-bundle.sh
# 检查 build/package 中所有 Mach-O:
#   1. 无 /opt/homebrew、/opt/local、/Users 绝对路径依赖残留
#   2. 所有 @rpath/@loader_path/@executable_path 引用可解析
# ============================================================
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PKG="$ROOT/build/package"
FAILED=0

# 收集所有 Mach-O
BINS=$(find "$PKG" -type f \( -name "*.dylib" -o -name "*.so" \) -print; \
       find "$PKG" -path "*/Contents/MacOS/*" -type f -print)
BINS=$(echo "$BINS" | sort -u)

check_binary() {
    local f="$1"
    # @executable_path 基准 = 最近的 *.app/Contents/MacOS
    local exedir=""
    local dir
    dir=$(dirname "$f")
    while [ "$dir" != "/" ]; do
        if [[ "$(basename "$dir")" == *.app ]] && [ -d "$dir/Contents/MacOS" ]; then
            exedir="$dir/Contents/MacOS"
            break
        fi
        dir=$(dirname "$dir")
    done
    local deps rpaths
    # 过滤 fat binary (universal) 的架构段头行 "(architecture ...)"，避免被误判为依赖
    deps=$(otool -L "$f" 2>/dev/null | awk 'NR>2 && $0 !~ /\(architecture/ {print $1}')

    for d in $deps; do
        case "$d" in
            /opt/homebrew/*|/opt/local/*|/Users/*)
                echo "RESIDUAL: $f -> $d"
                FAILED=1
                ;;
            /System/*|/usr/lib/*)
                ;;
            @rpath/*)
                local base="${d#@rpath/}"
                local found=""
                local dir
                dir=$(dirname "$f")
                rpaths=$(otool -l "$f" 2>/dev/null | awk '/cmd LC_RPATH/{getline; getline; print $2}')
                for rp in $rpaths; do
                    local resolved="$rp"
                    case "$rp" in
                        @loader_path*) resolved="${rp/@loader_path/$dir}" ;;
                        @executable_path*) resolved="${rp/@executable_path/$exedir}" ;;
                    esac
                    if [ -e "$resolved/$base" ]; then found=1; break; fi
                    # rpath 本身可能以 @rpath 形式嵌套，跳过
                done
                if [ -z "$found" ]; then
                    echo "UNRESOLVED: $f -> @rpath/$base (rpaths=[$rpaths])"
                    FAILED=1
                fi
                ;;
            @loader_path/*|@executable_path/*)
                local base
                local dir
                dir=$(dirname "$f")
                base="${d/@loader_path/$dir}"
                base="${base/@executable_path/$exedir}"
                if [ ! -e "$base" ]; then
                    echo "MISSING: $f -> $d"
                    FAILED=1
                fi
                ;;
        esac
    done
}

for f in $BINS; do
    file "$f" 2>/dev/null | grep -q "Mach-O" || continue
    check_binary "$f"
done

if [ "$FAILED" -eq 0 ]; then
    echo "PASS: 所有依赖均可在 bundle 内解析，无绝对路径残留"
else
    echo "FAIL: 存在无法解析的依赖，见上方输出"
fi
exit $FAILED
