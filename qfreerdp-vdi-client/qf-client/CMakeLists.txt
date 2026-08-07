# ============================================================
# qf-client: RDP 渲染/连接核心（Qt6 QML + FreeRDP）
# 由顶层 qfreerdp-vdi-client/CMakeLists.txt 以 add_subdirectory 引入
# ============================================================

# macOS 应用图标（.icns 放入 Resources，Info.plist 指定，见 Qt 文档）
set(MACOSX_BUNDLE_ICON_FILE logo.icns)
set_source_files_properties(${VCI_APP_ICON_MACOS} PROPERTIES
    MACOSX_PACKAGE_LOCATION "Resources"
)

qt_add_executable(qf-client MACOSX_BUNDLE
    src/mini-qf-client.cc
    src/mac_chrome.mm
    ${VCI_APP_ICON_MACOS}
)

# 内嵌 logo.png 为 qrc 资源（:/logo.png），供运行时 setWindowIcon 使用
qt_add_resources(qf-client ${VCI_LOGO_PNG})

# 注册 QML 模块（qrc:/MyTestApp/src/main.qml，与源码引用保持一致）
qt_add_qml_module(qf-client
    URI MyTestApp
    VERSION 1.0
    QML_FILES src/main.qml
    SOURCES src/rdp-view-item.h
)

# FreeRDP 3 installs headers under include/freerdp3.
target_include_directories(qf-client PRIVATE
    ${FreeRDP_INCLUDE_DIR}
    ${FreeRDP-Client_INCLUDE_DIR}
    ${WinPR_INCLUDE_DIR}
    src
)

target_link_libraries(qf-client PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::GuiPrivate
    Qt6::Qml
    Qt6::Quick
    freerdp
    freerdp-client
    winpr
    spdlog::spdlog
)

# 开发阶段：让可执行文件直接找到 install-mac 下的 dylib
# RUNTIME_OUTPUT_DIRECTORY: 不直接输出到 bin/，由 VDIClient POST_BUILD 内嵌进自身 bundle，
# 使最终构建产物只有一个可见应用（VDIClient.app）
set_target_properties(qf-client PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/qf-staging"
    BUILD_RPATH "${CMAKE_CURRENT_SOURCE_DIR}/../../freerdp-3.28.0/install-mac/lib"
    INSTALL_RPATH "@loader_path/../Frameworks;${CMAKE_CURRENT_SOURCE_DIR}/../../freerdp-3.28.0/install-mac/lib"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist"
    MACOSX_BUNDLE_BUNDLE_NAME "qf-client"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.vdi.mac.qfclient"
)
