# ============================================================
# vdi-client: 登录/VM 列表界面（Qt6 Widgets）
# 运行时通过 QProcess 启动同 bin 目录下的 qf-client.app
# 由顶层 qfreerdp-vdi-client/CMakeLists.txt 以 add_subdirectory 引入
# ============================================================

set(PROJECT_SOURCES
    src/main.cpp
    src/loginwindow.cpp
    src/loginwindow.h
    resources/resources.qrc
)

# macOS 应用图标（.icns 放入 Resources，Info.plist 指定，见 Qt 文档）
set(MACOSX_BUNDLE_ICON_FILE logo.icns)
set_source_files_properties(${VCI_APP_ICON_MACOS} PROPERTIES
    MACOSX_PACKAGE_LOCATION "Resources"
)

add_executable(VDIClient MACOSX_BUNDLE ${PROJECT_SOURCES} ${VCI_APP_ICON_MACOS})

# 规范 bundle 元信息（Info.plist 含 NSMicrophoneUsageDescription / NSCameraUsageDescription，
# open 启动时缺这两个键会导致 TCC 隐私违规 abort）
set_target_properties(VDIClient PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist"
    MACOSX_BUNDLE_BUNDLE_NAME "VDIClient"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.vdi.mac.VDIClient"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${VCI_VERSION}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${VCI_VERSION}"
)

target_link_libraries(VDIClient PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
)

# 确保 qf-client 先构建；把 qf-client.app 嵌入 VDIClient.app/Contents/MacOS/bin/，
# 使最终产物只有一个可见应用。运行时查找逻辑（loginwindow.cpp）优先命中 appDir/bin，
# 即 VDIClient.app/Contents/MacOS/bin/qf-client.app/Contents/MacOS/qf-client。
# 用「输出文件驱动」的 embed_qf_client 目标而非 POST_BUILD：
# POST_BUILD 只在 VDIClient 重链时执行，qf-client 单独变更（如 Info.plist）时不会同步，
# 会导致内嵌副本过期；此处依赖 qf-client 目标，任何 qf-client 重建后都会重新拷贝。
add_dependencies(VDIClient qf-client)
set(QF_EMBED_DEST "${CMAKE_BINARY_DIR}/bin/VDIClient.app/Contents/MacOS/bin/qf-client.app")
set(QF_EMBED_STAMP "${CMAKE_BINARY_DIR}/qf-embed.stamp")
add_custom_command(
    OUTPUT "${QF_EMBED_STAMP}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/bin/VDIClient.app/Contents/MacOS/bin"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${QF_EMBED_DEST}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_BINARY_DIR}/qf-staging/qf-client.app" "${QF_EMBED_DEST}"
    COMMAND ${CMAKE_COMMAND} -E touch "${QF_EMBED_STAMP}"
    DEPENDS qf-client
    COMMENT "Embedding qf-client.app into VDIClient.app/Contents/MacOS/bin"
)
add_custom_target(embed_qf_client ALL DEPENDS "${QF_EMBED_STAMP}")
add_dependencies(VDIClient embed_qf_client)
