import QtQuick
import QtQuick.Controls
import MyTestApp

Window {
    id: root
    visible: true
    title: qsTr("VDI Client")

    color: "black"
    minimumWidth: 650
    minimumHeight: 550

    // ========== 初始状态：全屏窗口 ==========
    visibility: Window.FullScreen

    property bool isFullscreen: true

    Component.onCompleted: {
        root.showFullScreen()
        // 全屏时隐藏 macOS 菜单栏与 Dock，只显示 RDP 画面
        rdpItem.setHideMenuBarDock(true)

        // 窗口创建后延迟到下一帧启动连接，确保 scene graph 已设置好 surface
        Qt.callLater(function() {
            rdpItem.startConnection()
        })
    }

    function toggleDisplayMode() {
        if (isFullscreen) {
            exitFullscreen()
        } else {
            enterFullscreen()
        }
    }

    function enterFullscreen() {
        isFullscreen = true
        rdpItem.setHideMenuBarDock(true)
        root.showFullScreen()
    }

    function exitFullscreen() {
        isFullscreen = false
        // 恢复 macOS 菜单栏与 Dock
        rdpItem.setHideMenuBarDock(false)
        // 直接进入普通大窗口，不经过最大化状态
        root.showNormal()
        const w = Math.round(Screen.desktopAvailableWidth * 0.80)
        const h = Math.round(Screen.desktopAvailableHeight * 0.80)
        root.width = w
        root.height = h
        // 水平居中显示，垂直略微偏上（留出 Dock 空间）
        root.x = Math.round((Screen.desktopAvailableWidth - w) / 2) + Screen.virtualX
        root.y = Math.round((Screen.desktopAvailableHeight - h) / 4) + Screen.virtualY
    }

    // ========== RDP 渲染区域（始终填满窗口，与工具栏分离） ==========
    RdpViewItem {
        id: rdpItem
        objectName: "rdpViewItem"
        anchors.fill: parent
        focus: true
        fullscreen: root.isFullscreen
        onWidthChanged: {
            rdpItem.notifyWindowResized()
        }
        onHeightChanged: {
            rdpItem.notifyWindowResized()
        }
    }

    // ========== Overlay 悬浮工具栏（独立，不影响 RDP 渲染区域） ==========

    // 屏幕顶部 5px 热区 — 仅检测鼠标进入，不阻挡 RDP 交互
    MouseArea {
        id: topHotZone
        anchors.horizontalCenter: parent.horizontalCenter
        width: 380
        anchors.top: parent.top
        height: 5
        hoverEnabled: true
        propagateComposedEvents: true
        z: toolbar.z + 1

        onContainsMouseChanged: {
            if (containsMouse) {
                hideDelayTimer.stop()
                toolbar.visible = true
                toolbar.y = toolbar.shownY
            } else {
                hideDelayTimer.restart()
            }
        }
        onPressed: mouse.accepted = false
        onReleased: mouse.accepted = false
        onClicked: mouse.accepted = false
        onDoubleClicked: mouse.accepted = false
        onPressAndHold: mouse.accepted = false
        onWheel: wheel.accepted = false
    }

    // 悬浮工具栏 — 固定在屏幕顶部中央
    Rectangle {
        id: toolbar
        width: 340
        height: 36
        radius: 6
        anchors.horizontalCenter: parent.horizontalCenter
        y: hiddenY
        z: 100
        visible: false
        color: "#3d3d3d"
        opacity: visible ? 0.93 : 0

        property int hiddenY: -height - 4
        property int shownY: 6
        property bool pinned: false

        Behavior on y {
            NumberAnimation { duration: 180; easing.type: Easing.OutCubic }
        }

        MouseArea {
            id: toolbarHover
            anchors.fill: parent
            hoverEnabled: true

            onContainsMouseChanged: {
                if (toolbar.pinned) return
                if (containsMouse) {
                    hideDelayTimer.stop()
                } else {
                    hideDelayTimer.restart()
                }
            }
        }

        Text {
            id: toolbarTitle
            anchors.left: parent.left
            anchors.leftMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            text: "VDI Client"
            color: "#d0d0d0"
            font.pixelSize: 12
        }

        Row {
            anchors.right: parent.right
            anchors.rightMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            spacing: 2

            ToolButton {
                property bool pinState: toolbar.pinned
                highlighted: pinState
                onClicked: toolbar.pinned = !toolbar.pinned

                Item {
                    anchors.centerIn: parent
                    width: 14; height: 16

                    Rectangle {
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.top: parent.top
                        width: 8; height: 8; radius: 4
                        color: parent.parent.highlighted ? "#ffffff" : "#999999"
                    }
                    Rectangle {
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.top: parent.top
                        anchors.topMargin: 4
                        width: 2.5; height: 10
                        radius: 1
                        color: parent.parent.highlighted ? "#ffffff" : "#999999"
                    }
                    Canvas {
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.bottom: parent.bottom
                        width: 6; height: 4
                        onPaint: {
                            var ctx = getContext("2d")
                            ctx.fillStyle = parent.parent.highlighted ? "#ffffff" : "#999999"
                            ctx.beginPath()
                            ctx.moveTo(1.5, 0)
                            ctx.lineTo(4.5, 0)
                            ctx.lineTo(3, 4)
                            ctx.closePath()
                            ctx.fill()
                        }
                    }
                }
            }
            ToolButton { text: "CAD"; hint: "Ctrl+Alt+Del"; onClicked: rdpItem.sendCtrlAltDelete() }

            ToolButton { text: "\u2013";  onClicked: root.visibility = Window.Minimized }
            ToolButton {
                onClicked: root.toggleDisplayMode()

                Item {
                    anchors.centerIn: parent
                    width: 12; height: 10

                    Loader {
                        anchors.fill: parent
                        active: root.isFullscreen
                        sourceComponent: Item {
                            anchors.fill: parent
                            Rectangle {
                                x: 0; y: 3
                                width: 9; height: 7
                                color: "transparent"
                                border.color: "#aaaaaa"
                                border.width: 1.2
                            }
                            Rectangle {
                                x: 3; y: 0
                                width: 9; height: 7
                                color: "#3d3d3d"
                                border.color: "#aaaaaa"
                                border.width: 1.2
                            }
                        }
                    }
                    Loader {
                        anchors.fill: parent
                        active: !root.isFullscreen
                        sourceComponent: Rectangle {
                            anchors.centerIn: parent
                            width: 12; height: 10
                            color: "transparent"
                            border.color: "#aaaaaa"
                            border.width: 1.2
                        }
                    }
                }
            }
            ToolButton { text: "\u2715"; isClose: true; onClicked: Qt.quit() }
        }
    }

    // ========== 延时隐藏定时器 ==========
    Timer {
        id: hideDelayTimer
        interval: 3000
        onTriggered: {
            if (toolbar.pinned) return
            toolbar.y = toolbar.hiddenY
            toolbar.visible = false
        }
    }

    // ========== 工具栏按钮组件 ==========
    component ToolButton: Rectangle {
        property string text: ""
        property string hint: ""
        property bool isClose: false
        property bool highlighted: false
        signal clicked()

        width: 32; height: 26; radius: 4
        color: highlighted ? "#1a5fb4" : (btnMouse.containsMouse ? (isClose ? "#e81123" : "#5a5a5a") : "transparent")

        Text {
            anchors.centerIn: parent
            text: parent.text
            color: "white"
            font.pixelSize: 13
        }
        MouseArea {
            id: btnMouse
            anchors.fill: parent
            hoverEnabled: true
            onClicked: parent.clicked()
        }
    }
}
