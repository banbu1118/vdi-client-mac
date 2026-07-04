cmake_minimum_required(VERSION 3.16)

project(VDIClient VERSION 1.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS version")

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network)

set(PROJECT_SOURCES
    src/main.cpp
    src/loginwindow.cpp
    src/loginwindow.h
    resources/resources.qrc
)

add_executable(${PROJECT_NAME} MACOSX_BUNDLE ${PROJECT_SOURCES})

target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
)

set_target_properties(${PROJECT_NAME} PROPERTIES
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.vdi.client"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}"
    MACOSX_BUNDLE_ICON_FILE "AppIcon.icns"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/VDIClient-Info.plist.in"
)

# ============================================================
# App Icon — 从 desktop-1024x1024.png 生成 AppIcon.icns
# ============================================================
set(APP_ICNS "${CMAKE_SOURCE_DIR}/AppIcon.icns")
set(ICON_PNG "${CMAKE_SOURCE_DIR}/desktop-1024x1024.png")
set(ICONSET_DIR "${CMAKE_BINARY_DIR}/AppIcon.iconset")

if(EXISTS "${ICON_PNG}" AND NOT EXISTS "${APP_ICNS}")
    file(MAKE_DIRECTORY "${ICONSET_DIR}")

    # 用 sips 缩放到各标准尺寸
    set(ICON_SIZES
        icon_16x16.png:16
        icon_16x16@2x.png:32
        icon_32x32.png:32
        icon_32x32@2x.png:64
        icon_128x128.png:128
        icon_128x128@2x.png:256
        icon_256x256.png:256
        icon_256x256@2x.png:512
        icon_512x512.png:512
        icon_512x512@2x.png:1024
    )
    foreach(entry ${ICON_SIZES})
        string(REPLACE ":" ";" parts "${entry}")
        list(GET parts 0 name)
        list(GET parts 1 size)
        execute_process(
            COMMAND sips -z ${size} ${size} "${ICON_PNG}" --out "${ICONSET_DIR}/${name}"
            OUTPUT_QUIET
        )
    endforeach()

    # 用 iconutil 打包为 icns
    execute_process(
        COMMAND iconutil -c icns "${ICONSET_DIR}" -o "${APP_ICNS}"
    )
endif()

# ============================================================
# Copy sdl3-freerdp 到 app bundle Resources
# 目录结构: Resources/sdl3-freerdp.app/Contents/{MacOS/,Frameworks/}
# ============================================================
if(EXISTS "${CMAKE_SOURCE_DIR}/../sdl3-freerdp")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        # 1. 复制 sdl3-freerdp 二进制和框架
        COMMAND ditto --noextattr "${CMAKE_SOURCE_DIR}/../sdl3-freerdp"
            "$<TARGET_BUNDLE_CONTENT_DIR:${PROJECT_NAME}>/Resources/sdl3-freerdp.app"
        # 2. 创建 Resources 目录并复制 AppIcon.icns
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_BUNDLE_CONTENT_DIR:${PROJECT_NAME}>/Resources/sdl3-freerdp.app/Contents/Resources"
        COMMAND ${CMAKE_COMMAND} -E copy "${APP_ICNS}"
            "$<TARGET_BUNDLE_CONTENT_DIR:${PROJECT_NAME}>/Resources/sdl3-freerdp.app/Contents/Resources/AppIcon.icns"
        # 3. 创建 Info.plist（使 macOS 能识别此 bundle 的图标）
        COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_SOURCE_DIR}/freerdp-Info.plist"
            "$<TARGET_BUNDLE_CONTENT_DIR:${PROJECT_NAME}>/Resources/sdl3-freerdp.app/Contents/Info.plist"
        # 4. Ad-hoc codesign sdl3-freerdp 二进制
        COMMAND codesign --force --sign - "$<TARGET_BUNDLE_CONTENT_DIR:${PROJECT_NAME}>/Resources/sdl3-freerdp.app/Contents/MacOS/sdl3-freerdp"
        COMMAND codesign --force --sign - "$<TARGET_BUNDLE_CONTENT_DIR:${PROJECT_NAME}>/Resources/sdl3-freerdp.app/Contents/MacOS/sfreerdp"
        COMMENT "Copying sdl3-freerdp (no xattrs), icons and ad-hoc codesigning"
    )
endif()

# ============================================================
# 复制 AppIcon.icns 到 VDI Client 自己的 Resources
# ============================================================
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy "${APP_ICNS}"
        "$<TARGET_BUNDLE_CONTENT_DIR:${PROJECT_NAME}>/Resources/AppIcon.icns"
    COMMENT "Copying AppIcon.icns to VDI Client Resources"
)

# ============================================================
# package_dmg — 生成可分发的 .dmg 安装包
# 步骤: 副本 → macdeployqt6 → codesign → hdiutil
# ============================================================
find_program(MACDEPLOYQT macdeployqt6
    PATHS /usr/local/bin /opt/homebrew/bin
)

set(DMG_NAME "${PROJECT_NAME}-${PROJECT_VERSION}-macOS.dmg")
set(DMG_STAGING "${CMAKE_BINARY_DIR}/dmg-staging")
set(DMG_STAGING_APP "${DMG_STAGING}/VDIClient.app")

add_custom_target(package_dmg
    COMMAND ${CMAKE_COMMAND} -E remove_directory "${DMG_STAGING}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DMG_STAGING}"
    COMMENT "Packaging VDIClient.dmg ..."
)

# Step 1: 复制 .app 到暂存目录
add_custom_command(TARGET package_dmg PRE_BUILD
    COMMAND ditto --noextattr "$<TARGET_BUNDLE_DIR:${PROJECT_NAME}>" "${DMG_STAGING_APP}"
    COMMENT "1/4  Copying .app to staging..."
)

# Step 2: 运行 macdeployqt6 部署 Qt 框架（只在暂存副本上执行）
if(MACDEPLOYQT)
    add_custom_command(TARGET package_dmg PRE_BUILD
        COMMAND "${MACDEPLOYQT}" "${DMG_STAGING_APP}"
            -always-overwrite -verbose=1
        COMMENT "2/4  Deploying Qt frameworks with macdeployqt6..."
    )
endif()

# Step 3: Ad-hoc codesign 整个 bundle
add_custom_command(TARGET package_dmg PRE_BUILD
    COMMAND codesign --force --sign - --deep "${DMG_STAGING_APP}/Contents/Resources/sdl3-freerdp.app/Contents/MacOS/sdl3-freerdp"
    COMMAND codesign --force --sign - --deep "${DMG_STAGING_APP}/Contents/Resources/sdl3-freerdp.app"
    COMMAND codesign --force --sign - --deep "${DMG_STAGING_APP}"
    COMMENT "3/4  Code-signing bundle..."
)

# Step 4: 创建 DMG
add_custom_command(TARGET package_dmg PRE_BUILD
    COMMAND ln -sfn /Applications "${DMG_STAGING}/Applications"
    COMMAND hdiutil create -ov -fs HFS+ -srcfolder "${DMG_STAGING}"
        -volname "${PROJECT_NAME}"
        -format UDZO -imagekey zlib-level=9
        "${CMAKE_BINARY_DIR}/${DMG_NAME}"
    COMMAND ${CMAKE_COMMAND} -E remove_directory "${DMG_STAGING}"
    COMMENT "4/4  Creating DMG..."
)

# 显示完成消息
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "============================================"
    COMMAND ${CMAKE_COMMAND} -E echo " Build complete!"
    COMMAND ${CMAKE_COMMAND} -E echo " To create a distributable DMG, run:"
    COMMAND ${CMAKE_COMMAND} -E echo "   cmake --build . --target package_dmg"
    COMMAND ${CMAKE_COMMAND} -E echo "============================================"
    COMMAND ${CMAKE_COMMAND} -E echo ""
)
