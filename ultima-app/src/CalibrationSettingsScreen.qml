import QtQuick 2.15

// Manual calibration panel for the 360 view — opened from the gear icon in
// Camera360Screen.qml. Slides in from the right so SurroundView keeps
// rendering live underneath: every stepper edit here writes straight to
// calibrationStore (calibrationstore.h, a context property from main.cpp),
// whose calibrationChanged() signal SurroundView already listens to (see
// surroundview.h's calibrationSource property) to rebuild the affected warp
// mesh on the next frame — so a parameter change is visible immediately,
// not after a restart.
//
// Fixed calibration fields (image size, principal point, fisheye k1-k4) and
// meshGridResolution aren't exposed here — those are lens/render properties,
// not things to tune by eye from a camera mount. See calibrationstore.h.
Item {
    id: root

    // width is fixed (not anchors.fill) specifically so x stays free for
    // the slide animation below — anchors.fill also binds x, and an anchor
    // binding wins over a plain property write, which silently pinned this
    // panel at x=0 full-width (always visible) the first time this was
    // wired up. anchors.top/bottom only, no left/right/fill.
    readonly property int panelWidth: 420
    width: panelWidth
    anchors.top: parent.top
    anchors.bottom: parent.bottom
    x: parent.width
    visible: x < parent.width
    Behavior on x {
        NumberAnimation { duration: 220; easing.type: Easing.OutCubic }
    }

    function open() { x = parent.width - panelWidth }
    function close() { x = parent.width }

    // Swallow touches under the panel so a drag/tap on a row never reaches
    // whatever's behind it (SurroundView, or Camera360Screen's own
    // full-screen MouseArea).
    MouseArea {
        anchors.fill: parent
    }

    Rectangle {
        anchors.fill: parent
        color: "#0a0a0a"
        opacity: 0.94
        border.color: "#333333"
        border.width: 1
    }

    Text {
        id: header
        text: "CAMERA CALIBRATION"
        color: "white"
        font.family: bahnschriftFont.name
        font.pixelSize: 22
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.margins: 18
    }

    component CloseGlyphButton: Rectangle {
        id: closeBtn
        signal clicked()
        width: 40; height: 40; radius: 8
        color: closeArea.pressed ? "#444444" : "#222222"
        border.color: "#555555"
        border.width: 1
        Text { text: "X"; color: "white"; font.pixelSize: 18; anchors.centerIn: parent }
        MouseArea { id: closeArea; anchors.fill: parent; onClicked: closeBtn.clicked() }
    }

    CloseGlyphButton {
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 14
        onClicked: root.close()
    }

    component SectionHeader: Text {
        color: "#999999"
        font.family: bahnschriftFont.name
        font.pixelSize: 15
        topPadding: 14
        bottomPadding: 4
    }

    // One camera's 6 tunable fields — instantiated once per camera below.
    // `model` is the CalibrationCameraModel (calibrationstore.h) to bind to.
    component CameraParamGroup: Column {
        property var model
        property string title: ""
        width: parent ? parent.width : 300
        spacing: 2

        SectionHeader { text: title }
        CalibrationParamRow {
            label: "Position X (fwd)"; unit: " m"; step: 0.01; decimals: 2
            value: model.posXMeters
            onValueEdited: model.posXMeters = newValue
        }
        CalibrationParamRow {
            label: "Position Y (left)"; unit: " m"; step: 0.01; decimals: 2
            value: model.posYMeters
            onValueEdited: model.posYMeters = newValue
        }
        CalibrationParamRow {
            label: "Position Z (up)"; unit: " m"; step: 0.01; decimals: 2
            value: model.posZMeters
            onValueEdited: model.posZMeters = newValue
        }
        CalibrationParamRow {
            label: "Yaw"; unit: " deg"; step: 0.5; decimals: 1
            value: model.yawDegrees
            onValueEdited: model.yawDegrees = newValue
        }
        CalibrationParamRow {
            label: "Pitch"; unit: " deg"; step: 0.5; decimals: 1
            value: model.pitchDegrees
            onValueEdited: model.pitchDegrees = newValue
        }
        CalibrationParamRow {
            label: "FOV"; unit: " deg"; step: 0.5; decimals: 1
            value: model.fovDegrees
            onValueEdited: model.fovDegrees = newValue
        }
    }

    Flickable {
        id: scrollArea
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: buttonRow.top
        anchors.margins: 18
        anchors.topMargin: 8
        anchors.bottomMargin: 10
        contentWidth: width
        contentHeight: paramColumn.height
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: paramColumn
            width: parent.width
            spacing: 0

            CameraParamGroup { title: "FRONT CAMERA"; model: calibrationStore.front }
            CameraParamGroup { title: "REAR CAMERA"; model: calibrationStore.rear }
            CameraParamGroup { title: "LEFT CAMERA"; model: calibrationStore.left }
            CameraParamGroup { title: "RIGHT CAMERA"; model: calibrationStore.right }

            SectionHeader { text: "VEHICLE" }
            CalibrationParamRow {
                label: "Length"; unit: " m"; step: 0.01; decimals: 2
                value: calibrationStore.geometry.vehicleLengthMeters
                onValueEdited: calibrationStore.geometry.vehicleLengthMeters = newValue
            }
            CalibrationParamRow {
                label: "Width"; unit: " m"; step: 0.01; decimals: 2
                value: calibrationStore.geometry.vehicleWidthMeters
                onValueEdited: calibrationStore.geometry.vehicleWidthMeters = newValue
            }
            CalibrationParamRow {
                label: "Ground extent X"; unit: " m"; step: 0.1; decimals: 1
                value: calibrationStore.geometry.groundHalfExtentXMeters
                onValueEdited: calibrationStore.geometry.groundHalfExtentXMeters = newValue
            }
            CalibrationParamRow {
                label: "Ground extent Y"; unit: " m"; step: 0.1; decimals: 1
                value: calibrationStore.geometry.groundHalfExtentYMeters
                onValueEdited: calibrationStore.geometry.groundHalfExtentYMeters = newValue
            }
            CalibrationParamRow {
                label: "Wedge overlap"; unit: " deg"; step: 1; decimals: 0
                value: calibrationStore.geometry.wedgeOverlapDegrees
                onValueEdited: calibrationStore.geometry.wedgeOverlapDegrees = newValue
            }
        }
    }

    component PanelButton: Rectangle {
        id: panelBtn
        property alias label: labelText.text
        property bool highlighted: false
        signal clicked()

        height: 46
        radius: 8
        color: btnArea.pressed ? (highlighted ? "#e0e0e0" : "#333333")
                                : (highlighted ? "white" : "#1a1a1a")
        border.color: "#555555"
        border.width: 1

        Text {
            id: labelText
            anchors.centerIn: parent
            font.family: bahnschriftFont.name
            font.pixelSize: 16
            color: panelBtn.highlighted ? "black" : "white"
        }

        MouseArea {
            id: btnArea
            anchors.fill: parent
            onClicked: panelBtn.clicked()
        }
    }

    Row {
        id: buttonRow
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: 18
        height: 46
        spacing: 10

        PanelButton {
            width: (parent.width - 20) / 3
            height: parent.height
            label: "RESET"
            onClicked: calibrationStore.resetToDefaults()
        }
        PanelButton {
            width: (parent.width - 20) / 3
            height: parent.height
            label: "SAVE"
            highlighted: true
            onClicked: calibrationStore.save()
        }
        PanelButton {
            width: (parent.width - 20) / 3
            height: parent.height
            label: "CLOSE"
            onClicked: root.close()
        }
    }
}
