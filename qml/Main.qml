import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import V4L2Passthru

Window {
    id: root
    width: 1280
    height: 720
    minimumWidth: 640
    minimumHeight: 360
    visible: true
    color: "#000000"
    title: "V4L2 Passthru"

    property bool controlsVisible: true
    property bool isFullscreen: visibility === Window.FullScreen

    function revealControls() {
        controlsVisible = true
        hideTimer.restart()
    }

    function toggleFullscreen() {
        if (root.visibility === Window.FullScreen) {
            root.showNormal()
        } else {
            root.showFullScreen()
        }
        revealControls()
    }

    function indexForId(modelList, targetId) {
        if (!modelList) return 0
        for (let i = 0; i < modelList.length; ++i) {
            if (modelList[i].id === targetId) return i
        }
        return 0
    }

    Timer {
        id: hideTimer
        interval: 2500
        running: CaptureController.streaming && !overlayBar.hovered && !settingsDrawer.visible
        onTriggered: {
            if (CaptureController.streaming && !overlayBar.hovered && !settingsDrawer.visible) {
                root.controlsVisible = false
            }
        }
    }

    VideoOutput {
        id: videoOutput
        anchors.fill: parent
        fillMode: VideoOutput.PreserveAspectFit
    }

    Component.onCompleted: {
        CaptureController.videoSink = videoOutput.videoSink
        CaptureController.start()
    }

    // Track cursor motion across the whole viewport to wake the auto-hiding HUD.
    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton
        cursorShape: (!root.controlsVisible && CaptureController.streaming) ? Qt.BlankCursor : Qt.ArrowCursor
        onPositionChanged: root.revealControls()
        onClicked: root.revealControls()
        onDoubleClicked: root.toggleFullscreen()
    }

    // Keyboard shortcuts for quick play control.
    Item {
        id: keyHandler
        anchors.fill: parent
        focus: true
        Keys.onPressed: (event) => {
            if (event.key === Qt.Key_F11) {
                root.toggleFullscreen()
                event.accepted = true
            } else if (event.key === Qt.Key_Escape && root.isFullscreen) {
                root.showNormal()
                event.accepted = true
            } else if (event.key === Qt.Key_M) {
                CaptureController.muted = !CaptureController.muted
                root.revealControls()
                event.accepted = true
            } else if (event.key === Qt.Key_R) {
                CaptureController.reconnectNow()
                root.revealControls()
                event.accepted = true
            } else if (event.key === Qt.Key_S) {
                settingsDrawer.visible = !settingsDrawer.visible
                root.revealControls()
                event.accepted = true
            }
        }
    }

    // Center status card (only shown when not actively receiving video).
    Rectangle {
        anchors.centerIn: parent
        visible: !CaptureController.streaming
        width: Math.min(parent.width - 48, 460)
        implicitHeight: statusCol.implicitHeight + 36
        radius: 12
        color: "#D914161B"
        border.color: CaptureController.reconnecting ? "#E0A82E" : "#3A3F4B"
        border.width: 1

        ColumnLayout {
            id: statusCol
            anchors.centerIn: parent
            width: parent.width - 40
            spacing: 10

            RowLayout {
                Layout.alignment: Qt.AlignHCenter
                spacing: 10

                BusyIndicator {
                    running: CaptureController.reconnecting || CaptureController.statusText === "Connecting..."
                    visible: running
                    implicitWidth: 22
                    implicitHeight: 22
                }

                Label {
                    text: CaptureController.statusText
                    color: "#F3F4F6"
                    font.pixelSize: 17
                    font.weight: Font.DemiBold
                }
            }

            Label {
                visible: CaptureController.errorDetail.length > 0
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: CaptureController.errorDetail
                color: "#9CA3AF"
                font.pixelSize: 12
            }

            RowLayout {
                Layout.alignment: Qt.AlignHCenter
                spacing: 10

                Button {
                    text: "Retry Now"
                    onClicked: CaptureController.reconnectNow()
                }
                Button {
                    text: "Devices"
                    onClicked: {
                        CaptureController.refreshDevices()
                        settingsDrawer.visible = !settingsDrawer.visible
                    }
                }
            }
        }
    }

    // Top floating HUD bar (auto-hides during gameplay).
    Rectangle {
        id: overlayBar
        property bool hovered: barHover.hovered || formatBox.popup.visible || fpsBox.popup.visible

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: 12
        height: 48
        radius: 10
        color: "#D914161B"
        border.color: "#2C313C"
        border.width: 1

        opacity: (root.controlsVisible || !CaptureController.streaming || settingsDrawer.visible) ? 1.0 : 0.0
        visible: opacity > 0.01
        Behavior on opacity { NumberAnimation { duration: 160 } }

        HoverHandler {
            id: barHover
            onHoveredChanged: {
                if (hovered) hideTimer.stop()
                else hideTimer.restart()
            }
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            spacing: 10

            // Live status dot + telemetry
            Rectangle {
                width: 10
                height: 10
                radius: 5
                color: CaptureController.streaming
                    ? "#22C55E"
                    : (CaptureController.reconnecting ? "#F59E0B" : "#EF4444")
            }

            Label {
                text: CaptureController.streaming
                    ? "%1 • %2 fps • %3".arg(CaptureController.activeResolution)
                                         .arg(CaptureController.measuredFps.toFixed(1))
                                         .arg(CaptureController.pixelFormat)
                    : CaptureController.statusText
                color: "#E5E7EB"
                font.pixelSize: 13
                font.weight: Font.Medium
            }

            Item { Layout.fillWidth: true }

            ComboBox {
                id: formatBox
                implicitWidth: 96
                model: ["YUYV", "MJPG"]
                currentIndex: CaptureController.pixelFormat === "MJPG" ? 1 : 0
                onActivated: (idx) => {
                    CaptureController.pixelFormat = model[idx]
                    root.revealControls()
                }
                ToolTip.visible: hovered
                ToolTip.text: "YUYV = zero decode latency; MJPG = lower USB bandwidth"
            }

            ComboBox {
                id: fpsBox
                implicitWidth: 88
                model: [60, 30]
                displayText: currentText + " fps"
                currentIndex: CaptureController.fps === 30 ? 1 : 0
                delegate: ItemDelegate {
                    width: fpsBox.width
                    text: modelData + " fps"
                    highlighted: fpsBox.highlightedIndex === index
                }
                onActivated: (idx) => {
                    CaptureController.fps = model[idx]
                    root.revealControls()
                }
            }

            Button {
                text: CaptureController.muted ? "Unmute (M)" : "Mute (M)"
                checked: CaptureController.muted
                checkable: true
                onClicked: {
                    CaptureController.muted = !CaptureController.muted
                    root.revealControls()
                }
            }

            Slider {
                id: volSlider
                implicitWidth: 95
                from: 0.0
                to: 1.5
                stepSize: 0.05
                value: CaptureController.volume
                enabled: CaptureController.audioEnabled && !CaptureController.muted
                onMoved: {
                    CaptureController.volume = value
                    root.revealControls()
                }
                ToolTip.visible: pressed || hovered
                ToolTip.text: "Volume: " + Math.round(value * 100) + "%"
            }

            Button {
                text: "Reconnect"
                onClicked: {
                    CaptureController.reconnectNow()
                    root.revealControls()
                }
            }

            Button {
                text: settingsDrawer.visible ? "Hide Setup" : "Setup"
                onClicked: {
                    if (!settingsDrawer.visible) {
                        CaptureController.refreshDevices()
                    }
                    settingsDrawer.visible = !settingsDrawer.visible
                    root.revealControls()
                }
            }

            Button {
                text: root.isFullscreen ? "Exit Full" : "Fullscreen"
                onClicked: root.toggleFullscreen()
            }
        }
    }

    // Compact device & recovery drawer.
    Rectangle {
        id: settingsDrawer
        visible: false
        anchors.top: overlayBar.bottom
        anchors.right: overlayBar.right
        anchors.topMargin: 8
        width: 400
        implicitHeight: drawerCol.implicitHeight + 28
        radius: 10
        color: "#EB14161B"
        border.color: "#2C313C"
        border.width: 1

        ColumnLayout {
            id: drawerCol
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 14
            spacing: 10

            RowLayout {
                Layout.fillWidth: true
                Label {
                    text: "Device & Recovery Settings"
                    color: "#F3F4F6"
                    font.pixelSize: 14
                    font.weight: Font.DemiBold
                }
                Item { Layout.fillWidth: true }
                Button {
                    text: "Refresh"
                    onClicked: CaptureController.refreshDevices()
                }
            }

            Label {
                text: "Video Capture Device"
                color: "#9CA3AF"
                font.pixelSize: 12
            }
            ComboBox {
                id: videoDevBox
                Layout.fillWidth: true
                model: CaptureController.videoDevices
                textRole: "label"
                valueRole: "id"
                currentIndex: root.indexForId(CaptureController.videoDevices, CaptureController.selectedVideoDevice)
                onActivated: (idx) => {
                    CaptureController.selectedVideoDevice = CaptureController.videoDevices[idx].id
                }
            }

            Label {
                text: "Audio Source (PipeWire / PulseAudio)"
                color: "#9CA3AF"
                font.pixelSize: 12
            }
            ComboBox {
                id: audioSrcBox
                Layout.fillWidth: true
                model: CaptureController.audioSources
                textRole: "label"
                valueRole: "id"
                currentIndex: root.indexForId(CaptureController.audioSources, CaptureController.selectedAudioSource)
                onActivated: (idx) => {
                    CaptureController.selectedAudioSource = CaptureController.audioSources[idx].id
                }
            }

            Label {
                Layout.fillWidth: true
                elide: Text.ElideRight
                text: CaptureController.audioStatus
                color: CaptureController.audioStatus.startsWith("Audio error") ? "#F59E0B" : "#9CA3AF"
                font.pixelSize: 12
            }

            RowLayout {
                Layout.fillWidth: true
                CheckBox {
                    text: "Audio Passthrough"
                    checked: CaptureController.audioEnabled
                    onToggled: CaptureController.audioEnabled = checked
                }
                Item { Layout.fillWidth: true }
                CheckBox {
                    text: "Auto-Reconnect"
                    checked: CaptureController.autoReconnect
                    onToggled: CaptureController.autoReconnect = checked
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    text: "Recoveries this session: " + CaptureController.reconnectCount
                    color: "#9CA3AF"
                    font.pixelSize: 12
                }
                Item { Layout.fillWidth: true }
                Button {
                    text: CaptureController.streaming || CaptureController.reconnecting ? "Stop" : "Start"
                    onClicked: {
                        if (CaptureController.streaming || CaptureController.reconnecting) {
                            CaptureController.stop()
                        } else {
                            CaptureController.start()
                        }
                    }
                }
            }
        }
    }
}
