cmake_minimum_required(VERSION 3.16)

# ============================================================
# qfreerdp-vdi-client: VDIClient(登录/VM列表, Qt Widgets)
#                      + qf-client(RDP 渲染/连接核心, Qt QML + FreeRDP)
# 合并自独立项目 vdi-client-mac 与 qfreerdp-mac，一次构建产出两个 .app
# ============================================================
project(VDIClientSuite VERSION 1.5.0 LANGUAGES C CXX)

# 支持 macOS 13+：.deps 自编译库与 Qt 均按 13.0 构建。
# 不显式设置会默认跟随当前系统 minos（如 macOS 26），导致在旧系统上无法加载。
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0")

set(VCI_VERSION "${PROJECT_VERSION}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 自动处理 Qt 元对象/资源/uic
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 使用自编译的 FreeRDP 3.28.0 (macOS arm64)，位于仓库根级
list(PREPEND CMAKE_PREFIX_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../freerdp-3.28.0/install-mac")

# 统一查找依赖（两个子 target 共享）
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network Gui Qml Quick GuiPrivate)
find_package(FreeRDP 3 REQUIRED)
find_package(WinPR 3 REQUIRED)
find_package(FreeRDP-Client 3 REQUIRED)
find_package(spdlog REQUIRED)
message(STATUS "FreeRDP_VERSION: ${FreeRDP_VERSION}")

# 共享应用图标（.icns 决定 Finder/Dock 图标，logo.png 内嵌为运行时图标）
set(VCI_APP_ICON_MACOS "${CMAKE_CURRENT_SOURCE_DIR}/logo.icns")
set(VCI_LOGO_PNG "${CMAKE_CURRENT_SOURCE_DIR}/logo.png")

# 两个 .app bundle 统一输出到 build/bin，供运行时 bin 查找逻辑使用
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

add_subdirectory(qf-client)
add_subdirectory(vdi-client)
